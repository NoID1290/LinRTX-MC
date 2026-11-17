#include "dlss_sr.hpp"
#include "native_log.hpp"

#include <algorithm>
#include <cstdio>

namespace linrtx {

bool toNgxPerfQuality(int32_t dlssMode, NVSDK_NGX_PerfQuality_Value& outPq) {
    switch (dlssMode) {
        case LINRTX_DLSS_DLAA:
            outPq = NVSDK_NGX_PerfQuality_Value_DLAA;
            return true;
        case LINRTX_DLSS_QUALITY:
            outPq = NVSDK_NGX_PerfQuality_Value_MaxQuality;
            return true;
        case LINRTX_DLSS_BALANCED:
            outPq = NVSDK_NGX_PerfQuality_Value_Balanced;
            return true;
        case LINRTX_DLSS_PERFORMANCE:
            outPq = NVSDK_NGX_PerfQuality_Value_MaxPerf;
            return true;
        case LINRTX_DLSS_ULTRA_PERFORMANCE:
            outPq = NVSDK_NGX_PerfQuality_Value_UltraPerformance;
            return true;
        default:
            return false;
    }
}

NVSDK_NGX_PerfQuality_Value resolveNgxPerfQuality(int32_t dlssMode,
                                                  uint32_t renderW,
                                                  uint32_t renderH,
                                                  uint32_t displayW,
                                                  uint32_t displayH) {
    NVSDK_NGX_PerfQuality_Value pq = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (toNgxPerfQuality(dlssMode, pq)) {
        return pq;
    }
    if (displayW == 0 || displayH == 0 || (renderW >= displayW && renderH >= displayH)) {
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
    const float sx = static_cast<float>(renderW) / static_cast<float>(displayW);
    const float sy = static_cast<float>(renderH) / static_cast<float>(displayH);
    const float ratio = std::min(sx, sy);
    if (ratio >= 0.62f) {
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
    if (ratio >= 0.54f) {
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    }
    if (ratio >= 0.45f) {
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    }
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

uint32_t clampEvenDim(uint32_t val, uint32_t maxVal) {
    uint32_t c = std::clamp(val, 16u, std::max(16u, maxVal));
    if (c < maxVal && (c & 1u) != 0u) {
        c = std::min(maxVal, c + 1u);
    }
    return c;
}

namespace {

float fallbackScaleRatio(int32_t dlssMode) {
    switch (dlssMode) {
        case LINRTX_DLSS_DLAA:
        case LINRTX_DLSS_OFF:
            return 1.0f;
        case LINRTX_DLSS_QUALITY:
            return 1.0f / 1.5f;
        case LINRTX_DLSS_BALANCED:
            return 1.0f / 1.7241379f;
        case LINRTX_DLSS_PERFORMANCE:
            return 0.5f;
        case LINRTX_DLSS_ULTRA_PERFORMANCE:
            return 1.0f / 3.0f;
        default:
            return 0.67f;
    }
}

unsigned int normalizePreset(int32_t preset) {
    switch (preset) {
        case LINRTX_PRESET_E:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_E;
        case LINRTX_PRESET_F:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_F;
        case LINRTX_PRESET_J:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
        case LINRTX_PRESET_K:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
        case LINRTX_PRESET_L:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
        case LINRTX_PRESET_M:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
        default:
            return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    }
}

void applyPresetHints(NVSDK_NGX_Parameter* params, unsigned int presetVal) {
    if (!params) return;
    static constexpr const char* kHintKeys[] = {
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
    };
    for (const char* key : kHintKeys) {
        NVSDK_NGX_Parameter_SetUI(params, key, presetVal);
    }
}

} // namespace

bool DlssSrEngine::queryOptimalScale(VkNgxContext& ctx,
                                     int32_t dlssMode,
                                     uint32_t displayW,
                                     uint32_t displayH,
                                     uint32_t* outRenderW,
                                     uint32_t* outRenderH,
                                     float* outSharpness) {
    if (displayW == 0 || displayH == 0) {
        if (outRenderW) *outRenderW = 0;
        if (outRenderH) *outRenderH = 0;
        if (outSharpness) *outSharpness = 0.0f;
        return false;
    }

    NVSDK_NGX_PerfQuality_Value pq = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ctx.ngxReady && ctx.dlssSupported && ctx.ngxParams && toNgxPerfQuality(dlssMode, pq)) {
        void* cbPtr = nullptr;
        NVSDK_NGX_Parameter_GetVoidPointer(ctx.ngxParams, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &cbPtr);
        if (cbPtr) {
            NVSDK_NGX_Parameter_SetUI(ctx.ngxParams, NVSDK_NGX_Parameter_Width, displayW);
            NVSDK_NGX_Parameter_SetUI(ctx.ngxParams, NVSDK_NGX_Parameter_Height, displayH);
            NVSDK_NGX_Parameter_SetI(ctx.ngxParams, NVSDK_NGX_Parameter_PerfQualityValue, pq);
            NVSDK_NGX_Parameter_SetI(ctx.ngxParams, NVSDK_NGX_Parameter_RTXValue, 0);
            auto cb = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(cbPtr);
            NVSDK_NGX_Result res = cb(ctx.ngxParams);
            unsigned int optW = 0, optH = 0;
            float sharp = 0.0f;
            if (NVSDK_NGX_SUCCEED(res)) {
                NVSDK_NGX_Parameter_GetUI(ctx.ngxParams, NVSDK_NGX_Parameter_OutWidth, &optW);
                NVSDK_NGX_Parameter_GetUI(ctx.ngxParams, NVSDK_NGX_Parameter_OutHeight, &optH);
                NVSDK_NGX_Parameter_GetF(ctx.ngxParams, NVSDK_NGX_Parameter_Sharpness, &sharp);
                if (optW > 0 && optH > 0) {
                    if (outRenderW) *outRenderW = clampEvenDim(optW, displayW);
                    if (outRenderH) *outRenderH = clampEvenDim(optH, displayH);
                    if (outSharpness) *outSharpness = sharp;
                    return true;
                }
            }
        }
    }

    const float ratio = fallbackScaleRatio(dlssMode);
    const uint32_t rw = clampEvenDim(static_cast<uint32_t>(displayW * ratio + 0.5f), displayW);
    const uint32_t rh = clampEvenDim(static_cast<uint32_t>(displayH * ratio + 0.5f), displayH);
    if (outRenderW) *outRenderW = rw;
    if (outRenderH) *outRenderH = rh;
    if (outSharpness) *outSharpness = 0.0f;
    return false; // Analytical fallback ratios used
}

bool DlssSrEngine::evaluate(VkNgxContext& ctx,
                            VkCommandBuffer cmd,
                            const LinRtxFrameConfig& frame,
                            SharedTexture& colorIn,
                            SharedTexture& depthIn,
                            SharedTexture& mvecIn,
                            SharedTexture& colorOut,
                            int32_t& outActivePreset,
                            std::string& outError,
                            int32_t& outErrorCode) {
    if (!ctx.ngxReady || !ctx.dlssSupported) {
        outError = "DLSS Super Resolution is not available on this NGX runtime";
        outErrorCode = -1;
        return false;
    }

    const NVSDK_NGX_PerfQuality_Value pq = resolveNgxPerfQuality(
        frame.dlssMode, frame.renderWidth, frame.renderHeight, frame.displayWidth, frame.displayHeight);

    const unsigned int presetVal = normalizePreset(frame.dlssPreset);

    int createFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                      NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (frame.depthInverted != 0) {
        createFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    }
    if (frame.colorIsHdrInput != 0) {
        createFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    }

    FeatureKey wanted{
        frame.renderWidth,
        frame.renderHeight,
        frame.displayWidth,
        frame.displayHeight,
        static_cast<int32_t>(pq),
        createFlags,
        static_cast<int32_t>(presetVal),
    };

    if (!handle_ || !params_ || !(key_ == wanted)) {
        if (handle_) {
            vkDeviceWaitIdle(ctx.device);
            NVSDK_NGX_VULKAN_ReleaseFeature(handle_);
            handle_ = nullptr;
        }
        if (params_) {
            NVSDK_NGX_VULKAN_DestroyParameters(params_);
            params_ = nullptr;
        }
        NVSDK_NGX_Result allocRes = NVSDK_NGX_VULKAN_AllocateParameters(&params_);
        if (NVSDK_NGX_FAILED(allocRes) || !params_) {
            outErrorCode = static_cast<int32_t>(allocRes);
            outError = "NVSDK_NGX_VULKAN_AllocateParameters failed";
            params_ = nullptr;
            return false;
        }

        NVSDK_NGX_Parameter_SetI(params_, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
        applyPresetHints(params_, presetVal);

        NVSDK_NGX_DLSS_Create_Params createParams{};
        createParams.Feature.InWidth = frame.renderWidth;
        createParams.Feature.InHeight = frame.renderHeight;
        createParams.Feature.InTargetWidth = frame.displayWidth;
        createParams.Feature.InTargetHeight = frame.displayHeight;
        createParams.Feature.InPerfQualityValue = pq;
        createParams.InFeatureCreateFlags = createFlags;
        createParams.InEnableOutputSubrects = false;

        NVSDK_NGX_Result createRes = NGX_VULKAN_CREATE_DLSS_EXT1(
            ctx.device, cmd, 1u, 1u, &handle_, params_, &createParams);
        logNative("dlssSr: create in=%ux%u out=%ux%u pq=%d flags=0x%x preset=0x%x res=0x%08x handle=%p",
                  frame.renderWidth, frame.renderHeight, frame.displayWidth, frame.displayHeight,
                  static_cast<int>(pq), createFlags, presetVal, static_cast<unsigned>(createRes),
                  static_cast<void*>(handle_));
        if (NVSDK_NGX_FAILED(createRes) || !handle_) {
            outErrorCode = static_cast<int32_t>(createRes);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "NGX_VULKAN_CREATE_DLSS_EXT1 failed: 0x%08X", static_cast<unsigned>(createRes));
            outError = buf;
            handle_ = nullptr;
            return false;
        }
        key_ = wanted;
    }

    // Ensure proper Vulkan image layouts for NGX compute read/write
    ctx.cmdBarrier(cmd, colorIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
    ctx.cmdBarrier(cmd, colorOut, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    NVSDK_NGX_Resource_VK resColor = VkNgxContext::makeNgxResource(colorIn, false);
    NVSDK_NGX_Resource_VK resDepth = VkNgxContext::makeNgxResource(depthIn, false);
    NVSDK_NGX_Resource_VK resMvec = VkNgxContext::makeNgxResource(mvecIn, false);
    NVSDK_NGX_Resource_VK resOut = VkNgxContext::makeNgxResource(colorOut, true);

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams{};
    evalParams.Feature.pInColor = &resColor;
    evalParams.Feature.pInOutput = &resOut;
    evalParams.Feature.InSharpness = 0.0f;
    evalParams.pInDepth = &resDepth;
    evalParams.pInMotionVectors = &resMvec;
    evalParams.InJitterOffsetX = frame.jitterOffsetX;
    evalParams.InJitterOffsetY = frame.jitterOffsetY;
    evalParams.InRenderSubrectDimensions = {frame.renderWidth, frame.renderHeight};
    evalParams.InReset = frame.resetHistory ? 1 : 0;
    evalParams.InMVScaleX = (frame.mvScaleX != 0.0f) ? frame.mvScaleX : static_cast<float>(frame.renderWidth);
    evalParams.InMVScaleY = (frame.mvScaleY != 0.0f) ? frame.mvScaleY : static_cast<float>(frame.renderHeight);
    evalParams.InPreExposure = 1.0f;
    evalParams.InExposureScale = 1.0f;
    evalParams.InFrameTimeDeltaInMsec = (frame.frameTimeMs > 0.0f) ? frame.frameTimeMs : 16.666f;

    NVSDK_NGX_Result evalRes = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, handle_, params_, &evalParams);
    if (NVSDK_NGX_FAILED(evalRes)) {
        if (errLogCount_ < 8) {
            ++errLogCount_;
            logNative("dlssSr: evaluate FAILED res=0x%08x", static_cast<unsigned>(evalRes));
        }
        outErrorCode = static_cast<int32_t>(evalRes);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "NGX_VULKAN_EVALUATE_DLSS_EXT failed: 0x%08X", static_cast<unsigned>(evalRes));
        outError = buf;
        return false;
    }
    if (!loggedFirstEval_) {
        loggedFirstEval_ = true;
        logNative("dlssSr: first evaluate succeeded res=0x%08x preset=0x%x",
                  static_cast<unsigned>(evalRes), presetVal);
    }

    outActivePreset = static_cast<int32_t>(presetVal);
    return true;
}

void DlssSrEngine::destroy(VkNgxContext& ctx) {
    if (handle_) {
        if (ctx.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(ctx.device);
        }
        NVSDK_NGX_VULKAN_ReleaseFeature(handle_);
        handle_ = nullptr;
    }
    if (params_) {
        NVSDK_NGX_VULKAN_DestroyParameters(params_);
        params_ = nullptr;
    }
    key_ = {};
    loggedFirstEval_ = false;
    errLogCount_ = 0;
}

} // namespace linrtx
