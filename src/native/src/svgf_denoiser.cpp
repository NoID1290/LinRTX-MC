#include "svgf_denoiser.hpp"
#include "native_log.hpp"
#include "svgf_atrous_spv.h"
#include "svgf_temporal_spv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace linrtx {
namespace {

struct alignas(4) TemporalPush {
    uint32_t width;
    uint32_t height;
    uint32_t resetHistory;
    uint32_t hasAlbedo;
    uint32_t hasNormals;
    uint32_t normalsPacked01;
    uint32_t demodulateAlbedo;
    uint32_t inputIsSrgb;
    uint32_t reverseZ;
    float nearPlane;
    float farPlane;
    float temporalBlend;
    float strength;
};
static_assert(sizeof(TemporalPush) == 52);

struct alignas(4) AtrousPush {
    uint32_t width;
    uint32_t height;
    int32_t stepSize;
    float phiColor;
    float phiNormal;
    float phiDepth;
    uint32_t hasAlbedo;
    uint32_t hasNormals;
    uint32_t normalsPacked01;
    uint32_t isFinalPass;
    uint32_t remodulateAlbedo;
    uint32_t preserveAlpha;
    uint32_t outputToSrgb;
    uint32_t reverseZ;
    float nearPlane;
    float farPlane;
};
static_assert(sizeof(AtrousPush) == 64);

void barrierReadOnly(VkNgxContext& ctx, VkCommandBuffer cmd, SharedTexture& tex) {
    ctx.cmdBarrier(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void barrierWriteGeneral(VkNgxContext& ctx, VkCommandBuffer cmd, SharedTexture& tex) {
    ctx.cmdBarrier(cmd, tex, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

} // namespace

bool SvgfDenoiser::HistoryBank::ensureSize(VkNgxContext& ctx, uint32_t w, uint32_t h) {
    if (width == w && height == h && pingA.valid() && pingB.valid() &&
        histColor[0].valid() && histColor[1].valid() &&
        histMoments[0].valid() && histMoments[1].valid()) {
        return true;
    }
    destroy(ctx);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!ctx.createSharedTexture(pingA, w, h, fmt, usage, 0, false) ||
        !ctx.createSharedTexture(pingB, w, h, fmt, usage, 0, false) ||
        !ctx.createSharedTexture(histColor[0], w, h, fmt, usage, 0, false) ||
        !ctx.createSharedTexture(histColor[1], w, h, fmt, usage, 0, false) ||
        !ctx.createSharedTexture(histMoments[0], w, h, fmt, usage, 0, false) ||
        !ctx.createSharedTexture(histMoments[1], w, h, fmt, usage, 0, false)) {
        destroy(ctx);
        return false;
    }
    width = w;
    height = h;
    currIdx = 0;
    validHistory = false;
    return true;
}

void SvgfDenoiser::HistoryBank::destroy(VkNgxContext& ctx) {
    ctx.destroySharedTexture(pingA);
    ctx.destroySharedTexture(pingB);
    ctx.destroySharedTexture(histColor[0]);
    ctx.destroySharedTexture(histColor[1]);
    ctx.destroySharedTexture(histMoments[0]);
    ctx.destroySharedTexture(histMoments[1]);
    width = 0;
    height = 0;
    currIdx = 0;
    validHistory = false;
}

bool SvgfDenoiser::init(VkNgxContext& ctx) {
    if (!ctx.vkReady) return false;
    if (initialized_) return true;

    if (!temporalPass_.initMulti(ctx.device, kSvgfTemporalSpv, sizeof(kSvgfTemporalSpv),
                                 7u, 2u, sizeof(TemporalPush))) {
        logNative("svgf: failed to create temporal compute pipeline");
        return false;
    }
    if (!atrousPass_.initMulti(ctx.device, kSvgfAtrousSpv, sizeof(kSvgfAtrousSpv),
                               5u, 1u, sizeof(AtrousPush))) {
        temporalPass_.destroy(ctx.device);
        logNative("svgf: failed to create atrous compute pipeline");
        return false;
    }

    const VkImageUsageFlags fbUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (!ctx.createSharedTexture(fallbackTex_, 4u, 4u, VK_FORMAT_R16G16B16A16_SFLOAT, fbUsage, 0, false)) {
        destroy(ctx);
        return false;
    }

    initialized_ = true;
    logNative("svgf: initialized temporal + atrous compute pipelines");
    return true;
}

void SvgfDenoiser::destroy(VkNgxContext& ctx) {
    if (ctx.device != VK_NULL_HANDLE) {
        temporalPass_.destroy(ctx.device);
        atrousPass_.destroy(ctx.device);
    }
    ctx.destroySharedTexture(fallbackTex_);
    banks_[0].destroy(ctx);
    banks_[1].destroy(ctx);
    initialized_ = false;
}

void SvgfDenoiser::resetHistory(uint32_t bankIndex) {
    if (bankIndex < 2u) {
        banks_[bankIndex].validHistory = false;
    }
}

bool SvgfDenoiser::evaluate(VkNgxContext& ctx,
                            VkCommandBuffer cmd,
                            const Params& params,
                            SharedTexture& colorIn,
                            SharedTexture& depthIn,
                            SharedTexture& mvecIn,
                            SharedTexture* albedoOpt,
                            SharedTexture* normalsOpt,
                            SharedTexture& colorOut,
                            std::string& outError) {
    if (!ctx.vkReady || params.width == 0 || params.height == 0) {
        outError = "SVGF denoiser invalid context or dimensions";
        return false;
    }
    if (!initialized_ && !init(ctx)) {
        outError = "SVGF denoiser pipeline init failed";
        return false;
    }

    HistoryBank& bank = banks_[params.bankIndex < 2u ? params.bankIndex : 0u];
    const bool freshlyAllocated = (bank.width != params.width || bank.height != params.height);
    if (!bank.ensureSize(ctx, params.width, params.height)) {
        outError = "SVGF failed to allocate history textures";
        return false;
    }

    const uint32_t prevIdx = bank.currIdx;
    const uint32_t nextIdx = (bank.currIdx + 1u) & 1u;
    const bool reset = params.resetHistory || freshlyAllocated || !bank.validHistory;

    if (fallbackTex_.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrierReadOnly(ctx, cmd, fallbackTex_);
    }
    if (reset) {
        // Ensure prior history images have a valid layout before first sample even when reset=1
        barrierReadOnly(ctx, cmd, bank.histColor[prevIdx]);
        barrierReadOnly(ctx, cmd, bank.histMoments[prevIdx]);
    }

    SharedTexture& albedoTex = (albedoOpt && albedoOpt->valid()) ? *albedoOpt : fallbackTex_;
    SharedTexture& normalsTex = (normalsOpt && normalsOpt->valid()) ? *normalsOpt : fallbackTex_;
    const bool hasAlbedo = (albedoOpt && albedoOpt->valid() && params.hasAlbedo);
    const bool hasNormals = (normalsOpt && normalsOpt->valid() && params.hasNormals);

    barrierReadOnly(ctx, cmd, colorIn);
    barrierReadOnly(ctx, cmd, depthIn);
    barrierReadOnly(ctx, cmd, mvecIn);
    if (hasAlbedo) barrierReadOnly(ctx, cmd, albedoTex);
    if (hasNormals) barrierReadOnly(ctx, cmd, normalsTex);
    if (!reset) {
        barrierReadOnly(ctx, cmd, bank.histColor[prevIdx]);
        barrierReadOnly(ctx, cmd, bank.histMoments[prevIdx]);
    }

    const uint32_t passes = std::clamp(params.atrousPasses, 1u, 5u);
    SharedTexture& tempOutIrr = (passes == 1u) ? bank.histColor[nextIdx] : bank.pingA;
    SharedTexture& tempOutMom = bank.histMoments[nextIdx];
    barrierWriteGeneral(ctx, cmd, tempOutIrr);
    barrierWriteGeneral(ctx, cmd, tempOutMom);

    TemporalPush tpc{};
    tpc.width = params.width;
    tpc.height = params.height;
    tpc.resetHistory = reset ? 1u : 0u;
    tpc.hasAlbedo = hasAlbedo ? 1u : 0u;
    tpc.hasNormals = hasNormals ? 1u : 0u;
    tpc.normalsPacked01 = params.normalsPacked01 ? 1u : 0u;
    tpc.demodulateAlbedo = (hasAlbedo && params.demodulateAlbedo) ? 1u : 0u;
    tpc.inputIsSrgb = params.inputIsSrgb ? 1u : 0u;
    tpc.reverseZ = params.reverseZ ? 1u : 0u;
    tpc.nearPlane = params.nearPlane > 0.0f ? params.nearPlane : 0.05f;
    tpc.farPlane = params.farPlane > tpc.nearPlane ? params.farPlane : 256.0f;
    tpc.temporalBlend = std::clamp(params.temporalBlend, 0.0f, 0.96f);
    tpc.strength = std::clamp(params.strength, 0.25f, 2.5f);

    const SharedTexture* tSamplers[7] = {
        &colorIn,
        &depthIn,
        &mvecIn,
        &albedoTex,
        &normalsTex,
        &bank.histColor[prevIdx],
        &bank.histMoments[prevIdx],
    };
    const SharedTexture* tStorages[2] = {&tempOutIrr, &tempOutMom};
    temporalPass_.dispatchMulti(cmd, ctx.device, tSamplers, 7u, tStorages, 2u,
                                params.width, params.height, &tpc, sizeof(tpc));

    const float s = std::clamp(params.strength, 0.25f, 2.5f);
    const float phiColorBase = 2.6f * s;
    const float phiNormal = 48.0f / std::max(0.5f, std::sqrt(s));
    const float phiDepth = 1.8f;

    SharedTexture* currSrc = &tempOutIrr;
    for (uint32_t p = 0; p < passes; ++p) {
        const bool isFinal = (p + 1u == passes);
        SharedTexture* currDst = nullptr;
        if (isFinal) {
            currDst = &colorOut;
        } else if (p == 0u) {
            // Feed first spatially stabilized iteration back into temporal history
            currDst = &bank.histColor[nextIdx];
        } else {
            currDst = (currSrc == &bank.pingB) ? &bank.pingA : &bank.pingB;
        }

        barrierReadOnly(ctx, cmd, *currSrc);
        barrierWriteGeneral(ctx, cmd, *currDst);

        AtrousPush apc{};
        apc.width = params.width;
        apc.height = params.height;
        apc.stepSize = 1 << p;
        apc.phiColor = phiColorBase * ((p == 0u) ? 1.15f : 1.0f);
        apc.phiNormal = phiNormal;
        apc.phiDepth = phiDepth;
        apc.hasAlbedo = hasAlbedo ? 1u : 0u;
        apc.hasNormals = hasNormals ? 1u : 0u;
        apc.normalsPacked01 = params.normalsPacked01 ? 1u : 0u;
        apc.isFinalPass = isFinal ? 1u : 0u;
        apc.remodulateAlbedo = (hasAlbedo && params.demodulateAlbedo) ? 1u : 0u;
        apc.preserveAlpha = params.preserveAlpha ? 1u : 0u;
        apc.outputToSrgb = params.inputIsSrgb ? 1u : 0u;
        apc.reverseZ = params.reverseZ ? 1u : 0u;
        apc.nearPlane = tpc.nearPlane;
        apc.farPlane = tpc.farPlane;

        const SharedTexture* aSamplers[5] = {
            currSrc,
            &depthIn,
            &normalsTex,
            &albedoTex,
            &colorIn,
        };
        const SharedTexture* aStorages[1] = {currDst};
        atrousPass_.dispatchMulti(cmd, ctx.device, aSamplers, 5u, aStorages, 1u,
                                  params.width, params.height, &apc, sizeof(apc));
        currSrc = currDst;
    }

    bank.currIdx = nextIdx;
    bank.validHistory = true;
    return true;
}

} // namespace linrtx
