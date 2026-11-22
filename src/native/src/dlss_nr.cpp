#include "dlss_nr.hpp"
#include "dlss_sr.hpp"
#include "native_log.hpp"
#include "neural_analysis_spv.h"
#include "neural_filter_spv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <filesystem>
#include <vector>

namespace linrtx {

void computeNeuralWorkingSize(uint32_t targetW,
                              uint32_t targetH,
                              float workingScale,
                              uint32_t& outWorkW,
                              uint32_t& outWorkH) {
    if (targetW == 0 || targetH == 0) {
        outWorkW = 0;
        outWorkH = 0;
        return;
    }
    const float clamped = std::clamp(workingScale, 0.50f, 1.00f);
    if (clamped >= 0.995f) {
        outWorkW = targetW;
        outWorkH = targetH;
        return;
    }
    const uint32_t rw = static_cast<uint32_t>(std::lround(static_cast<double>(targetW) * clamped));
    const uint32_t rh = static_cast<uint32_t>(std::lround(static_cast<double>(targetH) * clamped));
    outWorkW = std::clamp(rw, 16u, targetW);
    outWorkH = std::clamp(rh, 16u, targetH);
    if (outWorkW < targetW && (outWorkW & 1u) != 0u) {
        outWorkW = std::min(targetW, outWorkW + 1u);
    }
    if (outWorkH < targetH && (outWorkH & 1u) != 0u) {
        outWorkH = std::min(targetH, outWorkH + 1u);
    }
}

unsigned int normalizeNeuralPreset(int32_t neuralPreset) {
    switch (neuralPreset) {
        case LINRTX_NEURAL_PRESET_QUALITY:
            return 1u; // WEIGHTS_HT / Quality
        case LINRTX_NEURAL_PRESET_SMALL_FP8:
            return 2u; // WEIGHTS_CC_SMALL_FP8 / Fast
        default:
            return 0u; // Auto
    }
}

namespace {

namespace fs = std::filesystem;

struct NeuralAnalysisPush {
    uint32_t workWidth;
    uint32_t workHeight;
    uint32_t resetHistory;
    uint32_t inputIsHdr;
    uint32_t reverseZ;
    uint32_t useAutoMask;
    uint32_t presetMode;
    float nearPlane;
    float farPlane;
    float temporalBlend;
};
static_assert(sizeof(NeuralAnalysisPush) == 40);

struct NeuralFilterPush {
    uint32_t width;
    uint32_t height;
    uint32_t style;
    uint32_t presetMode;
    uint32_t inputIsHdr;
    uint32_t reverseZ;
    uint32_t useAutoMask;
    uint32_t frameIndex;
    float nearPlane;
    float farPlane;
    float intensity;
    float localToneStrength;
    float localStructureStrength;
    float skinStructureStrength;
};
static_assert(sizeof(NeuralFilterPush) == 56);

const char* styleLabel(int32_t style) {
    switch (style) {
        case LINRTX_NEURAL_STYLE_NATURAL: return "Natural";
        case LINRTX_NEURAL_STYLE_CINEMATIC: return "Cinematic";
        default: return "Default";
    }
}

} // namespace

void DlssNrEngine::discoverNgxLibrary(const char* searchDirs) {
    std::vector<std::string> dirs = VkNgxContext::splitSearchDirs(searchDirs);

    static constexpr const char* kCandidates[] = {
        "libnvidia-ngx-dlssnr.so",
        "libnvidia-ngx-dlssnr.so.1",
        "libnvidia-ngx-dlssnr.so.310",
        "libnvidia-ngx-dlssnr.so.310.9.16",
        "libnvidia-ngx-builtin-dlssnr.so",
    };

    dlssnrLibPresent_ = false;
    windowsDllPresent_ = false;
    dlssnrLibPath_.clear();

    for (const std::string& dir : dirs) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        if (fs::exists(fs::path(dir) / "nvngx_dlssnr.dll", ec)) {
            windowsDllPresent_ = true;
        }
        for (const char* name : kCandidates) {
            fs::path p = fs::path(dir) / name;
            if (fs::exists(p, ec)) {
                void* h = dlopen(p.c_str(), RTLD_LAZY | RTLD_LOCAL);
                if (h) {
                    dlssnrDlHandle_ = h;
                    dlssnrLibPresent_ = true;
                    dlssnrLibPath_ = p.string();
                    versionStr_ = "NGX F18 (" + p.filename().string() + ")";
                    logNative("dlssNr: loaded Linux NGX Feature 18 SO %s", p.c_str());
                    return;
                }
            }
        }
    }
    if (windowsDllPresent_) {
        logNative("dlssNr: detected Windows nvngx_dlssnr.dll in search dir; native Linux ld.so uses Vulkan Neural Engine");
    }
}

