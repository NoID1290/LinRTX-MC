#include "dlss_fg.hpp"
#include "native_log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace linrtx {
namespace {

uint32_t desiredGeneratedCount(int32_t fgMode, int32_t maxMultiplier) {
    uint32_t requested = 0;
    switch (fgMode) {
        case LINRTX_FG_2X:
            requested = 1;
            break;
        case LINRTX_FG_3X:
            requested = 2;
            break;
        case LINRTX_FG_4X:
            requested = 3;
            break;
        default:
            return 0;
    }
    const uint32_t supportedMaxGen = static_cast<uint32_t>(std::max(1, maxMultiplier - 1));
    return std::clamp(requested, 1u, std::min(supportedMaxGen, DlssFgEngine::kMaxGeneratedFrames));
}

void copyMatrix4x4(float dst[4][4], const float src[16]) {
    std::memcpy(&dst[0][0], src, sizeof(float) * 16);
}

} // namespace

void DlssFgEngine::resetHistory() {
    historyFrames_ = 0;
}

bool DlssFgEngine::evaluate(VkNgxContext& ctx,
                            VkCommandBuffer cmd,
                            const LinRtxFrameConfig& frame,
                            SharedTexture& backbuffer,
                            SharedTexture* hudlessOpt,
                            SharedTexture& depth,
                            SharedTexture& mvec,
                            std::array<SharedTexture, kMaxGeneratedFrames>& interpOutputs,
                            uint32_t& outGeneratedCount,
                            std::string& outError,
                            int32_t& outErrorCode) {
    outGeneratedCount = 0;

    if (!ctx.ngxReady || !ctx.dlssgSupported) {
        outError = "DLSS-G Frame Generation is not supported on this GPU/driver";
        outErrorCode = -1;
        return false;
    }

    const uint32_t genCount = desiredGeneratedCount(frame.fgMode, ctx.maxFgMultiplier);
    if (genCount == 0) {
        return true;
    }

    const bool useHudless = (hudlessOpt != nullptr && hudlessOpt->valid() && frame.useHudlessSplit != 0);
    FeatureKey wanted{
        frame.displayWidth,
        frame.displayHeight,
        depth.width,
        depth.height,
        static_cast<uint32_t>(backbuffer.vkFormat),
        useHudless,
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
            outError = "NVSDK_NGX_VULKAN_AllocateParameters (DLSS-G) failed";
            params_ = nullptr;
            return false;
        }
        historyFrames_ = 0;

        uint32_t alwaysMask = NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer |
                              NVSDK_NGX_DLSSG_ResourceFlags_MVecs |
                              NVSDK_NGX_DLSSG_ResourceFlags_Depth |
                              NVSDK_NGX_DLSSG_ResourceFlags_OutputInterpolated;
        uint32_t neverMask = NVSDK_NGX_DLSSG_ResourceFlags_UI |
                             NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
                             NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField |
                             NVSDK_NGX_DLSSG_ResourceFlags_OutputReal |
                             NVSDK_NGX_DLSSG_ResourceFlags_OutputDisableInterpolation;
        if (useHudless) {
            alwaysMask |= NVSDK_NGX_DLSSG_ResourceFlags_HUDLess;
        } else {
            neverMask |= NVSDK_NGX_DLSSG_ResourceFlags_HUDLess;
        }

        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_Width, frame.displayWidth);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_Height, frame.displayHeight);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_InternalWidth, depth.width);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_InternalHeight, depth.height);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, alwaysMask);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, neverMask);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 0u);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 1u);

        NVSDK_NGX_DLSSG_Create_Params cp{};
        cp.Width = frame.displayWidth;
        cp.Height = frame.displayHeight;
        cp.NativeBackbufferFormat = static_cast<unsigned int>(backbuffer.vkFormat);
        cp.RenderWidth = depth.width;
        cp.RenderHeight = depth.height;
        cp.DynamicResolutionScaling = (depth.width != frame.displayWidth || depth.height != frame.displayHeight);

        NVSDK_NGX_Result createRes = NGX_VK_CREATE_DLSSG(cmd, 1u, 1u, &handle_, params_, &cp);
        logNative("dlssFg: create disp=%ux%u internal=%ux%u vkFmt=%u hudless=%d res=0x%08x handle=%p",
                  frame.displayWidth, frame.displayHeight, depth.width, depth.height,
                  static_cast<unsigned>(backbuffer.vkFormat), useHudless ? 1 : 0,
                  static_cast<unsigned>(createRes), static_cast<void*>(handle_));
        if (NVSDK_NGX_FAILED(createRes) || !handle_) {
            outErrorCode = static_cast<int32_t>(createRes);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "NGX_VK_CREATE_DLSSG failed: 0x%08X", static_cast<unsigned>(createRes));
            outError = buf;
            handle_ = nullptr;
            return false;
        }
        key_ = wanted;
    }

    // Prepare input layouts
    ctx.cmdBarrier(cmd, backbuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    ctx.cmdBarrier(cmd, mvec, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (useHudless) {
        ctx.cmdBarrier(cmd, *hudlessOpt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    NVSDK_NGX_Resource_VK resBackbuffer = VkNgxContext::makeNgxResource(backbuffer, false);
    NVSDK_NGX_Resource_VK resDepth = VkNgxContext::makeNgxResource(depth, false);
    NVSDK_NGX_Resource_VK resMvec = VkNgxContext::makeNgxResource(mvec, false);
    NVSDK_NGX_Resource_VK resHudless{};
    if (useHudless) {
        resHudless = VkNgxContext::makeNgxResource(*hudlessOpt, false);
    }

    const bool doReset = (frame.resetHistory != 0) || (historyFrames_ == 0);

    NVSDK_NGX_DLSSG_Opt_Eval_Params opt{};
    opt.multiFrameCount = genCount;
    copyMatrix4x4(opt.cameraViewToClip, frame.viewToClip);
    copyMatrix4x4(opt.clipToCameraView, frame.clipToView);
    // Identity lens matrix unless lens distortion is used
    std::memset(opt.clipToLensClip, 0, sizeof(opt.clipToLensClip));
    opt.clipToLensClip[0][0] = 1.0f;
    opt.clipToLensClip[1][1] = 1.0f;
    opt.clipToLensClip[2][2] = 1.0f;
    opt.clipToLensClip[3][3] = 1.0f;
    copyMatrix4x4(opt.clipToPrevClip, frame.clipToPrevClip);
    copyMatrix4x4(opt.prevClipToClip, frame.prevClipToClip);

    opt.jitterOffset[0] = frame.jitterOffsetX;
    opt.jitterOffset[1] = frame.jitterOffsetY;
    // Motion vectors stored in normalized [-1, 1] UV displacement units (prevUV - currUV)
    opt.mvecScale[0] = 1.0f;
    opt.mvecScale[1] = 1.0f;
    opt.cameraPinholeOffset[0] = 0.0f;
    opt.cameraPinholeOffset[1] = 0.0f;

    std::memcpy(opt.cameraPos, frame.cameraPos, sizeof(float) * 3);
    std::memcpy(opt.cameraUp, frame.cameraUp, sizeof(float) * 3);
    std::memcpy(opt.cameraRight, frame.cameraRight, sizeof(float) * 3);
    std::memcpy(opt.cameraFwd, frame.cameraFwd, sizeof(float) * 3);

    opt.cameraNear = frame.cameraNear;
    opt.cameraFar = frame.cameraFar;
    opt.cameraFOV = frame.cameraFovRadians;
    opt.cameraAspectRatio = frame.cameraAspect;

    opt.colorBuffersHDR = (frame.hdrOutputMode != LINRTX_HDR_OUT_SDR_BOOST) || (frame.colorIsHdrInput != 0);
    opt.depthInverted = (frame.depthInverted != 0);
    opt.cameraMotionIncluded = true;
    opt.reset = doReset;
    opt.automodeOverrideReset = false;
    opt.notRenderingGameFrames = false;
    opt.orthoProjection = false;
    opt.motionVectorsInvalidValue = -1e10f;
    opt.motionVectorsDilated = false;
    opt.menuDetectionEnabled = true;

    opt.mvecsSubrectBase = {0, 0};
    opt.mvecsSubrectSize = {depth.width, depth.height};
    opt.depthSubrectBase = {0, 0};
    opt.depthSubrectSize = {depth.width, depth.height};
    opt.backbufferSubrectBase = {0, 0};
    opt.backbufferSubrectSize = {frame.displayWidth, frame.displayHeight};
    opt.outputInterpSubrectBase = {0, 0};
    opt.outputInterpSubrectSize = {frame.displayWidth, frame.displayHeight};
    if (useHudless) {
        opt.hudLessSubrectBase = {0, 0};
        opt.hudLessSubrectSize = {hudlessOpt->width, hudlessOpt->height};
    }

    frameCounter_++;
    NVSDK_NGX_Parameter_SetULL(params_, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, frameCounter_);

    uint32_t produced = 0;
    for (uint32_t idx = 0; idx < genCount; ++idx) {
        SharedTexture& dst = interpOutputs[idx];
        if (!dst.valid()) {
            outError = "DLSS-G output texture is not allocated";
            outErrorCode = -2;
            return false;
        }
        ctx.cmdBarrier(cmd, dst, VK_IMAGE_LAYOUT_GENERAL,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        NVSDK_NGX_Resource_VK resOut = VkNgxContext::makeNgxResource(dst, true);

        NVSDK_NGX_VK_DLSSG_Eval_Params ev{};
        ev.pBackbuffer = &resBackbuffer;
        ev.pDepth = &resDepth;
        ev.pMVecs = &resMvec;
        ev.pHudless = useHudless ? &resHudless : nullptr;
        ev.pOutputInterpFrame = &resOut;

        opt.multiFrameIndex = idx + 1u;

        NVSDK_NGX_Result evRes = NGX_VK_EVALUATE_DLSSG(cmd, handle_, params_, &ev, &opt);
        if (NVSDK_NGX_FAILED(evRes)) {
            if (errLogCount_ < 8) {
                ++errLogCount_;
                logNative("dlssFg: evaluate FAILED idx=%u res=0x%08x",
                          idx + 1u, static_cast<unsigned>(evRes));
            }
            outErrorCode = static_cast<int32_t>(evRes);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "NGX_VK_EVALUATE_DLSSG (idx=%u) failed: 0x%08X", idx + 1u, static_cast<unsigned>(evRes));
            outError = buf;
            return false;
        }
        produced++;
    }

    if (doReset) {
        // First real frame seeds history; emit 0 interpolated frames cleanly until 2nd real frame
        historyFrames_ = 1;
        outGeneratedCount = 0;
    } else {
        historyFrames_++;
        outGeneratedCount = produced;
    }
    if (!loggedFirstEval_) {
        loggedFirstEval_ = true;
        logNative("dlssFg: first evaluate succeeded genReq=%u produced=%u doReset=%d emitted=%u",
                  genCount, produced, doReset ? 1 : 0, outGeneratedCount);
    }
    return true;
}

void DlssFgEngine::destroy(VkNgxContext& ctx) {
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
    historyFrames_ = 0;
    loggedFirstEval_ = false;
    errLogCount_ = 0;
}

} // namespace linrtx
