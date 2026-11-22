#include "rtx_hdr.hpp"
#include "native_log.hpp"

#include "cas_sharpen_spv.h"
#include "rtx_hdr_spv.h"

#include <dlfcn.h>
#include <filesystem>
#include <vector>
#include <cstdio>

namespace linrtx {
namespace {

namespace fs = std::filesystem;

struct CasPushConstants {
    uint32_t width;
    uint32_t height;
    float sharpness;
    uint32_t pad;
};
static_assert(sizeof(CasPushConstants) == 16);

struct RtxHdrPushConstants {
    uint32_t width;
    uint32_t height;
    uint32_t outputMode;
    uint32_t inputIsHdr;
    float contrast;
    float saturation;
    float middleGray;
    float maxNits;
    float highlightBoost;
    uint32_t debandEnable;
    uint32_t frameIndex;
    uint32_t pad;
};
static_assert(sizeof(RtxHdrPushConstants) == 48);

} // namespace

bool RtxHdrEngine::discoverTrueHdr(const char* searchDirs) {
    std::vector<std::string> dirs = VkNgxContext::splitSearchDirs(searchDirs);

    static constexpr const char* kCandidates[] = {
        "libnvidia-ngx-truehdr.so",
        "libnvidia-ngx-truehdr.so.1",
        "libnvidia-ngx-truehdr.so.1.2.3",
    };

    for (const std::string& dir : dirs) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        // Skip standalone CUDA/VFX wheels (e.g. Python nvvfx/libs) that ship incompatible libnvngxruntime.so
        if (fs::exists(fs::path(dir) / "libnvngxruntime.so", ec)) {
            logNative("rtxHdr: skipping non-Vulkan CUDA VFX dir %s", dir.c_str());
            continue;
        }
        for (const char* name : kCandidates) {
            fs::path p = fs::path(dir) / name;
            if (fs::exists(p, ec)) {
                void* h = dlopen(p.c_str(), RTLD_LAZY | RTLD_LOCAL);
                if (h) {
                    trueHdrDlHandle_ = h;
                    trueHdrLibPresent_ = true;
                    trueHdrLibPath_ = p.string();
                    statusStr_ = "TrueHDR SO ready: " + p.filename().string();
                    logNative("rtxHdr: loaded TrueHDR SO %s", p.c_str());
                    return true;
                }
            }
        }
    }

    trueHdrLibPresent_ = false;
    trueHdrLibPath_.clear();
    statusStr_ = "GPU Compute RTX HDR (TrueHDR NGX not loaded)";
    return false;
}

bool RtxHdrEngine::init(VkNgxContext& ctx, const char* trueHdrSearchDirs) {
    destroy(ctx);
    if (!ctx.vkReady) {
        statusStr_ = "Vulkan unavailable";
        return false;
    }

    const bool casOk = casPass_.init(ctx.device, kCasSharpenSpv, sizeof(kCasSharpenSpv), sizeof(CasPushConstants));
    const bool hdrOk = hdrPass_.init(ctx.device, kRtxHdrSpv, sizeof(kRtxHdrSpv), sizeof(RtxHdrPushConstants));
    trueHdrSearchDirs_ = (trueHdrSearchDirs && *trueHdrSearchDirs) ? trueHdrSearchDirs : "";
    trueHdrProbed_ = false;
    trueHdrLibPresent_ = false;
    statusStr_ = (casOk && hdrOk) ? "GPU Compute RTX HDR Ready" : "Compute shader init failed";
    logNative("rtxHdr.init: casOk=%d hdrOk=%d (lazy TrueHDR probing deferred)", casOk ? 1 : 0, hdrOk ? 1 : 0);

    return casOk && hdrOk;
}

void RtxHdrEngine::destroy(VkNgxContext& ctx) {
    if (trueHdrHandle_) {
        if (ctx.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(ctx.device);
        }
        NVSDK_NGX_VULKAN_ReleaseFeature(trueHdrHandle_);
        trueHdrHandle_ = nullptr;
    }
    if (trueHdrParams_) {
        NVSDK_NGX_VULKAN_DestroyParameters(trueHdrParams_);
        trueHdrParams_ = nullptr;
    }
    if (trueHdrDlHandle_) {
        dlclose(trueHdrDlHandle_);
        trueHdrDlHandle_ = nullptr;
    }
    trueHdrProbed_ = false;
    trueHdrLibPresent_ = false;
    trueHdrInitFailed_ = false;
    trueHdrW_ = 0;
    trueHdrH_ = 0;

    casPass_.destroy(ctx.device);
    hdrPass_.destroy(ctx.device);
}