bool DlssNrEngine::init(VkNgxContext& ctx, const char* neuralSearchDirs) {
    destroy(ctx);
    if (!ctx.vkReady) {
        statusStr_ = "Vulkan unavailable";
        return false;
    }

    const bool anOk = analysisPass_.initMulti(
        ctx.device, kNeuralAnalysisSpv, sizeof(kNeuralAnalysisSpv), 4u, 1u, sizeof(NeuralAnalysisPush));
    const bool fltOk = filterPass_.initMulti(
        ctx.device, kNeuralFilterSpv, sizeof(kNeuralFilterSpv), 4u, 1u, sizeof(NeuralFilterPush));

    neuralSearchDirs_ = (neuralSearchDirs && *neuralSearchDirs) ? neuralSearchDirs : "";
    dlssnrProbed_ = false;
    dlssnrLibPresent_ = false;
    windowsDllPresent_ = false;
    ngxAttemptFailed_ = false;
    lastFailedUnlockFlag_ = -1;
    lastFailedPreset_ = -1;
    versionStr_ = "Vulkan Neural Engine 5";
    statusStr_ = (anOk && fltOk)
        ? ("Ready (" + (ctx.gpuArchName.empty() ? std::string("Vulkan GPU") : ctx.gpuArchName) + ")")
        : "Neural shader init failed";
    logNative("dlssNr.init: anOk=%d fltOk=%d arch='%s'",
              anOk ? 1 : 0, fltOk ? 1 : 0, ctx.gpuArchName.c_str());
    return anOk && fltOk;
}

void DlssNrEngine::releaseNgxFeature(VkNgxContext& ctx) {
    if (ngxHandle_) {
        if (ctx.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(ctx.device);
        }
        NVSDK_NGX_VULKAN_ReleaseFeature(ngxHandle_);
        ngxHandle_ = nullptr;
    }
    if (ngxParams_) {
        NVSDK_NGX_VULKAN_DestroyParameters(ngxParams_);
        ngxParams_ = nullptr;
    }
    ngxKey_ = {};
}

void DlssNrEngine::destroy(VkNgxContext& ctx) {
    releaseNgxFeature(ctx);
    if (dlssnrDlHandle_) {
        dlclose(dlssnrDlHandle_);
        dlssnrDlHandle_ = nullptr;
    }
    if (ctx.device != VK_NULL_HANDLE) {
        analysisPass_.destroy(ctx.device);
        filterPass_.destroy(ctx.device);
    }
    ctx.destroySharedTexture(workStats_[0]);
    ctx.destroySharedTexture(workStats_[1]);
    statsPing_ = 0;
    statsW_ = 0;
    statsH_ = 0;
    historyValid_ = false;
    loggedFirstEval_ = false;
    frameCounter_ = 0;
    dlssnrProbed_ = false;
    dlssnrLibPresent_ = false;
    windowsDllPresent_ = false;
    ngxAttemptFailed_ = false;
    lastFailedUnlockFlag_ = -1;
    lastFailedPreset_ = -1;
}

