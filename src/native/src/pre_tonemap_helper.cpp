#include "pre_tonemap_helper.hpp"

#include "guide_complete_spv.h"
#include "pre_tonemap_resolve_spv.h"

#include <cstring>

namespace linrtx {

namespace {

constexpr VkImageUsageFlags kInternalImageUsage =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

struct GuidePushConstants {
    uint32_t width;
    uint32_t height;
    uint32_t hasAlbedo;
    uint32_t hasNormals;
    uint32_t normalsAreWorld;
    uint32_t normalsPacked01;
    uint32_t reverseZ;
    float nearPlane;
    float farPlane;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
    float viewRow0[4];
    float viewRow1[4];
    float viewRow2[4];
};
static_assert(sizeof(GuidePushConstants) == 96);

struct ResolvePushConstants {
    uint32_t width;
    uint32_t height;
    uint32_t mode;
    uint32_t flipY;
    uint32_t preserveAlpha;
    uint32_t f1AccumActive;
    float f1FadeSamples;
    uint32_t pad;
};
static_assert(sizeof(ResolvePushConstants) == 32);

} // namespace

bool PreTonemapHelper::ensureInit(VkNgxContext& ctx, std::string& outError) {
    if (initialized_) {
        return true;
    }
    if (!guideCompletePass_.initMulti(ctx.device, kGuideCompleteSpv, sizeof(kGuideCompleteSpv),
                                      4, 4, sizeof(GuidePushConstants))) {
        outError = "Failed to initialize guide_complete compute pass";
        return false;
    }
    if (!resolvePass_.initMulti(ctx.device, kPreTonemapResolveSpv, sizeof(kPreTonemapResolveSpv),
                                2, 1, sizeof(ResolvePushConstants))) {
        guideCompletePass_.destroy(ctx.device);
        outError = "Failed to initialize pre_tonemap_resolve compute pass";
        return false;
    }
    initialized_ = true;
    return true;
}

void PreTonemapHelper::destroy(VkNgxContext& ctx) {
    guideCompletePass_.destroy(ctx.device);
    resolvePass_.destroy(ctx.device);
    ctx.destroySharedTexture(texPreHdrVk_);
    ctx.destroySharedTexture(texPreHdrOutVk_);
    ctx.destroySharedTexture(texPreHdrTmp_);
    ctx.destroySharedTexture(texGuideAlb_);
    ctx.destroySharedTexture(texGuideSpec_);
    ctx.destroySharedTexture(texGuideNorm_);
    ctx.destroySharedTexture(texGuideRough_);
    ctx.destroySharedTexture(fallbackDummy_);
    initialized_ = false;
}

bool PreTonemapHelper::ensureInternalTex(VkNgxContext& ctx,
                                         SharedTexture& tex,
                                         uint32_t w,
                                         uint32_t h,
                                         VkFormat vkFmt,
                                         VkImageUsageFlags usage) {
    if (tex.valid() && tex.width == w && tex.height == h && tex.vkFormat == vkFmt) {
        return true;
    }
    return ctx.createSharedTexture(tex, w, h, vkFmt, usage, 0, false);
}

bool PreTonemapHelper::ensureFallbackDummy(VkNgxContext& ctx, VkCommandBuffer cmd) {
    if (!fallbackDummy_.valid()) {
        if (!ctx.createSharedTexture(fallbackDummy_, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     0, false)) {
            return false;
        }
    }
    if (fallbackDummy_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        ctx.cmdBarrier(cmd, fallbackDummy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       0, VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    return true;
}

bool PreTonemapHelper::prepareColorForRr(VkNgxContext& ctx,
                                         VkCommandBuffer cmd,
                                         uint32_t w,
                                         uint32_t h,
                                         bool flipY,
                                         SharedTexture& rawHdrIn,
                                         SharedTexture*& outHdrVkFp16,
                                         std::string& outError) {
    if (!ensureInit(ctx, outError)) {
        return false;
    }
    if (!ensureInternalTex(ctx, texPreHdrVk_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, kInternalImageUsage)) {
        outError = "Failed to allocate texPreHdrVk";
        return false;
    }

    ctx.cmdBarrier(cmd, rawHdrIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, texPreHdrVk_, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    ResolvePushConstants pc{};
    pc.width = w;
    pc.height = h;
    pc.mode = 0u;
    pc.flipY = flipY ? 1u : 0u;
    pc.preserveAlpha = 0u;
    pc.f1AccumActive = 0u;
    pc.f1FadeSamples = 0.0f;

    const SharedTexture* samplers[2] = {&rawHdrIn, &rawHdrIn};
    const SharedTexture* storages[1] = {&texPreHdrVk_};
    resolvePass_.dispatchMulti(cmd, ctx.device, samplers, 2, storages, 1, w, h, &pc, sizeof(pc));
    outHdrVkFp16 = &texPreHdrVk_;
    return true;
}

bool PreTonemapHelper::completeGuides(VkNgxContext& ctx,
                                      VkCommandBuffer cmd,
                                      uint32_t w,
                                      uint32_t h,
                                      SharedTexture& colorIn,
                                      SharedTexture& depthIn,
                                      SharedTexture* explicitAlbedo,
                                      SharedTexture* explicitNormals,
                                      bool normalsAreWorld,
                                      bool normalsPacked01,
                                      bool reverseZ,
                                      float nearPlane,
                                      float farPlane,
                                      const float worldToView[16],
                                      SharedTexture*& outAlb,
                                      SharedTexture*& outSpec,
                                      SharedTexture*& outNorm,
                                      SharedTexture*& outRough,
                                      std::string& outError) {
    if (!ensureInit(ctx, outError)) {
        return false;
    }
    if (!ensureFallbackDummy(ctx, cmd) ||
        !ensureInternalTex(ctx, texGuideAlb_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, kInternalImageUsage) ||
        !ensureInternalTex(ctx, texGuideSpec_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, kInternalImageUsage) ||
        !ensureInternalTex(ctx, texGuideNorm_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, kInternalImageUsage) ||
        !ensureInternalTex(ctx, texGuideRough_, w, h, VK_FORMAT_R16_SFLOAT, kInternalImageUsage)) {
        outError = "Failed to allocate completed guide textures";
        return false;
    }

    ctx.cmdBarrier(cmd, colorIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, depthIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (explicitAlbedo && explicitAlbedo->valid()) {
        ctx.cmdBarrier(cmd, *explicitAlbedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    if (explicitNormals && explicitNormals->valid()) {
        ctx.cmdBarrier(cmd, *explicitNormals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    SharedTexture* outs[4] = {&texGuideAlb_, &texGuideSpec_, &texGuideNorm_, &texGuideRough_};
    for (SharedTexture* t : outs) {
        ctx.cmdBarrier(cmd, *t, VK_IMAGE_LAYOUT_GENERAL,
                       VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    GuidePushConstants pc{};
    pc.width = w;
    pc.height = h;
    pc.hasAlbedo = (explicitAlbedo && explicitAlbedo->valid()) ? 1u : 0u;
    pc.hasNormals = (explicitNormals && explicitNormals->valid()) ? 1u : 0u;
    pc.normalsAreWorld = normalsAreWorld ? 1u : 0u;
    pc.normalsPacked01 = normalsPacked01 ? 1u : 0u;
    pc.reverseZ = reverseZ ? 1u : 0u;
    pc.nearPlane = nearPlane > 0.0f ? nearPlane : 0.05f;
    pc.farPlane = farPlane > pc.nearPlane ? farPlane : 256.0f;

    if (worldToView) {
        std::memcpy(pc.viewRow0, &worldToView[0], sizeof(float) * 4);
        std::memcpy(pc.viewRow1, &worldToView[4], sizeof(float) * 4);
        std::memcpy(pc.viewRow2, &worldToView[8], sizeof(float) * 4);
    } else {
        pc.viewRow0[0] = 1.0f;
        pc.viewRow1[1] = 1.0f;
        pc.viewRow2[2] = 1.0f;
    }

    const SharedTexture* samplers[4] = {
        &colorIn,
        &depthIn,
        pc.hasAlbedo ? explicitAlbedo : &fallbackDummy_,
        pc.hasNormals ? explicitNormals : &fallbackDummy_
    };
    const SharedTexture* storages[4] = {
        &texGuideAlb_, &texGuideSpec_, &texGuideNorm_, &texGuideRough_
    };
    guideCompletePass_.dispatchMulti(cmd, ctx.device, samplers, 4, storages, 4, w, h, &pc, sizeof(pc));

    outAlb = &texGuideAlb_;
    outSpec = &texGuideSpec_;
    outNorm = &texGuideNorm_;
    outRough = &texGuideRough_;
    return true;
}

bool PreTonemapHelper::resolvePreTonemap(VkNgxContext& ctx,
                                         VkCommandBuffer cmd,
                                         uint32_t w,
                                         uint32_t h,
                                         SharedTexture& denoisedSrc,
                                         SharedTexture& origRawHdr,
                                         SharedTexture& outFinalHdr,
                                         bool flipSrcY,
                                         bool preserveAlpha,
                                         bool f1AccumActive,
                                         float f1FadeSamples,
                                         std::string& outError) {
    if (!ensureInit(ctx, outError)) {
        return false;
    }

    ctx.cmdBarrier(cmd, denoisedSrc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, origRawHdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, outFinalHdr, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    ResolvePushConstants pc{};
    pc.width = w;
    pc.height = h;
    pc.mode = 1u;
    pc.flipY = flipSrcY ? 1u : 0u;
    pc.preserveAlpha = preserveAlpha ? 1u : 0u;
    pc.f1AccumActive = f1AccumActive ? 1u : 0u;
    pc.f1FadeSamples = f1FadeSamples;

    const SharedTexture* samplers[2] = {&denoisedSrc, &origRawHdr};
    const SharedTexture* storages[1] = {&outFinalHdr};
    resolvePass_.dispatchMulti(cmd, ctx.device, samplers, 2, storages, 1, w, h, &pc, sizeof(pc));
    return true;
}

bool PreTonemapHelper::ensurePreHdrOutVk(VkNgxContext& ctx,
                                         uint32_t w,
                                         uint32_t h,
                                         SharedTexture*& outTex,
                                         std::string& outError) {
    if (!ensureInternalTex(ctx, texPreHdrOutVk_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, kInternalImageUsage)) {
        outError = "Failed to allocate texPreHdrOutVk";
        return false;
    }
    outTex = &texPreHdrOutVk_;
    return true;
}

bool PreTonemapHelper::ensureScratchTmp(VkNgxContext& ctx,
                                        uint32_t w,
                                        uint32_t h,
                                        VkFormat vkFmt,
                                        SharedTexture*& outTex,
                                        std::string& outError) {
    if (!ensureInternalTex(ctx, texPreHdrTmp_, w, h, vkFmt, kInternalImageUsage)) {
        outError = "Failed to allocate texPreHdrTmp";
        return false;
    }
    outTex = &texPreHdrTmp_;
    return true;
}

} // namespace linrtx