bool RtxHdrEngine::applyCasSharpen(VkNgxContext& ctx,
                                   VkCommandBuffer cmd,
                                   SharedTexture& srcTex,
                                   SharedTexture& dstTex,
                                   uint32_t width,
                                   uint32_t height,
                                   float sharpness) {
    if (!casPass_.valid()) return false;

    ctx.cmdBarrier(cmd, srcTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, dstTex, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    CasPushConstants pc{width, height, sharpness, 0u};
    casPass_.dispatch(cmd, ctx.device, srcTex, dstTex, width, height, &pc, sizeof(pc));
    return true;
}

bool RtxHdrEngine::evaluateTrueHdrNgx(VkNgxContext& ctx,
                                      VkCommandBuffer cmd,
                                      const LinRtxFrameConfig& frame,
                                      SharedTexture& srcTex,
                                      SharedTexture& dstPqTex,
                                      int32_t& outErrCode) {
    if (!ctx.ngxReady || trueHdrInitFailed_) {
        return false;
    }
    if (frame.hdrEngine != LINRTX_HDR_TRUEHDR_ONLY) {
        return false;
    }
    if (!trueHdrProbed_) {
        trueHdrProbed_ = true;
        discoverTrueHdr(trueHdrSearchDirs_.c_str());
    }
    if (!trueHdrLibPresent_) {
        return false;
    }

    if (!trueHdrHandle_ || !trueHdrParams_ || trueHdrW_ != frame.displayWidth || trueHdrH_ != frame.displayHeight) {
        if (trueHdrHandle_) {
            vkDeviceWaitIdle(ctx.device);
            NVSDK_NGX_VULKAN_ReleaseFeature(trueHdrHandle_);
            trueHdrHandle_ = nullptr;
        }
        if (trueHdrParams_) {
            NVSDK_NGX_VULKAN_DestroyParameters(trueHdrParams_);
            trueHdrParams_ = nullptr;
        }
        NVSDK_NGX_Result allocRes = NVSDK_NGX_VULKAN_AllocateParameters(&trueHdrParams_);
        if (NVSDK_NGX_FAILED(allocRes) || !trueHdrParams_) {
            outErrCode = static_cast<int32_t>(allocRes);
            trueHdrInitFailed_ = true;
            trueHdrParams_ = nullptr;
            statusStr_ = "TrueHDR NGX params alloc failed; using GPU HDR shader";
            return false;
        }

        NVSDK_NGX_Parameter_SetUI(trueHdrParams_, NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        NVSDK_NGX_Parameter_SetUI(trueHdrParams_, NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        NVSDK_NGX_Parameter_SetUI(trueHdrParams_, NVSDK_NGX_Parameter_Width, frame.displayWidth);
        NVSDK_NGX_Parameter_SetUI(trueHdrParams_, NVSDK_NGX_Parameter_Height, frame.displayHeight);
        NVSDK_NGX_Parameter_SetUI(trueHdrParams_, NVSDK_NGX_Parameter_OutWidth, frame.displayWidth);
        NVSDK_NGX_Parameter_SetUI(trueHdrParams_, NVSDK_NGX_Parameter_OutHeight, frame.displayHeight);
        NVSDK_NGX_Parameter_SetUI(trueHdrParams_, NVSDK_NGX_Parameter_Input1_Format, static_cast<unsigned int>(srcTex.vkFormat));
        NVSDK_NGX_Parameter_SetUI(trueHdrParams_, NVSDK_NGX_Parameter_Output_Format, static_cast<unsigned int>(dstPqTex.vkFormat));
        NVSDK_NGX_Parameter_SetUI(trueHdrParams_, NVSDK_NGX_Parameter_Format, static_cast<unsigned int>(dstPqTex.vkFormat));

        // Feature 14 is NVIDIA TrueHDR in NGX / nvvfx
        NVSDK_NGX_Result cres = NVSDK_NGX_VULKAN_CreateFeature1(
            ctx.device, cmd, static_cast<NVSDK_NGX_Feature>(14), trueHdrParams_, &trueHdrHandle_);
        if (NVSDK_NGX_FAILED(cres) || !trueHdrHandle_) {
            logNative("rtxHdr: TrueHDR create failed w=%u h=%u res=0x%08x",
                      frame.displayWidth, frame.displayHeight, static_cast<unsigned>(cres));
            outErrCode = static_cast<int32_t>(cres);
            trueHdrHandle_ = nullptr;
            trueHdrInitFailed_ = true;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "TrueHDR NGX create returned 0x%08X; fallback active", static_cast<unsigned>(cres));
            statusStr_ = buf;
            return false;
        }
        trueHdrW_ = frame.displayWidth;
        trueHdrH_ = frame.displayHeight;
    }

    ctx.cmdBarrier(cmd, srcTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, dstPqTex, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    NVSDK_NGX_Resource_VK resIn = VkNgxContext::makeNgxResource(srcTex, false);
    NVSDK_NGX_Resource_VK resOut = VkNgxContext::makeNgxResource(dstPqTex, true);

    NVSDK_NGX_Parameter_SetVoidPointer(trueHdrParams_, NVSDK_NGX_Parameter_Input1, &resIn);
    NVSDK_NGX_Parameter_SetVoidPointer(trueHdrParams_, NVSDK_NGX_Parameter_Output, &resOut);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.Contrast", frame.hdrContrast);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.Saturation", frame.hdrSaturation);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.MiddleGray", frame.hdrMiddleGray);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.MaxLuminance", frame.hdrMaxLuminance);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.DebandingOff", frame.hdrDebandingOff ? 1u : 0u);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.InLeft", 0u);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.InTop", 0u);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.InRight", frame.displayWidth);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.InBottom", frame.displayHeight);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.OutLeft", 0u);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.OutTop", 0u);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.OutRight", frame.displayWidth);
    NVSDK_NGX_Parameter_SetUI(trueHdrParams_, "TrueHDR.OutBottom", frame.displayHeight);

    NVSDK_NGX_Result eres = NVSDK_NGX_VULKAN_EvaluateFeature_C(cmd, trueHdrHandle_, trueHdrParams_, nullptr);
    if (NVSDK_NGX_FAILED(eres)) {
        outErrCode = static_cast<int32_t>(eres);
        trueHdrInitFailed_ = true;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "TrueHDR eval 0x%08X; using GPU HDR shader", static_cast<unsigned>(eres));
        statusStr_ = buf;
        return false;
    }

    statusStr_ = "TrueHDR NGX Active";
    return true;
}