bool DlssNrEngine::ensureWorkTextures(VkNgxContext& ctx, uint32_t workW, uint32_t workH) {
    if (workStats_[0].valid() && workStats_[1].valid() && statsW_ == workW && statsH_ == workH) {
        return true;
    }
    constexpr VkImageUsageFlags kUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const bool ok =
        ctx.createSharedTexture(workStats_[0], workW, workH, VK_FORMAT_R16G16B16A16_SFLOAT, kUsage, 0, false) &&
        ctx.createSharedTexture(workStats_[1], workW, workH, VK_FORMAT_R16G16B16A16_SFLOAT, kUsage, 0, false);
    if (!ok) {
        ctx.destroySharedTexture(workStats_[0]);
        ctx.destroySharedTexture(workStats_[1]);
        statsW_ = 0;
        statsH_ = 0;
        return false;
    }
    statsW_ = workW;
    statsH_ = workH;
    statsPing_ = 0;
    historyValid_ = false;
    return true;
}

bool DlssNrEngine::evaluateNgxFeature18(VkNgxContext& ctx,
                                        VkCommandBuffer cmd,
                                        const LinRtxFrameConfig& frame,
                                        SharedTexture& srcColor,
                                        SharedTexture& depthIn,
                                        SharedTexture& mvecIn,
                                        SharedTexture& dstColor,
                                        int32_t& outActivePreset,
                                        std::string& outError,
                                        int32_t& outErrorCode) {
    if (!ctx.ngxReady) {
        outErrorCode = -1;
        outError = "NGX runtime not ready";
        return false;
    }
    if (!dlssnrProbed_) {
        dlssnrProbed_ = true;
        discoverNgxLibrary(neuralSearchDirs_.c_str());
    }

    // Re-enable probing if user flipped unlock or preset settings
    if (ngxAttemptFailed_ &&
        (lastFailedUnlockFlag_ != frame.neuralUnlockAnyRtx || lastFailedPreset_ != frame.neuralPreset)) {
        ngxAttemptFailed_ = false;
    }
    if (ngxAttemptFailed_) {
        outErrorCode = static_cast<int32_t>(NVSDK_NGX_Result_FAIL_FeatureNotSupported);
        outError = "NGX Feature 18 unavailable on this driver/runtime";
        return false;
    }

    const bool canTryFeature18 = dlssnrLibPresent_ || ctx.dlssnrSupported ||
                                 (frame.neuralUnlockAnyRtx != 0) ||
                                 (frame.neuralMode == LINRTX_NEURAL_NGX_ONLY);
    if (!canTryFeature18) {
        return false;
    }

    unsigned int presetVal = normalizeNeuralPreset(frame.neuralPreset);
    NgxKey wanted{
        srcColor.width,
        srcColor.height,
        static_cast<uint32_t>(srcColor.vkFormat),
        static_cast<uint32_t>(dstColor.vkFormat),
        presetVal,
        frame.neuralStyle,
        frame.neuralUnlockAnyRtx,
    };

    if (!ngxHandle_ || !ngxParams_ || !(ngxKey_ == wanted)) {
        releaseNgxFeature(ctx);

        auto tryCreatePreset = [&](unsigned int tryPreset, NVSDK_NGX_Result& outRes) -> bool {
            NVSDK_NGX_Result allocRes = NVSDK_NGX_VULKAN_AllocateParameters(&ngxParams_);
            if (NVSDK_NGX_FAILED(allocRes) || !ngxParams_) {
                outRes = allocRes;
                ngxParams_ = nullptr;
                return false;
            }

            NVSDK_NGX_Parameter_SetUI(ngxParams_, NVSDK_NGX_Parameter_CreationNodeMask, 1u);
            NVSDK_NGX_Parameter_SetUI(ngxParams_, NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
            NVSDK_NGX_Parameter_SetUI(ngxParams_, NVSDK_NGX_Parameter_Width, srcColor.width);
            NVSDK_NGX_Parameter_SetUI(ngxParams_, NVSDK_NGX_Parameter_Height, srcColor.height);
            NVSDK_NGX_Parameter_SetUI(ngxParams_, NVSDK_NGX_Parameter_OutWidth, dstColor.width);
            NVSDK_NGX_Parameter_SetUI(ngxParams_, NVSDK_NGX_Parameter_OutHeight, dstColor.height);
            NVSDK_NGX_Parameter_SetUI(ngxParams_, NVSDK_NGX_Parameter_Input1_Format, static_cast<unsigned int>(srcColor.vkFormat));
            NVSDK_NGX_Parameter_SetUI(ngxParams_, NVSDK_NGX_Parameter_Output_Format, static_cast<unsigned int>(dstColor.vkFormat));
            NVSDK_NGX_Parameter_SetI(ngxParams_, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
            NVSDK_NGX_Parameter_SetUI(ngxParams_, "DLSSNR.Hint.Render.Preset", tryPreset);
            NVSDK_NGX_Parameter_SetI(ngxParams_, "DLSSNR.Style", frame.neuralStyle);
            NVSDK_NGX_Parameter_SetI(ngxParams_, "DLSSNR.DepthInverted", frame.depthInverted ? 1 : 0);

            if (frame.neuralUnlockAnyRtx != 0) {
                // RTX 20/30/40/50 unlock & capability hints (per openNR / OptiScaler_DLSSNR)
                NVSDK_NGX_Parameter_SetI(ngxParams_, "DLSSNR.Available", 1);
                NVSDK_NGX_Parameter_SetI(ngxParams_, NVSDK_NGX_Parameter_RTXValue, 1);
                NVSDK_NGX_Parameter_SetUI(ngxParams_, "DLSSNR.Width", srcColor.width);
                NVSDK_NGX_Parameter_SetUI(ngxParams_, "DLSSNR.Height", srcColor.height);
                const unsigned int flags = (frame.depthInverted ? 8u : 0u) | (frame.colorIsHdrInput ? 1u : 0u);
                NVSDK_NGX_Parameter_SetUI(ngxParams_, "DLSSNR.Create.Flags", flags);
            }

            outRes = NVSDK_NGX_VULKAN_CreateFeature1(
                ctx.device,
                cmd,
                static_cast<NVSDK_NGX_Feature>(NVSDK_NGX_Feature_Reserved18),
                ngxParams_,
                &ngxHandle_);
            logNative("dlssNr: CreateFeature(18) w=%u h=%u preset=%u unlock=%d res=0x%08x handle=%p",
                      srcColor.width, srcColor.height, tryPreset, frame.neuralUnlockAnyRtx,
                      static_cast<unsigned>(outRes), static_cast<void*>(ngxHandle_));
            if (NVSDK_NGX_SUCCEED(outRes) && ngxHandle_) {
                return true;
            }
            ngxHandle_ = nullptr;
            NVSDK_NGX_VULKAN_DestroyParameters(ngxParams_);
            ngxParams_ = nullptr;
            return false;
        };

        NVSDK_NGX_Result createRes = NVSDK_NGX_Result_Fail;
        bool created = tryCreatePreset(presetVal, createRes);
        if (!created && frame.neuralUnlockAnyRtx != 0 && presetVal != 2u) {
            // Try Small/FP8 preset (WEIGHTS_CC_SMALL_FP8) on Turing/Ampere/Ada unlock
            unsigned int fallbackPreset = 2u;
            created = tryCreatePreset(fallbackPreset, createRes);
            if (created) {
                presetVal = fallbackPreset;
                wanted.preset = fallbackPreset;
            }
        }
        if (!created) {
            ngxAttemptFailed_ = true;
            lastFailedUnlockFlag_ = frame.neuralUnlockAnyRtx;
            lastFailedPreset_ = frame.neuralPreset;
            outErrorCode = static_cast<int32_t>(createRes);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "NGX Feature 18 create returned 0x%08X", static_cast<unsigned>(createRes));
            outError = buf;
            return false;
        }
        ngxKey_ = wanted;
    }

    ctx.cmdBarrier(cmd, srcColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, depthIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, mvecIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, dstColor, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    NVSDK_NGX_Resource_VK resColor = VkNgxContext::makeNgxResource(srcColor, false);
    NVSDK_NGX_Resource_VK resDepth = VkNgxContext::makeNgxResource(depthIn, false);
    NVSDK_NGX_Resource_VK resMvec = VkNgxContext::makeNgxResource(mvecIn, false);
    NVSDK_NGX_Resource_VK resOut = VkNgxContext::makeNgxResource(dstColor, true);

    const float mvScaleX = static_cast<float>(srcColor.width);
    const float mvScaleY = static_cast<float>(srcColor.height);
    const float frameTimeMs = (frame.frameTimeMs > 0.0f) ? frame.frameTimeMs : 16.666f;

    NVSDK_NGX_Parameter_SetVoidPointer(ngxParams_, "DLSSNR.Color", &resColor);
    NVSDK_NGX_Parameter_SetVoidPointer(ngxParams_, NVSDK_NGX_Parameter_Input1, &resColor);
    NVSDK_NGX_Parameter_SetVoidPointer(ngxParams_, "DLSSNR.Output", &resOut);
    NVSDK_NGX_Parameter_SetVoidPointer(ngxParams_, NVSDK_NGX_Parameter_Output, &resOut);
    NVSDK_NGX_Parameter_SetVoidPointer(ngxParams_, "DLSSNR.Depth", &resDepth);
    NVSDK_NGX_Parameter_SetVoidPointer(ngxParams_, "DLSSNR.MVec", &resMvec);

    NVSDK_NGX_Parameter_SetI(ngxParams_, "DLSSNR.DepthInverted", frame.depthInverted ? 1 : 0);
    NVSDK_NGX_Parameter_SetI(ngxParams_, "DLSSNR.Reset", frame.resetHistory ? 1 : 0);
    NVSDK_NGX_Parameter_SetF(ngxParams_, "DLSSNR.MVecScaleX", mvScaleX);
    NVSDK_NGX_Parameter_SetF(ngxParams_, "DLSSNR.MVecScaleY", mvScaleY);
    NVSDK_NGX_Parameter_SetI(ngxParams_, "DLSSNR.Style", frame.neuralStyle);
    NVSDK_NGX_Parameter_SetF(ngxParams_, "DLSSNR.Intensity", std::clamp(frame.neuralIntensity, 0.0f, 2.0f));
    NVSDK_NGX_Parameter_SetF(ngxParams_, "DLSSNR.LocalToneStrength", std::clamp(frame.neuralLocalToneStrength, 0.0f, 2.0f));
    NVSDK_NGX_Parameter_SetF(ngxParams_, "DLSSNR.LocalStructureStrength", std::clamp(frame.neuralLocalStructureStrength, 0.0f, 2.0f));
    NVSDK_NGX_Parameter_SetF(ngxParams_, "DLSSNR.SkinStructureStrength", std::clamp(frame.neuralSkinStructureStrength, -1.0f, 2.0f));
    NVSDK_NGX_Parameter_SetI(ngxParams_, "DLSSNR.UseAutoMask", frame.neuralUseAutoMask ? 1 : 0);
    NVSDK_NGX_Parameter_SetF(ngxParams_, "PreExposure", 1.0f);
    NVSDK_NGX_Parameter_SetF(ngxParams_, "FrametimeDelta", frameTimeMs);

    NVSDK_NGX_Result evalRes = NVSDK_NGX_VULKAN_EvaluateFeature_C(cmd, ngxHandle_, ngxParams_, nullptr);
    if (NVSDK_NGX_FAILED(evalRes)) {
        outErrorCode = static_cast<int32_t>(evalRes);
        ngxAttemptFailed_ = true;
        lastFailedUnlockFlag_ = frame.neuralUnlockAnyRtx;
        lastFailedPreset_ = frame.neuralPreset;
        releaseNgxFeature(ctx);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "NGX Feature 18 eval failed: 0x%08X", static_cast<unsigned>(evalRes));
        outError = buf;
        return false;
    }

    outActivePreset = static_cast<int32_t>(presetVal);
    if (!dlssnrLibPresent_) {
        versionStr_ = "NGX F18 (310.9.16)";
    }
    statusStr_ = std::string("NGX Feature 18 (") + styleLabel(frame.neuralStyle) +
                 (frame.neuralUnlockAnyRtx ? ", Unlocked)" : ")");
    if (!loggedFirstEval_) {
        loggedFirstEval_ = true;
        logNative("dlssNr: first evaluate succeeded backend=NGX_F18 %ux%u style=%s intensity=%.2f",
                  srcColor.width, srcColor.height, styleLabel(frame.neuralStyle), frame.neuralIntensity);
    }
    return true;
}

bool DlssNrEngine::evaluateVulkanNeural(VkNgxContext& ctx,
                                        VkCommandBuffer cmd,
                                        const LinRtxFrameConfig& frame,
                                        SharedTexture& srcColor,
                                        SharedTexture& depthIn,
                                        SharedTexture& mvecIn,
                                        SharedTexture& dstColor,
                                        int32_t& outActivePreset,
                                        std::string& outError,
                                        int32_t& outErrorCode) {
    if (!analysisPass_.valid() || !filterPass_.valid()) {
        outError = "Vulkan Neural Engine shaders not initialized";
        outErrorCode = -3;
        return false;
    }

    uint32_t workW = 0, workH = 0;
    computeNeuralWorkingSize(srcColor.width, srcColor.height, frame.neuralWorkingScale, workW, workH);
    if (workW == 0 || workH == 0 || !ensureWorkTextures(ctx, workW, workH)) {
        outError = "Failed to allocate Neural Engine working buffers";
        outErrorCode = -3;
        return false;
    }

    const uint32_t currIdx = statsPing_ & 1u;
    const uint32_t prevIdx = currIdx ^ 1u;
    SharedTexture& currStats = workStats_[currIdx];
    SharedTexture& prevStats = workStats_[prevIdx];

    ctx.cmdBarrier(cmd, srcColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, depthIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, mvecIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, prevStats, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, currStats, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    const unsigned int presetVal = normalizeNeuralPreset(frame.neuralPreset);
    const bool needReset = (frame.resetHistory != 0) || !historyValid_;

    NeuralAnalysisPush apc{};
    apc.workWidth = workW;
    apc.workHeight = workH;
    apc.resetHistory = needReset ? 1u : 0u;
    apc.inputIsHdr = (frame.colorIsHdrInput != 0) ? 1u : 0u;
    apc.reverseZ = (frame.depthInverted != 0) ? 1u : 0u;
    apc.useAutoMask = (frame.neuralUseAutoMask != 0) ? 1u : 0u;
    apc.presetMode = presetVal;
    apc.nearPlane = (frame.cameraNear > 0.0f) ? frame.cameraNear : 0.05f;
    apc.farPlane = (frame.cameraFar > apc.nearPlane) ? frame.cameraFar : 256.0f;
    apc.temporalBlend = 0.82f;

    const SharedTexture* aSamplers[4] = {&srcColor, &depthIn, &mvecIn, &prevStats};
    const SharedTexture* aStorages[1] = {&currStats};
    analysisPass_.dispatchMulti(cmd, ctx.device, aSamplers, 4u, aStorages, 1u, workW, workH, &apc, sizeof(apc));

    ctx.cmdBarrier(cmd, currStats, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, dstColor, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    ++frameCounter_;
    NeuralFilterPush fpc{};
    fpc.width = srcColor.width;
    fpc.height = srcColor.height;
    fpc.style = static_cast<uint32_t>(std::clamp(frame.neuralStyle, 0, 2));
    fpc.presetMode = presetVal;
    fpc.inputIsHdr = (frame.colorIsHdrInput != 0) ? 1u : 0u;
    fpc.reverseZ = (frame.depthInverted != 0) ? 1u : 0u;
    fpc.useAutoMask = (frame.neuralUseAutoMask != 0) ? 1u : 0u;
    fpc.frameIndex = static_cast<uint32_t>(frameCounter_);
    fpc.nearPlane = apc.nearPlane;
    fpc.farPlane = apc.farPlane;
    fpc.intensity = std::clamp(frame.neuralIntensity, 0.0f, 2.0f);
    fpc.localToneStrength = std::clamp(frame.neuralLocalToneStrength, 0.0f, 2.0f);
    fpc.localStructureStrength = std::clamp(frame.neuralLocalStructureStrength, 0.0f, 2.0f);
    fpc.skinStructureStrength = std::clamp(frame.neuralSkinStructureStrength, -1.0f, 2.0f);

    const SharedTexture* fSamplers[4] = {&srcColor, &depthIn, &mvecIn, &currStats};
    const SharedTexture* fStorages[1] = {&dstColor};
    filterPass_.dispatchMulti(cmd, ctx.device, fSamplers, 4u, fStorages, 1u,
                              srcColor.width, srcColor.height, &fpc, sizeof(fpc));

    statsPing_ = currIdx ^ 1u;
    historyValid_ = true;
    outActivePreset = static_cast<int32_t>(presetVal);
    versionStr_ = "Vulkan Neural Engine 5";

    const std::string& arch = ctx.gpuArchName.empty() ? std::string("Vulkan GPU") : ctx.gpuArchName;
    if (windowsDllPresent_) {
        statusStr_ = std::string("Vulkan Neural Filter (") + styleLabel(frame.neuralStyle) + ", " + arch + "; PE DLL ignored)";
    } else {
        statusStr_ = std::string("Vulkan Neural Filter (") + styleLabel(frame.neuralStyle) + ", " + arch + ")";
    }
    if (!loggedFirstEval_) {
        loggedFirstEval_ = true;
        logNative("dlssNr: first evaluate succeeded backend=VulkanNeural5 %ux%u (work %ux%u) style=%s intensity=%.2f",
                  srcColor.width, srcColor.height, workW, workH, styleLabel(frame.neuralStyle), frame.neuralIntensity);
    }
    return true;
}

bool DlssNrEngine::evaluate(VkNgxContext& ctx,
                            VkCommandBuffer cmd,
                            const LinRtxFrameConfig& frame,
                            SharedTexture& srcColor,
                            SharedTexture& depthIn,
                            SharedTexture& mvecIn,
                            SharedTexture& dstColor,
                            int32_t& outActivePreset,
                            bool& outUsedNgx,
                            std::string& outError,
                            int32_t& outErrorCode) {
    outUsedNgx = false;
    if (frame.neuralMode == LINRTX_NEURAL_OFF) {
        statusStr_ = "Off";
        return true;
    }

    const bool tryNgx = (frame.neuralMode == LINRTX_NEURAL_AUTO || frame.neuralMode == LINRTX_NEURAL_NGX_ONLY);
    if (tryNgx && evaluateNgxFeature18(ctx, cmd, frame, srcColor, depthIn, mvecIn, dstColor,
                                       outActivePreset, outError, outErrorCode)) {
        outUsedNgx = true;
        return true;
    }

    if (frame.neuralMode == LINRTX_NEURAL_NGX_ONLY) {
        if (outError.empty()) {
            outError = "NGX Feature 18 (DLSSNR) unavailable on this Linux runtime";
            outErrorCode = -1;
        }
        statusStr_ = outError;
        return false;
    }

    return evaluateVulkanNeural(ctx, cmd, frame, srcColor, depthIn, mvecIn, dstColor,
                                outActivePreset, outError, outErrorCode);
}

} // namespace linrtx
