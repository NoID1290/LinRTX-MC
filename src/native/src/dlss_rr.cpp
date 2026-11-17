#include "dlss_rr.hpp"
#include "dlss_sr.hpp"
#include "color_convert_spv.h"
#include "gbuffer_synth_spv.h"
#include "native_log.hpp"
#include "nvsdk_ngx_helpers_dlssd_vk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace linrtx {
namespace {

struct alignas(16) GuideSynthPush {
    uint32_t width;
    uint32_t height;
    uint32_t hasExplicitAlbedo;
    uint32_t hasExplicitNormals;
    uint32_t normalsAreWorld;
    uint32_t normalsPacked01;
    uint32_t reverseZ;
    uint32_t inputIsSrgb;
    float nearPlane;
    float farPlane;
    float invProjX;
    float invProjY;
    float viewRow0[4];
    float viewRow1[4];
    float viewRow2[4];
};
static_assert(sizeof(GuideSynthPush) == 96);

struct alignas(16) ColorConvertPush {
    uint32_t width;
    uint32_t height;
    uint32_t toLinear;
    uint32_t pad;
};
static_assert(sizeof(ColorConvertPush) == 16);

unsigned int normalizeRrPreset(int32_t preset) {
    switch (preset) {
        case LINRTX_RR_PRESET_D:
            return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D;
        case LINRTX_RR_PRESET_E:
            return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E;
        case LINRTX_RR_PRESET_F:
            return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F;
        default:
            return NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default;
    }
}

void applyRrPresetHints(NVSDK_NGX_Parameter* params, unsigned int presetVal) {
    if (!params) return;
    static constexpr const char* kKeys[] = {
        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality,
    };
    for (const char* key : kKeys) {
        NVSDK_NGX_Parameter_SetUI(params, key, presetVal);
    }
}

bool isUnorm8Format(GLenum fmt) {
    return fmt == kGlRgba8 || fmt == 0x8F97 /* GL_RGBA8_SNORM */ || fmt == 0x8C43 /* GL_SRGB8_ALPHA8 */;
}

void barrierReadOnly(VkNgxContext& ctx, VkCommandBuffer cmd, SharedTexture& tex) {
    ctx.cmdBarrier(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void barrierWriteGeneral(VkNgxContext& ctx, VkCommandBuffer cmd, SharedTexture& tex) {
    ctx.cmdBarrier(cmd, tex, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

} // namespace

bool DlssRrEngine::queryOptimalScale(VkNgxContext& ctx,
                                     int32_t dlssMode,
                                     uint32_t displayW,
                                     uint32_t displayH,
                                     uint32_t* outRenderW,
                                     uint32_t* outRenderH,
                                     float* outSharpness) {
    NVSDK_NGX_PerfQuality_Value pq = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ctx.ngxReady && ctx.dlssdSupported && ctx.ngxParams && toNgxPerfQuality(dlssMode, pq)) {
        void* cbPtr = nullptr;
        NVSDK_NGX_Parameter_GetVoidPointer(ctx.ngxParams, NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &cbPtr);
        if (cbPtr) {
            NVSDK_NGX_Parameter_SetUI(ctx.ngxParams, NVSDK_NGX_Parameter_Width, displayW);
            NVSDK_NGX_Parameter_SetUI(ctx.ngxParams, NVSDK_NGX_Parameter_Height, displayH);
            NVSDK_NGX_Parameter_SetI(ctx.ngxParams, NVSDK_NGX_Parameter_PerfQualityValue, pq);
            NVSDK_NGX_Parameter_SetI(ctx.ngxParams, NVSDK_NGX_Parameter_RTXValue, 0);
            auto cb = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(cbPtr);
            if (NVSDK_NGX_SUCCEED(cb(ctx.ngxParams))) {
                unsigned int optW = 0, optH = 0;
                float sharp = 0.0f;
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
    return DlssSrEngine::queryOptimalScale(ctx, dlssMode, displayW, displayH, outRenderW, outRenderH, outSharpness);
}

bool DlssRrEngine::ensureSynthResources(VkNgxContext& ctx, uint32_t w, uint32_t h) {
    if (!guideSynthPass_.valid()) {
        if (!guideSynthPass_.initMulti(ctx.device, kGbufferSynthSpv, sizeof(kGbufferSynthSpv),
                                       4u, 4u, sizeof(GuideSynthPush))) {
            return false;
        }
    }
    const VkImageUsageFlags fbUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (!dummyGuideTex_.valid()) {
        if (!ctx.createSharedTexture(dummyGuideTex_, 4u, 4u, VK_FORMAT_R16G16B16A16_SFLOAT, fbUsage, 0, false)) {
            return false;
        }
    }
    if (synthAlbedo_.valid() && synthAlbedo_.width == w && synthAlbedo_.height == h &&
        synthSpecular_.valid() && synthNormals_.valid() && synthRoughness_.valid()) {
        return true;
    }

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const bool ok =
        ctx.createSharedTexture(synthAlbedo_, w, h, VK_FORMAT_R8G8B8A8_UNORM, usage, 0, false) &&
        ctx.createSharedTexture(synthSpecular_, w, h, VK_FORMAT_R8G8B8A8_UNORM, usage, 0, false) &&
        ctx.createSharedTexture(synthNormals_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, usage, 0, false) &&
        ctx.createSharedTexture(synthRoughness_, w, h, VK_FORMAT_R16_SFLOAT, usage, 0, false);
    if (!ok) {
        ctx.destroySharedTexture(synthAlbedo_);
        ctx.destroySharedTexture(synthSpecular_);
        ctx.destroySharedTexture(synthNormals_);
        ctx.destroySharedTexture(synthRoughness_);
    }
    return ok;
}

bool DlssRrEngine::ensureHdrConvertResources(VkNgxContext& ctx,
                                             uint32_t renderW,
                                             uint32_t renderH,
                                             uint32_t displayW,
                                             uint32_t displayH) {
    if (!colorConvertPass_.valid()) {
        if (!colorConvertPass_.init(ctx.device, kColorConvertSpv, sizeof(kColorConvertSpv),
                                    sizeof(ColorConvertPush))) {
            return false;
        }
    }
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!synthColorHdr_.valid() || synthColorHdr_.width != renderW || synthColorHdr_.height != renderH) {
        if (!ctx.createSharedTexture(synthColorHdr_, renderW, renderH, VK_FORMAT_R16G16B16A16_SFLOAT, usage, 0, false)) {
            return false;
        }
    }
    if (!synthOutHdr_.valid() || synthOutHdr_.width != displayW || synthOutHdr_.height != displayH) {
        if (!ctx.createSharedTexture(synthOutHdr_, displayW, displayH, VK_FORMAT_R16G16B16A16_SFLOAT, usage, 0, false)) {
            return false;
        }
    }
    return true;
}

void DlssRrEngine::destroySynthResources(VkNgxContext& ctx) {
    if (ctx.device != VK_NULL_HANDLE) {
        guideSynthPass_.destroy(ctx.device);
        colorConvertPass_.destroy(ctx.device);
    }
    ctx.destroySharedTexture(synthAlbedo_);
    ctx.destroySharedTexture(synthSpecular_);
    ctx.destroySharedTexture(synthNormals_);
    ctx.destroySharedTexture(synthRoughness_);
    ctx.destroySharedTexture(synthColorHdr_);
    ctx.destroySharedTexture(synthOutHdr_);
    ctx.destroySharedTexture(dummyGuideTex_);
}

bool DlssRrEngine::evaluate(VkNgxContext& ctx,
                            VkCommandBuffer cmd,
                            const LinRtxFrameConfig& frame,
                            SharedTexture& colorIn,
                            SharedTexture& depthIn,
                            SharedTexture& mvecIn,
                            SharedTexture* explicitDiffuseAlbedo,
                            SharedTexture* explicitSpecularAlbedo,
                            SharedTexture* explicitNormals,
                            SharedTexture* explicitRoughness,
                            SharedTexture& colorOut,
                            int32_t& outActivePreset,
                            std::string& outError,
                            int32_t& outErrorCode) {
    if (!ctx.ngxReady || !ctx.dlssdSupported) {
        outError = "DLSS Ray Reconstruction (DLSSD) is not available on this NGX runtime";
        outErrorCode = -1;
        return false;
    }

    const NVSDK_NGX_PerfQuality_Value pq = resolveNgxPerfQuality(
        frame.dlssMode, frame.renderWidth, frame.renderHeight, frame.displayWidth, frame.displayHeight);

    const unsigned int presetVal = normalizeRrPreset(frame.rrPreset);

    // DLSS-RR Transformer (dlssd_transformer) requires linear HDR color + IsHDR flag;
    // when MC/Iris input is LDR sRGB (RGBA8), we convert sRGB -> linear FP16 before DLSSD
    // and linear FP16 -> sRGB afterward so color & albedo are consistently linear.
    int createFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                      NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                      NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (frame.depthInverted != 0) {
        createFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
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
            outError = "NVSDK_NGX_VULKAN_AllocateParameters (DLSS-RR) failed";
            params_ = nullptr;
            return false;
        }

        NVSDK_NGX_Parameter_SetI(params_, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
        applyRrPresetHints(params_, presetVal);

        NVSDK_NGX_DLSSD_Create_Params createParams{};
        createParams.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        createParams.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
        createParams.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        createParams.InWidth = frame.renderWidth;
        createParams.InHeight = frame.renderHeight;
        createParams.InTargetWidth = frame.displayWidth;
        createParams.InTargetHeight = frame.displayHeight;
        createParams.InPerfQualityValue = pq;
        createParams.InFeatureCreateFlags = createFlags;
        createParams.InEnableOutputSubrects = false;

        NVSDK_NGX_Result createRes = NGX_VULKAN_CREATE_DLSSD_EXT1(
            ctx.device, cmd, 1u, 1u, &handle_, params_, &createParams);
        logNative("dlssRr: create in=%ux%u out=%ux%u pq=%d flags=0x%x preset=0x%x res=0x%08x handle=%p",
                  frame.renderWidth, frame.renderHeight, frame.displayWidth, frame.displayHeight,
                  static_cast<int>(pq), createFlags, presetVal, static_cast<unsigned>(createRes),
                  static_cast<void*>(handle_));
        if (NVSDK_NGX_FAILED(createRes) || !handle_) {
            outErrorCode = static_cast<int32_t>(createRes);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "NGX_VULKAN_CREATE_DLSSD_EXT1 failed: 0x%08X", static_cast<unsigned>(createRes));
            outError = buf;
            handle_ = nullptr;
            return false;
        }
        key_ = wanted;
    }

    if (!ensureSynthResources(ctx, frame.renderWidth, frame.renderHeight)) {
        outError = "Failed to initialize DLSS-RR guide buffers";
        outErrorCode = -2;
        return false;
    }
    const bool convertLdrToHdr = (frame.colorIsHdrInput == 0);
    if (convertLdrToHdr &&
        !ensureHdrConvertResources(ctx, frame.renderWidth, frame.renderHeight,
                                   frame.displayWidth, frame.displayHeight)) {
        outError = "Failed to initialize DLSS-RR HDR color conversion buffers";
        outErrorCode = -2;
        return false;
    }

    barrierReadOnly(ctx, cmd, colorIn);
    barrierReadOnly(ctx, cmd, depthIn);
    barrierReadOnly(ctx, cmd, mvecIn);
    if (dummyGuideTex_.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrierReadOnly(ctx, cmd, dummyGuideTex_);
    }

    const bool hasExpAlbedo = (explicitDiffuseAlbedo && explicitDiffuseAlbedo->valid());
    const bool hasExpNormals = (explicitNormals && explicitNormals->valid());
    if (hasExpAlbedo) barrierReadOnly(ctx, cmd, *explicitDiffuseAlbedo);
    if (hasExpNormals) barrierReadOnly(ctx, cmd, *explicitNormals);

    // Always run guide synthesis/normalization into unpacked view-space G-buffers so any mix of
    // explicit (Camellia/Iris) and fallback (depth-derived) guides is 100% consistent.
    barrierWriteGeneral(ctx, cmd, synthAlbedo_);
    barrierWriteGeneral(ctx, cmd, synthSpecular_);
    barrierWriteGeneral(ctx, cmd, synthNormals_);
    barrierWriteGeneral(ctx, cmd, synthRoughness_);

    const float aspect = (frame.cameraAspect > 0.05f)
                             ? frame.cameraAspect
                             : (static_cast<float>(frame.renderWidth) / std::max(1.0f, static_cast<float>(frame.renderHeight)));
    const float fov = (frame.cameraFovRadians > 0.05f) ? frame.cameraFovRadians : 1.22173f;
    const float tanHalf = std::tan(fov * 0.5f);
    const float projM00 = std::abs(frame.viewToClip[0]);
    const float projM11 = std::abs(frame.viewToClip[5]);

    GuideSynthPush gpc{};
    gpc.width = frame.renderWidth;
    gpc.height = frame.renderHeight;
    gpc.hasExplicitAlbedo = hasExpAlbedo ? 1u : 0u;
    gpc.hasExplicitNormals = hasExpNormals ? 1u : 0u;
    gpc.normalsAreWorld = frame.gbufferNormalsAreWorld ? 1u : 0u;
    gpc.normalsPacked01 = (hasExpNormals && isUnorm8Format(frame.normalsFormatGl)) ? 1u : 0u;
    gpc.reverseZ = frame.depthInverted ? 1u : 0u;
    gpc.inputIsSrgb = (frame.colorIsHdrInput == 0) ? 1u : 0u;
    gpc.nearPlane = (frame.cameraNear > 0.0f) ? frame.cameraNear : 0.05f;
    gpc.farPlane = (frame.cameraFar > gpc.nearPlane) ? frame.cameraFar : 256.0f;
    gpc.invProjX = (projM00 > 1e-5f) ? (1.0f / projM00) : (aspect * tanHalf);
    gpc.invProjY = (projM11 > 1e-5f) ? (1.0f / projM11) : tanHalf;
    gpc.viewRow0[0] = frame.cameraRight[0]; gpc.viewRow0[1] = frame.cameraRight[1]; gpc.viewRow0[2] = frame.cameraRight[2];
    gpc.viewRow1[0] = frame.cameraUp[0];    gpc.viewRow1[1] = frame.cameraUp[1];    gpc.viewRow1[2] = frame.cameraUp[2];
    gpc.viewRow2[0] = -frame.cameraFwd[0];  gpc.viewRow2[1] = -frame.cameraFwd[1];  gpc.viewRow2[2] = -frame.cameraFwd[2];

    const SharedTexture* gSamplers[4] = {
        &colorIn,
        &depthIn,
        hasExpAlbedo ? explicitDiffuseAlbedo : &dummyGuideTex_,
        hasExpNormals ? explicitNormals : &dummyGuideTex_,
    };
    const SharedTexture* gStorages[4] = {
        &synthAlbedo_,
        &synthSpecular_,
        &synthNormals_,
        &synthRoughness_,
    };
    guideSynthPass_.dispatchMulti(cmd, ctx.device, gSamplers, 4u, gStorages, 4u,
                                  frame.renderWidth, frame.renderHeight, &gpc, sizeof(gpc));

    SharedTexture* usedDiffuse = &synthAlbedo_;
    SharedTexture* usedSpecular = (explicitSpecularAlbedo && explicitSpecularAlbedo->valid())
                                      ? explicitSpecularAlbedo
                                      : &synthSpecular_;
    SharedTexture* usedNormals = &synthNormals_;
    SharedTexture* usedRoughness = (explicitRoughness && explicitRoughness->valid())
                                       ? explicitRoughness
                                       : &synthRoughness_;

    barrierReadOnly(ctx, cmd, *usedDiffuse);
    barrierReadOnly(ctx, cmd, *usedSpecular);
    barrierReadOnly(ctx, cmd, *usedNormals);
    barrierReadOnly(ctx, cmd, *usedRoughness);
    SharedTexture* rrColorIn = &colorIn;
    SharedTexture* rrColorOut = &colorOut;
    if (convertLdrToHdr) {
        barrierWriteGeneral(ctx, cmd, synthColorHdr_);
        ColorConvertPush cpc{frame.renderWidth, frame.renderHeight, 1u, 0u};
        colorConvertPass_.dispatch(cmd, ctx.device, colorIn, synthColorHdr_,
                                   frame.renderWidth, frame.renderHeight, &cpc, sizeof(cpc));
        barrierReadOnly(ctx, cmd, synthColorHdr_);
        barrierWriteGeneral(ctx, cmd, synthOutHdr_);
        rrColorIn = &synthColorHdr_;
        rrColorOut = &synthOutHdr_;
    } else {
        barrierWriteGeneral(ctx, cmd, colorOut);
    }

    NVSDK_NGX_Resource_VK resColor = VkNgxContext::makeNgxResource(*rrColorIn, false);
    NVSDK_NGX_Resource_VK resDepth = VkNgxContext::makeNgxResource(depthIn, false);
    NVSDK_NGX_Resource_VK resMvec = VkNgxContext::makeNgxResource(mvecIn, false);
    NVSDK_NGX_Resource_VK resDiffuse = VkNgxContext::makeNgxResource(*usedDiffuse, false);
    NVSDK_NGX_Resource_VK resSpec = VkNgxContext::makeNgxResource(*usedSpecular, false);
    NVSDK_NGX_Resource_VK resNormals = VkNgxContext::makeNgxResource(*usedNormals, false);
    NVSDK_NGX_Resource_VK resRoughness = VkNgxContext::makeNgxResource(*usedRoughness, false);
    NVSDK_NGX_Resource_VK resOut = VkNgxContext::makeNgxResource(*rrColorOut, true);

    static float kIdentityMat[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    float viewToClipCopy[16]{};
    std::memcpy(viewToClipCopy, frame.viewToClip, sizeof(viewToClipCopy));

    NVSDK_NGX_VK_DLSSD_Eval_Params evalParams{};
    evalParams.pInColor = &resColor;
    evalParams.pInOutput = &resOut;
    evalParams.pInDepth = &resDepth;
    evalParams.pInMotionVectors = &resMvec;
    evalParams.pInDiffuseAlbedo = &resDiffuse;
    evalParams.pInSpecularAlbedo = &resSpec;
    evalParams.pInNormals = &resNormals;
    evalParams.pInRoughness = &resRoughness;
    evalParams.pInWorldToViewMatrix = kIdentityMat;
    evalParams.pInViewToClipMatrix = viewToClipCopy;
    evalParams.InToneMapperType = NVSDK_NGX_TONEMAPPER_ACES;
    evalParams.InJitterOffsetX = frame.jitterOffsetX;
    evalParams.InJitterOffsetY = frame.jitterOffsetY;
    evalParams.InRenderSubrectDimensions = {frame.renderWidth, frame.renderHeight};
    evalParams.InReset = frame.resetHistory ? 1 : 0;
    evalParams.InMVScaleX = (frame.mvScaleX != 0.0f) ? frame.mvScaleX : static_cast<float>(frame.renderWidth);
    evalParams.InMVScaleY = (frame.mvScaleY != 0.0f) ? frame.mvScaleY : static_cast<float>(frame.renderHeight);
    evalParams.InPreExposure = 1.0f;
    evalParams.InExposureScale = 1.0f;
    evalParams.InFrameTimeDeltaInMsec = (frame.frameTimeMs > 0.0f) ? frame.frameTimeMs : 16.666f;

    NVSDK_NGX_Result evalRes = NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd, handle_, params_, &evalParams);
    if (NVSDK_NGX_FAILED(evalRes)) {
        if (errLogCount_ < 8) {
            ++errLogCount_;
            logNative("dlssRr: evaluate FAILED res=0x%08x", static_cast<unsigned>(evalRes));
        }
        outErrorCode = static_cast<int32_t>(evalRes);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "NGX_VULKAN_EVALUATE_DLSSD_EXT failed: 0x%08X", static_cast<unsigned>(evalRes));
        outError = buf;
        return false;
    }

    if (convertLdrToHdr) {
        barrierReadOnly(ctx, cmd, synthOutHdr_);
        barrierWriteGeneral(ctx, cmd, colorOut);
        ColorConvertPush cpc{frame.displayWidth, frame.displayHeight, 0u, 0u};
        colorConvertPass_.dispatch(cmd, ctx.device, synthOutHdr_, colorOut,
                                   frame.displayWidth, frame.displayHeight, &cpc, sizeof(cpc));
    }

    if (!loggedFirstEval_) {
        loggedFirstEval_ = true;
        logNative("dlssRr: first evaluate succeeded res=0x%08x preset=0x%x (guides: explicitAlb=%d explicitNorm=%d)",
                  static_cast<unsigned>(evalRes), presetVal, hasExpAlbedo ? 1 : 0, hasExpNormals ? 1 : 0);
    }

    outActivePreset = static_cast<int32_t>(presetVal);
    return true;
}

void DlssRrEngine::destroy(VkNgxContext& ctx) {
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
    destroySynthResources(ctx);
    key_ = {};
    loggedFirstEval_ = false;
    errLogCount_ = 0;
}

} // namespace linrtx