bool RtxHdrEngine::evaluateHdr(VkNgxContext& ctx,
                               VkCommandBuffer cmd,
                               const LinRtxFrameConfig& frame,
                               SharedTexture& srcTex,
                               SharedTexture& trueHdrTempPqTex,
                               SharedTexture& dstTex,
                               bool& outUsedTrueHdr,
                               bool& outUsedShader,
                               std::string& outError,
                               int32_t& outErrorCode,
                               bool allowTrueHdr) {
    outUsedTrueHdr = false;
    outUsedShader = false;

    if (frame.hdrEngine == LINRTX_HDR_OFF) {
        statusStr_ = "Off";
        return true;
    }

    bool tryTrueHdr = allowTrueHdr && (frame.hdrEngine == LINRTX_HDR_AUTO || frame.hdrEngine == LINRTX_HDR_TRUEHDR_ONLY);
    SharedTexture* shaderInput = &srcTex;
    uint32_t inputMode = (frame.colorIsHdrInput != 0) ? 1u : 0u;

    if (tryTrueHdr && evaluateTrueHdrNgx(ctx, cmd, frame, srcTex, trueHdrTempPqTex, outErrorCode)) {
        outUsedTrueHdr = true;
        // If output target is PQ BT.2020 directly without extra highlight boost, skip redundant compute if same format,
        // otherwise convert PQ -> target mode (or apply highlight boost cleanly via compute).
        if (frame.hdrOutputMode == LINRTX_HDR_OUT_PQ_BT2020 &&
            frame.hdrHighlightBoost <= 0.001f &&
            dstTex.vkFormat == trueHdrTempPqTex.vkFormat) {
            ctx.cmdBarrier(cmd, trueHdrTempPqTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            ctx.cmdBarrier(cmd, dstTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {frame.displayWidth, frame.displayHeight, 1};
            vkCmdCopyImage(cmd,
                           trueHdrTempPqTex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dstTex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);
            return true;
        }
        shaderInput = &trueHdrTempPqTex;
        inputMode = 2u; // PQ BT.2020 input from TrueHDR
    } else if (frame.hdrEngine == LINRTX_HDR_TRUEHDR_ONLY) {
        outError = statusStr_.empty() ? "TrueHDR-only requested but unavailable" : statusStr_;
        return false;
    }

    if (!hdrPass_.valid()) {
        outError = "RTX HDR compute shader not initialized";
        outErrorCode = -3;
        return false;
    }

    ctx.cmdBarrier(cmd, *shaderInput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, dstTex, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    frameCounter_++;
    RtxHdrPushConstants pc{};
    pc.width = frame.displayWidth;
    pc.height = frame.displayHeight;
    pc.outputMode = static_cast<uint32_t>(frame.hdrOutputMode);
    pc.inputIsHdr = inputMode;
    pc.contrast = static_cast<float>(frame.hdrContrast) / 100.0f;
    pc.saturation = static_cast<float>(frame.hdrSaturation) / 100.0f;
    pc.middleGray = static_cast<float>(frame.hdrMiddleGray) / 100.0f;
    pc.maxNits = static_cast<float>(frame.hdrMaxLuminance);
    pc.highlightBoost = frame.hdrHighlightBoost;
    pc.debandEnable = (frame.hdrDebandingOff == 0) ? 1u : 0u;
    pc.frameIndex = static_cast<uint32_t>(frameCounter_);

    hdrPass_.dispatch(cmd, ctx.device, *shaderInput, dstTex, frame.displayWidth, frame.displayHeight, &pc, sizeof(pc));
    outUsedShader = true;
    if (!outUsedTrueHdr) {
        statusStr_ = (frame.hdrOutputMode == LINRTX_HDR_OUT_PQ_BT2020)
                         ? "GPU Compute HDR10 (BT.2020 PQ)"
                         : (frame.hdrOutputMode == LINRTX_HDR_OUT_SCRGB_FP16 ? "GPU Compute scRGB HDR" : "GPU Compute RTX HDR Boost");
    }
    return true;
}

} // namespace linrtx
