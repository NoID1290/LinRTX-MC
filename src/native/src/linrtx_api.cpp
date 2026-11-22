#include "linrtx_api.h"

#include "dlss_fg.hpp"
#include "dlss_nr.hpp"
#include "pre_tonemap_helper.hpp"
#include "dlss_rr.hpp"
#include "dlss_sr.hpp"
#include "native_log.hpp"
#include "ngx_worker.hpp"
#include "rtx_hdr.hpp"
#include "svgf_denoiser.hpp"
#include "vk_ngx_context.hpp"
#include "vk_presenter.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using namespace linrtx;

void mapGlFormatToVk(GLenum glFmt, VkFormat fallbackVk, GLenum fallbackGl, VkFormat& outVk, GLenum& outGl) {
    switch (glFmt) {
        case 0x8814: // GL_RGBA32F
            outVk = VK_FORMAT_R32G32B32A32_SFLOAT;
            outGl = 0x8814;
            return;
        case kGlRgba16f: // 0x881A GL_RGBA16F
            outVk = VK_FORMAT_R16G16B16A16_SFLOAT;
            outGl = kGlRgba16f;
            return;
        case kGlRgba8: // 0x8058 GL_RGBA8
        case 0x8C43:   // GL_SRGB8_ALPHA8
            outVk = VK_FORMAT_R8G8B8A8_UNORM;
            outGl = kGlRgba8;
            return;
        case 0x8F97: // GL_RGBA8_SNORM
            outVk = VK_FORMAT_R8G8B8A8_SNORM;
            outGl = 0x8F97;
            return;
        case kGlRgb10A2: // 0x8059 GL_RGB10_A2
            outVk = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            outGl = kGlRgb10A2;
            return;
        case kGlR32f: // 0x822E GL_R32F
            outVk = VK_FORMAT_R32_SFLOAT;
            outGl = kGlR32f;
            return;
        case 0x822D: // GL_R16F
            outVk = VK_FORMAT_R16_SFLOAT;
            outGl = 0x822D;
            return;
        case kGlRg16f: // 0x822F GL_RG16F
            outVk = VK_FORMAT_R16G16_SFLOAT;
            outGl = kGlRg16f;
            return;
        case 0x8230: // GL_RG32F
            outVk = VK_FORMAT_R32G32_SFLOAT;
            outGl = 0x8230;
            return;
        case 0x8229: // GL_R8
            outVk = VK_FORMAT_R8_UNORM;
            outGl = 0x8229;
            return;
        default:
            outVk = fallbackVk;
            outGl = fallbackGl;
            return;
    }
}

struct EngineState {
    std::mutex mu;
    bool initialized = false;
    LinRtxInitConfig initCfg{};
    std::string initNgxDir;
    std::string initHdrDirs;
    std::string initLogDir;
    std::string initNeuralDirs;

    VkNgxContext ctx;
    SvgfDenoiser svgf;
    PreTonemapHelper preTonemap;
    DlssSrEngine dlssSr;
    DlssRrEngine dlssRr;
    DlssFgEngine dlssFg;
    DlssNrEngine dlssNr;
    RtxHdrEngine rtxHdr;
    VkPresenter presenter;

    // Transient per-resolution SharedTextures
    SharedTexture texColorIn;
    SharedTexture texDepthIn;
    SharedTexture texMvecIn;
    SharedTexture texDenoiseOut;
    SharedTexture texNeuralPreOut;
    SharedTexture texDiffuseAlbedoIn;
    SharedTexture texSpecularAlbedoIn;
    SharedTexture texNormalsIn;
    SharedTexture texRoughnessIn;
    SharedTexture texDlssOut;
    SharedTexture texCasOut;
    SharedTexture texNeuralOut;
    SharedTexture texBackbufferIn;
    SharedTexture texHudlessIn;
    SharedTexture texTrueHdrPqTmp;
    SharedTexture texHdrOut;
    SharedTexture texHdrHudlessOut;
    std::array<SharedTexture, DlssFgEngine::kMaxGeneratedFrames> texInterpOut{};

    // Pre-tonemap Iris HDR denoiser SharedTextures
    SharedTexture texPreHdrIn;
    SharedTexture texPreHdrOut;
    SharedTexture texPreDepthIn;
    SharedTexture texPreMvecIn;
    SharedTexture texPreAlbedoIn;
    SharedTexture texPreNormalsIn;

    SharedTexture* lastRealFrameTex = nullptr;
    uint32_t lastGeneratedCount = 0;
    bool depthMvecFreshThisFrame = false;

    LinRtxStatus status{};

    void clearStatusError() {
        status.lastErrorCode = 0;
        status.lastErrorMsg[0] = '\0';
    }

    void setStatusError(int32_t code, const std::string& msg) {
        status.lastErrorCode = code;
        std::snprintf(status.lastErrorMsg, sizeof(status.lastErrorMsg), "%s", msg.c_str());
    }

    bool ensureTex(SharedTexture& tex,
                   uint32_t w,
                   uint32_t h,
                   VkFormat vkFmt,
                   VkImageUsageFlags usage,
                   GLenum glFmt,
                   bool exportToGl) {
        if (tex.valid() && tex.width == w && tex.height == h && tex.vkFormat == vkFmt &&
            ((tex.glTex != 0) == exportToGl)) {
            return true;
        }
        return ctx.createSharedTexture(tex, w, h, vkFmt, usage, glFmt, exportToGl);
    }

    void destroyAllTextures() {
        ctx.destroySharedTexture(texColorIn);
        ctx.destroySharedTexture(texDepthIn);
        ctx.destroySharedTexture(texMvecIn);
        ctx.destroySharedTexture(texDenoiseOut);
        ctx.destroySharedTexture(texNeuralPreOut);
        ctx.destroySharedTexture(texDiffuseAlbedoIn);
        ctx.destroySharedTexture(texSpecularAlbedoIn);
        ctx.destroySharedTexture(texNormalsIn);
        ctx.destroySharedTexture(texRoughnessIn);
        ctx.destroySharedTexture(texDlssOut);
        ctx.destroySharedTexture(texCasOut);
        ctx.destroySharedTexture(texNeuralOut);
        ctx.destroySharedTexture(texBackbufferIn);
        ctx.destroySharedTexture(texHudlessIn);
        ctx.destroySharedTexture(texTrueHdrPqTmp);
        ctx.destroySharedTexture(texHdrOut);
        ctx.destroySharedTexture(texHdrHudlessOut);
        ctx.destroySharedTexture(texPreHdrIn);
        ctx.destroySharedTexture(texPreHdrOut);
        ctx.destroySharedTexture(texPreDepthIn);
        ctx.destroySharedTexture(texPreMvecIn);
        ctx.destroySharedTexture(texPreAlbedoIn);
        ctx.destroySharedTexture(texPreNormalsIn);
        for (auto& t : texInterpOut) {
            ctx.destroySharedTexture(t);
        }
        lastRealFrameTex = nullptr;
    }

    void determineStage2Format(const LinRtxFrameConfig& frame, VkFormat& outVk, GLenum& outGl) {
        if (frame.hdrEngine != LINRTX_HDR_OFF) {
            if (frame.hdrOutputMode == LINRTX_HDR_OUT_PQ_BT2020) {
                outVk = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
                outGl = kGlRgb10A2;
                return;
            }
            if (frame.hdrOutputMode == LINRTX_HDR_OUT_SCRGB_FP16) {
                outVk = VK_FORMAT_R16G16B16A16_SFLOAT;
                outGl = kGlRgba16f;
                return;
            }
            outVk = VK_FORMAT_R8G8B8A8_UNORM;
            outGl = kGlRgba8;
            return;
        }
        if (frame.colorIsHdrInput != 0) {
            outVk = VK_FORMAT_R16G16B16A16_SFLOAT;
            outGl = kGlRgba16f;
            return;
        }
        outVk = VK_FORMAT_R8G8B8A8_UNORM;
        outGl = kGlRgba8;
    }
};

EngineState& engine() {
    static EngineState s;
    return s;
}

} // namespace

extern "C" {

int32_t linrtx_init(const LinRtxInitConfig* config) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    // Tear down any previous session cleanly
    if (s.initialized) {
        s.presenter.destroy(s.ctx);
        s.destroyAllTextures();
        ngxWorker().run([&]() {
            s.svgf.destroy(s.ctx);
            s.dlssNr.destroy(s.ctx);
            s.rtxHdr.destroy(s.ctx);
            s.dlssFg.destroy(s.ctx);
            s.dlssRr.destroy(s.ctx);
            s.dlssSr.destroy(s.ctx);
        });
        s.ctx.shutdown();
        ngxWorker().shutdown();
        s.initialized = false;
    }

    s.status = {};
    s.status.structSize = sizeof(LinRtxStatus);

    if (!config || config->structSize != sizeof(LinRtxInitConfig)) {
        s.setStatusError(-10, "Invalid LinRtxInitConfig structSize");
        return -10;
    }

    s.initCfg = *config;
    s.initNgxDir = config->ngxLibDir ? config->ngxLibDir : "";
    s.initHdrDirs = config->trueHdrSearchDirs ? config->trueHdrSearchDirs : "";
    s.initLogDir = config->logDir ? config->logDir : "/tmp";
    s.initNeuralDirs = (config->neuralSearchDirs && *config->neuralSearchDirs)
                           ? config->neuralSearchDirs
                           : s.initHdrDirs;
    s.initCfg.ngxLibDir = s.initNgxDir.c_str();
    s.initCfg.trueHdrSearchDirs = s.initHdrDirs.c_str();
    s.initCfg.logDir = s.initLogDir.c_str();
    s.initCfg.neuralSearchDirs = s.initNeuralDirs.c_str();

    initNativeLog(s.initCfg.logDir);
    logNative("linrtx_init: validation=%u isWayland=%u ngxLibDir='%s' hdrSearch='%s' neuralSearch='%s'",
              s.initCfg.enableValidation, s.initCfg.isWayland,
              s.initCfg.ngxLibDir, s.initCfg.trueHdrSearchDirs, s.initCfg.neuralSearchDirs);

    if (!s.ctx.init(&s.initCfg)) {
        logNative("linrtx_init: ctx.init FAILED code=%d err='%s'",
                  s.ctx.lastErrorCode, s.ctx.lastError.c_str());
        s.setStatusError(s.ctx.lastErrorCode ? s.ctx.lastErrorCode : -1, s.ctx.lastError);
        std::snprintf(s.status.gpuName, sizeof(s.status.gpuName), "%s", s.ctx.gpuName.c_str());
        return -1;
    }

    s.rtxHdr.init(s.ctx, s.initHdrDirs.c_str());
    s.dlssNr.init(s.ctx, s.initNeuralDirs.c_str());

    s.initialized = true;
    s.status.vkReady = s.ctx.vkReady ? 1 : 0;
    s.status.ngxReady = s.ctx.ngxReady ? 1 : 0;
    s.status.dlssSupported = s.ctx.dlssSupported ? 1 : 0;
    s.status.dlssdSupported = s.ctx.dlssdSupported ? 1 : 0;
    s.status.dlssgSupported = s.ctx.dlssgSupported ? 1 : 0;
    s.status.trueHdrNgxLoaded = s.rtxHdr.trueHdrLibraryPresent() ? 1 : 0;
    s.status.maxFgMultiplier = s.ctx.maxFgMultiplier;
    s.status.dlssnrNgxLoaded = s.dlssNr.ngxLibraryPresent() ? 1 : 0;
    s.status.dlssnrNgxSupported = s.ctx.dlssnrSupported ? 1 : 0;
    s.status.dlssnrUnlocked = (s.ctx.pciVendorId == 0x10DE) ? 1 : 0;
    std::snprintf(s.status.gpuName, sizeof(s.status.gpuName), "%s", s.ctx.gpuName.c_str());
    std::snprintf(s.status.gpuArchName, sizeof(s.status.gpuArchName), "%s", s.ctx.gpuArchName.c_str());
    std::snprintf(s.status.driverVersion, sizeof(s.status.driverVersion), "%s", s.ctx.driverVersion.c_str());
    std::snprintf(s.status.dlssVersion, sizeof(s.status.dlssVersion), "%s", s.dlssSr.versionString().c_str());
    std::snprintf(s.status.dlssdVersion, sizeof(s.status.dlssdVersion), "%s", s.dlssRr.versionString().c_str());
    std::snprintf(s.status.dlssgVersion, sizeof(s.status.dlssgVersion), "%s", s.dlssFg.versionString().c_str());
    std::snprintf(s.status.dlssnrVersion, sizeof(s.status.dlssnrVersion), "%s", s.dlssNr.versionString().c_str());
    std::snprintf(s.status.dlssnrStatus, sizeof(s.status.dlssnrStatus), "%s", s.dlssNr.statusString().c_str());
    std::snprintf(s.status.trueHdrStatus, sizeof(s.status.trueHdrStatus), "%s", s.rtxHdr.statusString().c_str());
    std::snprintf(s.status.denoiserStatus, sizeof(s.status.denoiserStatus), "Ready (Vulkan SVGF)");
    std::snprintf(s.status.swapchainStatus, sizeof(s.status.swapchainStatus), "%s", s.presenter.statusString().c_str());

    if (!s.ctx.lastError.empty()) {
        s.setStatusError(s.ctx.lastErrorCode, s.ctx.lastError);
    }
    logNative("linrtx_init: complete vkReady=%d ngxReady=%d dlss=%d dlssd=%d dlssg=%d maxFg=%d err='%s'",
              s.status.vkReady, s.status.ngxReady, s.status.dlssSupported, s.status.dlssdSupported,
              s.status.dlssgSupported, s.status.maxFgMultiplier, s.ctx.lastError.c_str());
    return 0;
}

int32_t linrtx_get_optimal_scale(
    int32_t dlssMode,
    uint32_t displayW,
    uint32_t displayH,
    uint32_t* outRenderW,
    uint32_t* outRenderH,
    float* outRecommendedSharpness) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    const bool ngxUsed = ngxWorker().run([&]() {
        return s.dlssSr.queryOptimalScale(
            s.ctx, dlssMode, displayW, displayH, outRenderW, outRenderH, outRecommendedSharpness);
    });
    return ngxUsed ? 0 : 1;
}

int32_t linrtx_evaluate_dlss(const LinRtxFrameConfig* frame) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    s.status.dlssActive = 0;
    s.status.dlssdActive = 0;
    s.status.denoiserActive = 0;
    s.status.dlssnrActive = 0;
    s.status.dlssnrUsingNgx = 0;
    s.depthMvecFreshThisFrame = false;

    if (!s.initialized || !s.ctx.vkReady) {
        return -1;
    }
    if (!frame || frame->structSize != sizeof(LinRtxFrameConfig)) {
        s.setStatusError(-10, "Invalid LinRtxFrameConfig structSize");
        return -10;
    }

    const bool wantDenoise = (frame->denoiseMode != LINRTX_DENOISE_OFF);
    const bool wantUpscale = (frame->dlssMode != LINRTX_DLSS_OFF);
    const bool wantNeural = (frame->neuralMode != LINRTX_NEURAL_OFF);
    if (!wantNeural) {
        std::snprintf(s.status.dlssnrStatus, sizeof(s.status.dlssnrStatus), "Off");
    }
    if ((!wantUpscale && !wantDenoise && !wantNeural) || !frame->colorInTexGlId || !frame->depthInTexGlId ||
        !frame->mvecInTexGlId || !frame->outUpscaledTexGlId) {
        return 0;
    }

    const bool wantRr = wantUpscale && ((frame->rrMode == LINRTX_RR_ON) ||
                                        (frame->rrMode == LINRTX_RR_AUTO && frame->useGbufferGuides != 0));
    const bool tryRr = wantRr && s.ctx.dlssdSupported;
    const bool trySr = wantUpscale && s.ctx.dlssSupported;
    if (wantUpscale && !tryRr && !trySr && !wantDenoise && !wantNeural) {
        s.setStatusError(-2, "DLSS Super Resolution / Ray Reconstruction unavailable");
        return -2;
    }

    const VkFormat colorVkFmt = (frame->colorIsHdrInput != 0) ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    const GLenum colorGlFmt = (frame->colorIsHdrInput != 0) ? kGlRgba16f : kGlRgba8;
    constexpr VkImageUsageFlags kSrvUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageUsageFlags kStorageUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Wait for any asynchronous prior-frame GPU work BEFORE ensureTex may destroy and
    // reallocate shared VkImages / GL textures (e.g. on quality or preset change).
    s.ctx.waitGpuIdleFence();

    bool okAlloc =
        s.ensureTex(s.texColorIn, frame->renderWidth, frame->renderHeight, colorVkFmt, kSrvUsage, colorGlFmt, true) &&
        s.ensureTex(s.texDepthIn, frame->renderWidth, frame->renderHeight, VK_FORMAT_R32_SFLOAT, kSrvUsage, kGlR32f, true) &&
        s.ensureTex(s.texMvecIn, frame->renderWidth, frame->renderHeight, VK_FORMAT_R16G16_SFLOAT, kSrvUsage, kGlRg16f, true) &&
        s.ensureTex(s.texDlssOut, frame->displayWidth, frame->displayHeight, colorVkFmt, kStorageUsage, colorGlFmt, true) &&
        s.ensureTex(s.texCasOut, frame->displayWidth, frame->displayHeight, colorVkFmt, kStorageUsage, colorGlFmt, true);

    if (okAlloc && wantDenoise) {
        const bool sameDim = (frame->renderWidth == frame->displayWidth && frame->renderHeight == frame->displayHeight);
        okAlloc = s.ensureTex(s.texDenoiseOut, frame->renderWidth, frame->renderHeight,
                              colorVkFmt, kStorageUsage, colorGlFmt, sameDim);
    }
    const bool neuralPreSr = wantNeural && (frame->neuralPlacement == LINRTX_NEURAL_PRE_SR) && wantUpscale && !tryRr;
    if (okAlloc && wantNeural) {
        if (neuralPreSr) {
            okAlloc = s.ensureTex(s.texNeuralPreOut, frame->renderWidth, frame->renderHeight,
                                  colorVkFmt, kStorageUsage, colorGlFmt, false);
        } else {
            okAlloc = s.ensureTex(s.texNeuralOut, frame->displayWidth, frame->displayHeight,
                                  colorVkFmt, kStorageUsage, colorGlFmt, true);
        }
    }

    const bool hasExpAlbedo = (frame->diffuseAlbedoInTexGlId != 0);
    const bool hasExpSpec = (frame->specularAlbedoInTexGlId != 0);
    const bool hasExpNorm = (frame->normalsInTexGlId != 0);
    const bool hasExpRough = (frame->roughnessInTexGlId != 0);

    auto ensureOptionalGuide = [&](SharedTexture& tex, uint32_t glTexId, uint32_t glFmt,
                                   VkFormat fbVk, GLenum fbGl) -> bool {
        if (glTexId == 0) return true;
        VkFormat vkFmt = fbVk;
        GLenum realGlFmt = fbGl;
        mapGlFormatToVk(glFmt, fbVk, fbGl, vkFmt, realGlFmt);
        return s.ensureTex(tex, frame->renderWidth, frame->renderHeight, vkFmt, kSrvUsage, realGlFmt, true);
    };

    if (okAlloc) okAlloc = ensureOptionalGuide(s.texDiffuseAlbedoIn, frame->diffuseAlbedoInTexGlId, frame->diffuseAlbedoFormatGl, VK_FORMAT_R8G8B8A8_UNORM, kGlRgba8);
    if (okAlloc) okAlloc = ensureOptionalGuide(s.texSpecularAlbedoIn, frame->specularAlbedoInTexGlId, frame->specularAlbedoFormatGl, VK_FORMAT_R8G8B8A8_UNORM, kGlRgba8);
    if (okAlloc) okAlloc = ensureOptionalGuide(s.texNormalsIn, frame->normalsInTexGlId, frame->normalsFormatGl, VK_FORMAT_R16G16B16A16_SFLOAT, kGlRgba16f);
    if (okAlloc) okAlloc = ensureOptionalGuide(s.texRoughnessIn, frame->roughnessInTexGlId, frame->roughnessFormatGl, VK_FORMAT_R16_SFLOAT, 0x822D);

    if (!okAlloc) {
        s.setStatusError(-3, "Failed to allocate GL<->VK shared textures for DLSS/Denoise");
        return -3;
    }

    static bool loggedFirstEntry = false;
    static bool loggedFirstSuccess = false;
    static uint32_t errLogCount = 0;
    if (!loggedFirstEntry) {
        loggedFirstEntry = true;
        logNative("linrtx_evaluate_dlss: first call in=%ux%u out=%ux%u mode=%d preset=%d rr=%d(supp=%d) rrPreset=%d denoise=%d guides=%d",
                  frame->renderWidth, frame->renderHeight, frame->displayWidth, frame->displayHeight,
                  frame->dlssMode, frame->dlssPreset, frame->rrMode, s.ctx.dlssdSupported ? 1 : 0,
                  frame->rrPreset, frame->denoiseMode, frame->useGbufferGuides);
    }

    SharedTexture* glInTexs[8]{};
    uint32_t glInCount = 0;
    s.ctx.copyGlToShared(frame->colorInTexGlId, s.texColorIn, frame->renderWidth, frame->renderHeight);
    glInTexs[glInCount++] = &s.texColorIn;
    s.ctx.copyGlToShared(frame->depthInTexGlId, s.texDepthIn, frame->renderWidth, frame->renderHeight);
    glInTexs[glInCount++] = &s.texDepthIn;
    s.ctx.copyGlToShared(frame->mvecInTexGlId, s.texMvecIn, frame->renderWidth, frame->renderHeight);
    glInTexs[glInCount++] = &s.texMvecIn;
    if (hasExpAlbedo) {
        s.ctx.copyGlToShared(frame->diffuseAlbedoInTexGlId, s.texDiffuseAlbedoIn, frame->renderWidth, frame->renderHeight);
        glInTexs[glInCount++] = &s.texDiffuseAlbedoIn;
    }
    if (hasExpSpec) {
        s.ctx.copyGlToShared(frame->specularAlbedoInTexGlId, s.texSpecularAlbedoIn, frame->renderWidth, frame->renderHeight);
        glInTexs[glInCount++] = &s.texSpecularAlbedoIn;
    }
    if (hasExpNorm) {
        s.ctx.copyGlToShared(frame->normalsInTexGlId, s.texNormalsIn, frame->renderWidth, frame->renderHeight);
        glInTexs[glInCount++] = &s.texNormalsIn;
    }
    if (hasExpRough) {
        s.ctx.copyGlToShared(frame->roughnessInTexGlId, s.texRoughnessIn, frame->renderWidth, frame->renderHeight);
        glInTexs[glInCount++] = &s.texRoughnessIn;
    }
    s.depthMvecFreshThisFrame = true;

    const bool glSignaled = s.ctx.signalGlToVkSemaphore(glInTexs, glInCount);

    int32_t activePreset = frame->dlssPreset;
    int32_t activeRrPreset = frame->rrPreset;
    int32_t activeNeuralPreset = frame->neuralPreset;
    int32_t usedRr = 0;
    int32_t usedSr = 0;
    int32_t usedDenoiser = 0;
    int32_t usedNeural = 0;
    bool neuralUsedNgx = false;
    SharedTexture* stage1OutTex = nullptr;
    std::string err;
    int32_t errCode = 0;
    bool vkSignaledGl = false;

    const int32_t vkRc = ngxWorker().run([&]() -> int32_t {
        if (!s.ctx.beginCommands(glSignaled)) {
            err = "beginCommands failed for Stage-1";
            errCode = -4;
            return -4;
        }

        SharedTexture* srcForSr = &s.texColorIn;
        if (wantDenoise && !tryRr) {
            SvgfDenoiser::Params dp{};
            dp.width = frame->renderWidth;
            dp.height = frame->renderHeight;
            dp.resetHistory = (frame->resetHistory != 0);
            dp.hasAlbedo = hasExpAlbedo;
            dp.hasNormals = hasExpNorm && (frame->gbufferNormalsAreWorld == 0);
            dp.normalsPacked01 = (frame->normalsFormatGl == kGlRgba8 || frame->normalsFormatGl == 0x8C43);
            dp.demodulateAlbedo = hasExpAlbedo;
            dp.inputIsSrgb = (frame->colorIsHdrInput == 0);
            dp.preserveAlpha = false;
            dp.reverseZ = (frame->depthInverted != 0);
            dp.nearPlane = frame->cameraNear > 0.0f ? frame->cameraNear : 0.05f;
            dp.farPlane = frame->cameraFar > dp.nearPlane ? frame->cameraFar : 256.0f;
            dp.temporalBlend = (frame->denoiseTemporalBlend > 0.0f) ? frame->denoiseTemporalBlend : 0.88f;
            dp.strength = (frame->denoiseStrength > 0.0f) ? frame->denoiseStrength : 1.0f;
            dp.atrousPasses = (frame->denoiseMode == LINRTX_DENOISE_SVGF_FAST)
                                  ? std::min(2u, frame->denoiseAtrousPasses ? frame->denoiseAtrousPasses : 2u)
                                  : (frame->denoiseAtrousPasses ? frame->denoiseAtrousPasses : 4u);
            dp.bankIndex = 0u;

            std::string dErr;
            if (s.svgf.evaluate(s.ctx, s.ctx.cmdBuf, dp,
                                s.texColorIn, s.texDepthIn, s.texMvecIn,
                                hasExpAlbedo ? &s.texDiffuseAlbedoIn : nullptr,
                                hasExpNorm ? &s.texNormalsIn : nullptr,
                                s.texDenoiseOut, dErr)) {
                srcForSr = &s.texDenoiseOut;
                usedDenoiser = 1;
            }
        }

        if (neuralPreSr) {
            std::string nrErr;
            int32_t nrCode = 0;
            if (s.dlssNr.evaluate(
                    s.ctx, s.ctx.cmdBuf, *frame,
                    *srcForSr, s.texDepthIn, s.texMvecIn, s.texNeuralPreOut,
                    activeNeuralPreset, neuralUsedNgx, nrErr, nrCode)) {
                srcForSr = &s.texNeuralPreOut;
                usedNeural = 1;
            } else if (frame->neuralMode == LINRTX_NEURAL_NGX_ONLY) {
                err = nrErr;
                errCode = nrCode;
                s.ctx.endAndSubmitCommands(false, true);
                return -5;
            }
        }

        bool upscaledOk = false;
        if (tryRr) {
            std::string rrErr;
            int32_t rrCode = 0;
            SharedTexture* rrAlb = hasExpAlbedo ? &s.texDiffuseAlbedoIn : nullptr;
            SharedTexture* rrSpec = hasExpSpec ? &s.texSpecularAlbedoIn : nullptr;
            SharedTexture* rrNorm = hasExpNorm ? &s.texNormalsIn : nullptr;
            SharedTexture* rrRough = hasExpRough ? &s.texRoughnessIn : nullptr;
            if ((rrAlb || rrNorm) && (frame->gbufferNormalsAreWorld != 0 || !rrSpec || !rrRough)) {
                std::string gErr;
                const bool normPacked = (frame->normalsFormatGl == kGlRgba8 || frame->normalsFormatGl == 0x8C43);
                if (!s.preTonemap.completeGuides(
                        s.ctx, s.ctx.cmdBuf, frame->renderWidth, frame->renderHeight,
                        s.texColorIn, s.texDepthIn, rrAlb, rrNorm,
                        frame->gbufferNormalsAreWorld != 0, normPacked,
                        frame->depthInverted != 0,
                        frame->cameraNear > 0.0f ? frame->cameraNear : 0.05f,
                        frame->cameraFar > 1.0f ? frame->cameraFar : 256.0f,
                        frame->worldToView,
                        rrAlb, rrSpec, rrNorm, rrRough, gErr)) {
                    logNative("Stage1 guide completion warning: %s", gErr.c_str());
                }
            }
            if (s.dlssRr.evaluate(
                    s.ctx, s.ctx.cmdBuf, *frame,
                    s.texColorIn, s.texDepthIn, s.texMvecIn,
                    rrAlb, rrSpec, rrNorm, rrRough,
                    s.texDlssOut, activeRrPreset, rrErr, rrCode)) {
                upscaledOk = true;
                usedRr = 1;
            } else if (!trySr && !wantDenoise && !wantNeural) {
                err = rrErr;
                errCode = rrCode;
                s.ctx.endAndSubmitCommands(false, true);
                return -5;
            } else {
                logNative("linrtx_evaluate_dlss: DLSS-RR fallback (%s)", rrErr.c_str());
            }
        }

        if (!upscaledOk && trySr) {
            if (!s.dlssSr.evaluate(
                    s.ctx, s.ctx.cmdBuf, *frame,
                    *srcForSr, s.texDepthIn, s.texMvecIn, s.texDlssOut,
                    activePreset, err, errCode)) {
                if (!usedDenoiser && !wantNeural) {
                    s.ctx.endAndSubmitCommands(false, true);
                    return -5;
                }
            } else {
                upscaledOk = true;
                usedSr = 1;
            }
        }

        SharedTexture* finalUpscaled = upscaledOk ? &s.texDlssOut : srcForSr;
        if (!upscaledOk &&
            (srcForSr->width != frame->displayWidth || srcForSr->height != frame->displayHeight)) {
            s.ctx.cmdBarrier(s.ctx.cmdBuf, *srcForSr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            s.ctx.cmdBarrier(s.ctx.cmdBuf, s.texDlssOut, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {static_cast<int32_t>(srcForSr->width), static_cast<int32_t>(srcForSr->height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {static_cast<int32_t>(frame->displayWidth), static_cast<int32_t>(frame->displayHeight), 1};
            vkCmdBlitImage(s.ctx.cmdBuf,
                           srcForSr->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s.texDlssOut.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);
            finalUpscaled = &s.texDlssOut;
        }
        if (frame->sharpness > 0.001f) {
            if (s.rtxHdr.applyCasSharpen(
                    s.ctx, s.ctx.cmdBuf, *finalUpscaled, s.texCasOut,
                    frame->displayWidth, frame->displayHeight, frame->sharpness)) {
                finalUpscaled = &s.texCasOut;
            }
        }
        if (wantNeural && !usedNeural) {
            std::string nrErr;
            int32_t nrCode = 0;
            if (s.dlssNr.evaluate(
                    s.ctx, s.ctx.cmdBuf, *frame,
                    *finalUpscaled, s.texDepthIn, s.texMvecIn, s.texNeuralOut,
                    activeNeuralPreset, neuralUsedNgx, nrErr, nrCode)) {
                finalUpscaled = &s.texNeuralOut;
                usedNeural = 1;
            } else if (frame->neuralMode == LINRTX_NEURAL_NGX_ONLY) {
                err = nrErr;
                errCode = nrCode;
                s.ctx.endAndSubmitCommands(false, true);
                return -5;
            }
        }

        // Prepare final upscaled texture for GL external semaphore transfer
        s.ctx.cmdBarrier(s.ctx.cmdBuf, *finalUpscaled, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        if (!s.ctx.endAndSubmitCommands(true, false, &vkSignaledGl)) {
            err = "endAndSubmitCommands failed for Stage-1";
            errCode = -6;
            return -6;
        }
        stage1OutTex = finalUpscaled;
        return 0;
    });

    if (vkRc < 0 || !stage1OutTex) {
        if (errLogCount < 8) {
            ++errLogCount;
            logNative("linrtx_evaluate_dlss: FAILED vkRc=%d errCode=%d err='%s'",
                      vkRc, errCode, err.c_str());
        }
        if (wantNeural) {
            std::snprintf(s.status.dlssnrStatus, sizeof(s.status.dlssnrStatus), "%s", s.dlssNr.statusString().c_str());
        }
        s.setStatusError(errCode, err);
        return vkRc < 0 ? vkRc : -6;
    }

    SharedTexture& finalUpscaledTex = *stage1OutTex;
    if (vkSignaledGl) {
        const SharedTexture* glOutTexs[1] = {&finalUpscaledTex};
        s.ctx.waitVkToGlSemaphore(glOutTexs, 1);
    }
    s.ctx.copySharedToGl(finalUpscaledTex, frame->outUpscaledTexGlId, frame->displayWidth, frame->displayHeight);
    if (!loggedFirstSuccess) {
        loggedFirstSuccess = true;
        logNative("linrtx_evaluate_dlss: first success sr=%d rr=%d denoiser=%d neural=%d(ngx=%d)",
                  usedSr, usedRr, usedDenoiser, usedNeural, neuralUsedNgx ? 1 : 0);
    }
    const bool preRrActive = (frame->flagsReserved0 & LINRTX_FRAME_FLAG_PRETONEMAP_RR_ACTIVE) != 0u;
    s.status.dlssActive = (usedSr || usedRr || preRrActive) ? 1 : 0;
    s.status.dlssdActive = (usedRr || preRrActive) ? 1 : 0;
    s.status.denoiserActive = (usedDenoiser || usedRr || preRrActive) ? 1 : s.status.denoiserActive;
    s.status.activeDlssPreset = activePreset;
    if (usedRr) {
        s.status.activeRrPreset = activeRrPreset;
    }
    s.status.dlssnrActive = usedNeural;
    s.status.dlssnrUsingNgx = (usedNeural && neuralUsedNgx) ? 1 : 0;
    s.status.dlssnrNgxLoaded = s.dlssNr.ngxLibraryPresent() ? 1 : 0;
    s.status.dlssnrUnlocked = (frame->neuralUnlockAnyRtx != 0 && s.ctx.pciVendorId == 0x10DE) ? 1 : 0;
    s.status.activeNeuralPreset = activeNeuralPreset;
    std::snprintf(s.status.dlssnrVersion, sizeof(s.status.dlssnrVersion), "%s", s.dlssNr.versionString().c_str());
    std::snprintf(s.status.dlssnrStatus, sizeof(s.status.dlssnrStatus), "%s", s.dlssNr.statusString().c_str());
    if (usedRr) {
        std::snprintf(s.status.denoiserStatus, sizeof(s.status.denoiserStatus), "DLSS Ray Reconstruction");
    } else if (usedDenoiser) {
        std::snprintf(s.status.denoiserStatus, sizeof(s.status.denoiserStatus), "Vulkan SVGF + Atrous");
    }
    s.clearStatusError();
    return 0;
}

int32_t linrtx_evaluate_denoise(const LinRtxDenoiseConfig* cfg) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    if (!s.initialized || !s.ctx.vkReady) {
        return -1;
    }
    if (!cfg || cfg->structSize != sizeof(LinRtxDenoiseConfig)) {
        s.setStatusError(-10, "Invalid LinRtxDenoiseConfig structSize");
        return -10;
    }
    if (cfg->width == 0 || cfg->height == 0 || !cfg->hdrColorTexGlId ||
        !cfg->outHdrColorTexGlId || !cfg->depthTexGlId || !cfg->mvecTexGlId) {
        return -11;
    }

    const bool topLeftVk = (cfg->inputsTopLeftVk != 0u);
    const bool wantRr = topLeftVk && s.ctx.dlssdSupported &&
                        (cfg->rrMode == LINRTX_RR_ON || cfg->rrMode == LINRTX_RR_AUTO);
    const bool wantSvgf = (cfg->denoiseMode != LINRTX_DENOISE_OFF) ||
                          (!wantRr && cfg->rrMode != LINRTX_RR_OFF);
    if (!wantRr && !wantSvgf) {
        return -12;
    }

    VkFormat inVk = VK_FORMAT_R32G32B32A32_SFLOAT;
    GLenum inGl = 0x8814;
    mapGlFormatToVk(cfg->hdrColorFormatGl, VK_FORMAT_R32G32B32A32_SFLOAT, 0x8814, inVk, inGl);

    VkFormat outVk = inVk;
    GLenum outGl = inGl;
    mapGlFormatToVk(cfg->outHdrColorFormatGl, inVk, inGl, outVk, outGl);

    constexpr VkImageUsageFlags kSrvUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageUsageFlags kStorageUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    s.ctx.waitGpuIdleFence();

    bool okAlloc =
        s.ensureTex(s.texPreHdrIn, cfg->width, cfg->height, inVk, kSrvUsage, inGl, true) &&
        s.ensureTex(s.texPreHdrOut, cfg->width, cfg->height, outVk, kStorageUsage, outGl, true) &&
        s.ensureTex(s.texPreDepthIn, cfg->width, cfg->height, VK_FORMAT_R32_SFLOAT, kSrvUsage, kGlR32f, true) &&
        s.ensureTex(s.texPreMvecIn, cfg->width, cfg->height, VK_FORMAT_R16G16_SFLOAT, kSrvUsage, kGlRg16f, true);

    const bool hasAlb = (cfg->diffuseAlbedoTexGlId != 0);
    const bool hasNorm = (cfg->normalsTexGlId != 0);
    if (okAlloc && hasAlb) {
        VkFormat aVk = VK_FORMAT_R8G8B8A8_UNORM;
        GLenum aGl = kGlRgba8;
        mapGlFormatToVk(cfg->diffuseAlbedoFormatGl, VK_FORMAT_R8G8B8A8_UNORM, kGlRgba8, aVk, aGl);
        okAlloc = s.ensureTex(s.texPreAlbedoIn, cfg->width, cfg->height, aVk, kSrvUsage, aGl, true);
    }
    if (okAlloc && hasNorm) {
        VkFormat nVk = VK_FORMAT_R16G16B16A16_SFLOAT;
        GLenum nGl = kGlRgba16f;
        mapGlFormatToVk(cfg->normalsFormatGl, VK_FORMAT_R16G16B16A16_SFLOAT, kGlRgba16f, nVk, nGl);
        okAlloc = s.ensureTex(s.texPreNormalsIn, cfg->width, cfg->height, nVk, kSrvUsage, nGl, true);
    }

    if (!okAlloc) {
        s.setStatusError(-3, "Failed to allocate pre-tonemap denoise textures");
        return -3;
    }

    SharedTexture* glInTexs[6]{};
    uint32_t glInCount = 0;
    s.ctx.copyGlToShared(cfg->hdrColorTexGlId, s.texPreHdrIn, cfg->width, cfg->height);
    glInTexs[glInCount++] = &s.texPreHdrIn;
    s.ctx.copyGlToShared(cfg->depthTexGlId, s.texPreDepthIn, cfg->width, cfg->height);
    glInTexs[glInCount++] = &s.texPreDepthIn;
    s.ctx.copyGlToShared(cfg->mvecTexGlId, s.texPreMvecIn, cfg->width, cfg->height);
    glInTexs[glInCount++] = &s.texPreMvecIn;
    if (hasAlb) {
        s.ctx.copyGlToShared(cfg->diffuseAlbedoTexGlId, s.texPreAlbedoIn, cfg->width, cfg->height);
        glInTexs[glInCount++] = &s.texPreAlbedoIn;
    }
    if (hasNorm) {
        s.ctx.copyGlToShared(cfg->normalsTexGlId, s.texPreNormalsIn, cfg->width, cfg->height);
        glInTexs[glInCount++] = &s.texPreNormalsIn;
    }

    const bool glSignaled = s.ctx.signalGlToVkSemaphore(glInTexs, glInCount);

    std::string err;
    bool vkSignaledGl = false;
    bool usedPreRr = false;
    bool usedPreSvgf = false;
    int32_t activeRrPreset = cfg->rrPreset;
    static uint32_t preRrErrLogCount = 0;

    const int32_t rc = ngxWorker().run([&]() -> int32_t {
        if (!s.ctx.beginCommands(glSignaled)) {
            err = "beginCommands failed for pre-tonemap denoise";
            return -4;
        }

        SharedTexture* preHdrVk = nullptr;
        SharedTexture* denoisedSrc = nullptr;
        bool resolveFlipY = topLeftVk;

        if (topLeftVk) {
            if (!s.preTonemap.prepareColorForRr(s.ctx, s.ctx.cmdBuf, cfg->width, cfg->height,
                                                true /*flipY GL->VK*/, s.texPreHdrIn, preHdrVk, err)) {
                s.ctx.endAndSubmitCommands(false, true);
                return -5;
            }
        }

        if (wantRr && preHdrVk) {
            const bool normPacked = (cfg->normalsFormatGl == kGlRgba8 || cfg->normalsFormatGl == 0x8C43);
            SharedTexture* gAlb = nullptr;
            SharedTexture* gSpec = nullptr;
            SharedTexture* gNorm = nullptr;
            SharedTexture* gRough = nullptr;
            SharedTexture* preOutVk = nullptr;
            if (s.preTonemap.completeGuides(
                    s.ctx, s.ctx.cmdBuf, cfg->width, cfg->height,
                    *preHdrVk, s.texPreDepthIn,
                    hasAlb ? &s.texPreAlbedoIn : nullptr,
                    hasNorm ? &s.texPreNormalsIn : nullptr,
                    cfg->normalsAreWorld != 0, normPacked, true,
                    cfg->cameraNear > 0.0f ? cfg->cameraNear : 0.05f,
                    cfg->cameraFar > 1.0f ? cfg->cameraFar : 256.0f,
                    cfg->worldToView,
                    gAlb, gSpec, gNorm, gRough, err) &&
                s.preTonemap.ensurePreHdrOutVk(s.ctx, cfg->width, cfg->height, preOutVk, err)) {

                LinRtxFrameConfig rrFrame{};
                rrFrame.structSize = sizeof(LinRtxFrameConfig);
                rrFrame.renderWidth = cfg->width;
                rrFrame.renderHeight = cfg->height;
                rrFrame.displayWidth = cfg->width;
                rrFrame.displayHeight = cfg->height;
                rrFrame.dlssMode = LINRTX_DLSS_DLAA;
                rrFrame.jitterOffsetX = cfg->jitterOffsetX;
                rrFrame.jitterOffsetY = cfg->jitterOffsetY;
                rrFrame.mvScaleX = static_cast<float>(cfg->width);
                rrFrame.mvScaleY = static_cast<float>(cfg->height);
                rrFrame.resetHistory = cfg->resetHistory;
                rrFrame.depthInverted = 1;
                rrFrame.colorIsHdrInput = 1;
                rrFrame.cameraNear = cfg->cameraNear > 0.0f ? cfg->cameraNear : 0.05f;
                rrFrame.cameraFar = cfg->cameraFar > rrFrame.cameraNear ? cfg->cameraFar : 256.0f;
                rrFrame.cameraFovRadians = cfg->cameraFovRadians > 0.01f ? cfg->cameraFovRadians : 1.2217f;
                rrFrame.cameraAspect = cfg->cameraAspect > 0.01f
                    ? cfg->cameraAspect
                    : (static_cast<float>(cfg->width) / static_cast<float>(cfg->height));
                std::memcpy(rrFrame.cameraPos, cfg->cameraPos, sizeof(rrFrame.cameraPos));
                std::memcpy(rrFrame.cameraUp, cfg->cameraUp, sizeof(rrFrame.cameraUp));
                std::memcpy(rrFrame.cameraRight, cfg->cameraRight, sizeof(rrFrame.cameraRight));
                std::memcpy(rrFrame.cameraFwd, cfg->cameraFwd, sizeof(rrFrame.cameraFwd));
                std::memcpy(rrFrame.viewToClip, cfg->viewToClip, sizeof(rrFrame.viewToClip));
                std::memcpy(rrFrame.clipToView, cfg->clipToView, sizeof(rrFrame.clipToView));
                std::memcpy(rrFrame.clipToPrevClip, cfg->clipToPrevClip, sizeof(rrFrame.clipToPrevClip));
                std::memcpy(rrFrame.prevClipToClip, cfg->prevClipToClip, sizeof(rrFrame.prevClipToClip));
                std::memcpy(rrFrame.worldToView, cfg->worldToView, sizeof(rrFrame.worldToView));
                rrFrame.rrMode = LINRTX_RR_ON;
                rrFrame.rrPreset = cfg->rrPreset;
                rrFrame.useGbufferGuides = 1;
                rrFrame.gbufferNormalsAreWorld = 0;

                std::string rrErr;
                int32_t rrCode = 0;
                if (s.dlssRr.evaluate(
                        s.ctx, s.ctx.cmdBuf, rrFrame,
                        *preHdrVk, s.texPreDepthIn, s.texPreMvecIn,
                        gAlb, gSpec, gNorm, gRough,
                        *preOutVk, activeRrPreset, rrErr, rrCode)) {
                    usedPreRr = true;
                    denoisedSrc = preOutVk;
                    resolveFlipY = true;
                } else {
                    if (preRrErrLogCount < 4) {
                        ++preRrErrLogCount;
                        logNative("linrtx_evaluate_denoise: pre-tonemap DLSS-RR fallback (%s)", rrErr.c_str());
                    }
                    err = rrErr;
                }
            }
        }

        if (!usedPreRr) {
            if (!wantSvgf && cfg->rrMode == LINRTX_RR_ON) {
                s.ctx.endAndSubmitCommands(false, true);
                return -5;
            }
            SharedTexture* svgfIn = topLeftVk ? preHdrVk : &s.texPreHdrIn;
            SharedTexture* svgfOut = nullptr;
            if (!svgfIn || !s.preTonemap.ensureScratchTmp(s.ctx, cfg->width, cfg->height, outVk, svgfOut, err)) {
                s.ctx.endAndSubmitCommands(false, true);
                return -5;
            }

            SvgfDenoiser::Params dp{};
            dp.width = cfg->width;
            dp.height = cfg->height;
            dp.resetHistory = (cfg->resetHistory != 0);
            dp.hasAlbedo = hasAlb;
            dp.hasNormals = hasNorm;
            dp.normalsPacked01 = (cfg->normalsFormatGl == kGlRgba8 || cfg->normalsFormatGl == 0x8C43);
            dp.demodulateAlbedo = (cfg->demodulateAlbedo != 0) && hasAlb;
            dp.inputIsSrgb = false; // Pre-tonemap radiance is strictly linear HDR
            dp.preserveAlpha = false;
            dp.reverseZ = true;
            dp.nearPlane = cfg->cameraNear > 0.0f ? cfg->cameraNear : 0.05f;
            dp.farPlane = cfg->cameraFar > dp.nearPlane ? cfg->cameraFar : 256.0f;
            dp.temporalBlend = cfg->temporalBlend >= 0.0f ? cfg->temporalBlend : 0.88f;
            dp.strength = cfg->denoiseStrength > 0.0f ? cfg->denoiseStrength : 1.0f;
            const uint32_t reqPasses = cfg->atrousPasses ? cfg->atrousPasses : 4u;
            dp.atrousPasses = (cfg->denoiseMode == LINRTX_DENOISE_SVGF_FAST)
                                  ? std::min(2u, reqPasses)
                                  : reqPasses;
            dp.bankIndex = 1u;

            if (!s.svgf.evaluate(s.ctx, s.ctx.cmdBuf, dp,
                                 *svgfIn, s.texPreDepthIn, s.texPreMvecIn,
                                 hasAlb ? &s.texPreAlbedoIn : nullptr,
                                 hasNorm ? &s.texPreNormalsIn : nullptr,
                                 *svgfOut, err)) {
                s.ctx.endAndSubmitCommands(false, true);
                return -5;
            }
            usedPreSvgf = true;
            denoisedSrc = svgfOut;
            resolveFlipY = topLeftVk;
        }

        if (!s.preTonemap.resolvePreTonemap(
                s.ctx, s.ctx.cmdBuf, cfg->width, cfg->height,
                *denoisedSrc, s.texPreHdrIn, s.texPreHdrOut,
                resolveFlipY, cfg->preserveAlpha != 0,
                cfg->f1AccumActive != 0, cfg->f1FadeSamples, err)) {
            s.ctx.endAndSubmitCommands(false, true);
            return -5;
        }

        s.ctx.cmdBarrier(s.ctx.cmdBuf, s.texPreHdrOut, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (!s.ctx.endAndSubmitCommands(true, false, &vkSignaledGl)) {
            err = "endAndSubmitCommands failed for pre-tonemap denoise";
            return -6;
        }
        return 0;
    });

    if (rc < 0) {
        s.setStatusError(rc, err);
        return rc;
    }

    if (vkSignaledGl) {
        const SharedTexture* outTexs[1] = {&s.texPreHdrOut};
        s.ctx.waitVkToGlSemaphore(outTexs, 1);
    }
    s.ctx.copySharedToGl(s.texPreHdrOut, cfg->outHdrColorTexGlId, cfg->width, cfg->height);
    s.status.denoiserActive = 1;
    s.status.dlssdActive = usedPreRr ? 1 : 0;
    if (usedPreRr) {
        s.status.activeRrPreset = activeRrPreset;
        std::snprintf(s.status.denoiserStatus, sizeof(s.status.denoiserStatus),
                      "Iris Pre-Tonemap DLSS-RR%s", cfg->f1AccumActive ? " (F1)" : "");
    } else if (usedPreSvgf) {
        std::snprintf(s.status.denoiserStatus, sizeof(s.status.denoiserStatus),
                      "Iris Pre-Tonemap SVGF%s", cfg->f1AccumActive ? " (F1)" : "");
    }
    return 0;
}

int32_t linrtx_evaluate_post(const LinRtxFrameConfig* frame) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    s.status.dlssgActive = 0;
    s.status.trueHdrActive = 0;
    s.status.hdrShaderActive = 0;
    s.status.generatedFrameCount = 0;
    s.lastGeneratedCount = 0;
    s.lastRealFrameTex = nullptr;

    if (!s.initialized || !s.ctx.vkReady) {
        return -1;
    }
    if (!frame || frame->structSize != sizeof(LinRtxFrameConfig)) {
        s.setStatusError(-10, "Invalid LinRtxFrameConfig structSize");
        return -10;
    }

    const bool wantFg = (frame->fgMode != LINRTX_FG_OFF) && s.ctx.dlssgSupported;
    const bool wantHdr = (frame->hdrEngine != LINRTX_HDR_OFF);
    const bool wantVkSwap = (frame->presentBackend == LINRTX_PRESENT_VULKAN_HDR);

    // Wait for prior async GPU submissions before destroying/resizing swapchains or shared textures
    s.ctx.waitGpuIdleFence();

    if (!wantVkSwap && s.presenter.active()) {
        s.presenter.destroy(s.ctx);
        s.status.vkSwapchainActive = 0;
        s.status.vkSwapchainHdr10 = 0;
        std::snprintf(s.status.swapchainStatus, sizeof(s.status.swapchainStatus), "OpenGL Swap");
    }

    if (!wantFg && !wantHdr && !wantVkSwap) {
        s.depthMvecFreshThisFrame = false;
        return 0;
    }

    if (!frame->backbufferInTexGlId) {
        return 0;
    }

    if (wantVkSwap && !s.presenter.active()) {
        s.presenter.init(s.ctx, &s.initCfg, frame->displayWidth, frame->displayHeight,
                         static_cast<float>(frame->hdrMaxLuminance));
    } else if (wantVkSwap && s.presenter.active()) {
        s.presenter.resize(s.ctx, frame->displayWidth, frame->displayHeight,
                           static_cast<float>(frame->hdrMaxLuminance));
    }
    s.status.vkSwapchainActive = s.presenter.active() ? 1 : 0;
    s.status.vkSwapchainHdr10 = s.presenter.isHdr10() ? 1 : 0;
    std::snprintf(s.status.swapchainStatus, sizeof(s.status.swapchainStatus), "%s", s.presenter.statusString().c_str());

    const VkFormat inVkFmt = (frame->colorIsHdrInput != 0) ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    const GLenum inGlFmt = (frame->colorIsHdrInput != 0) ? kGlRgba16f : kGlRgba8;
    VkFormat stage2VkFmt = VK_FORMAT_R8G8B8A8_UNORM;
    GLenum stage2GlFmt = kGlRgba8;
    s.determineStage2Format(*frame, stage2VkFmt, stage2GlFmt);

    constexpr VkImageUsageFlags kSrvUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageUsageFlags kStorageUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    bool okAlloc = s.ensureTex(
        s.texBackbufferIn, frame->displayWidth, frame->displayHeight, inVkFmt, kSrvUsage, inGlFmt, true);

    const bool useHudless = (frame->useHudlessSplit != 0 && frame->hudlessInTexGlId != 0 && wantFg);
    if (okAlloc && useHudless) {
        okAlloc = s.ensureTex(
            s.texHudlessIn, frame->displayWidth, frame->displayHeight, inVkFmt, kSrvUsage, inGlFmt, true);
    }

    if (okAlloc && wantHdr) {
        okAlloc = s.ensureTex(s.texHdrOut, frame->displayWidth, frame->displayHeight,
                              stage2VkFmt, kStorageUsage, stage2GlFmt, true);
        if (okAlloc && s.rtxHdr.trueHdrLibraryPresent()) {
            okAlloc = s.ensureTex(s.texTrueHdrPqTmp, frame->displayWidth, frame->displayHeight,
                                  VK_FORMAT_A2B10G10R10_UNORM_PACK32, kStorageUsage, kGlRgb10A2, false);
        }
        if (okAlloc && useHudless) {
            okAlloc = s.ensureTex(s.texHdrHudlessOut, frame->displayWidth, frame->displayHeight,
                                  stage2VkFmt, kStorageUsage, stage2GlFmt, false);
        }
    }

    const uint32_t mvW = frame->renderWidth > 0 ? frame->renderWidth : frame->displayWidth;
    const uint32_t mvH = frame->renderHeight > 0 ? frame->renderHeight : frame->displayHeight;
    if (okAlloc && wantFg) {
        okAlloc = s.ensureTex(s.texDepthIn, mvW, mvH, VK_FORMAT_R32_SFLOAT, kSrvUsage, kGlR32f, true) &&
                  s.ensureTex(s.texMvecIn, mvW, mvH, VK_FORMAT_R16G16_SFLOAT, kSrvUsage, kGlRg16f, true);
        for (uint32_t i = 0; i < DlssFgEngine::kMaxGeneratedFrames && okAlloc; ++i) {
            okAlloc = s.ensureTex(s.texInterpOut[i], frame->displayWidth, frame->displayHeight,
                                  stage2VkFmt, kStorageUsage, stage2GlFmt, true);
        }
    }

    if (!okAlloc) {
        s.setStatusError(-7, "Failed to allocate Stage-2 GL<->VK shared textures");
        s.depthMvecFreshThisFrame = false;
        return -7;
    }

    static bool loggedFirstEntry = false;
    static bool loggedFirstSuccess = false;
    static uint32_t errLogCount = 0;
    const bool copyDepthMvec = wantFg && !s.depthMvecFreshThisFrame && frame->depthInTexGlId && frame->mvecInTexGlId;
    if (!loggedFirstEntry) {
        loggedFirstEntry = true;
        logNative("linrtx_evaluate_post: first call disp=%ux%u wantFg=%d wantHdr=%d wantVkSwap=%d hudless=%d depthMvecFresh=%d hdrEngine=%d fgMode=%d",
                  frame->displayWidth, frame->displayHeight, wantFg ? 1 : 0, wantHdr ? 1 : 0,
                  wantVkSwap ? 1 : 0, useHudless ? 1 : 0, s.depthMvecFreshThisFrame ? 1 : 0,
                  frame->hdrEngine, frame->fgMode);
    }

    SharedTexture* glInTexs[4]{};
    uint32_t glInCount = 0;
    s.ctx.copyGlToShared(frame->backbufferInTexGlId, s.texBackbufferIn, frame->displayWidth, frame->displayHeight);
    glInTexs[glInCount++] = &s.texBackbufferIn;
    if (useHudless) {
        s.ctx.copyGlToShared(frame->hudlessInTexGlId, s.texHudlessIn, frame->displayWidth, frame->displayHeight);
        glInTexs[glInCount++] = &s.texHudlessIn;
    }
    if (copyDepthMvec) {
        s.ctx.copyGlToShared(frame->depthInTexGlId, s.texDepthIn, mvW, mvH);
        s.ctx.copyGlToShared(frame->mvecInTexGlId, s.texMvecIn, mvW, mvH);
        glInTexs[glInCount++] = &s.texDepthIn;
        glInTexs[glInCount++] = &s.texMvecIn;
    }
    s.depthMvecFreshThisFrame = false;

    const bool glSignaled = s.ctx.signalGlToVkSemaphore(glInTexs, glInCount);

    SharedTexture* currentRealTex = &s.texBackbufferIn;
    SharedTexture* currentHudlessTex = useHudless ? &s.texHudlessIn : nullptr;

    std::string err;
    int32_t errCode = 0;
    uint32_t genCount = 0;
    const bool glPresentActive = (!wantVkSwap || !s.presenter.active());
    const bool copyRealBackToGl = glPresentActive && (frame->outHdrOrFgTexGlId != 0);
    const bool wantCopyInterp0BackToGl = glPresentActive && (frame->outInterpTexGlId != 0);
    bool vkSignaledGl = false;

    const int32_t postRc = ngxWorker().run([&]() -> int32_t {
        if (!s.ctx.beginCommands(glSignaled)) {
            err = "beginCommands failed in Stage-2";
            errCode = -8;
            return -8;
        }

        if (wantHdr) {
            bool usedTrueHdr = false;
            bool usedShader = false;
            if (!s.rtxHdr.evaluateHdr(
                    s.ctx, s.ctx.cmdBuf, *frame,
                    s.texBackbufferIn, s.texTrueHdrPqTmp, s.texHdrOut,
                    usedTrueHdr, usedShader, err, errCode)) {
                s.ctx.endAndSubmitCommands(false, true);
                s.setStatusError(errCode, err);
                std::snprintf(s.status.trueHdrStatus, sizeof(s.status.trueHdrStatus), "%s", s.rtxHdr.statusString().c_str());
                return -9;
            }
            s.status.trueHdrActive = usedTrueHdr ? 1 : 0;
            s.status.hdrShaderActive = usedShader ? 1 : 0;
            std::snprintf(s.status.trueHdrStatus, sizeof(s.status.trueHdrStatus), "%s", s.rtxHdr.statusString().c_str());
            currentRealTex = &s.texHdrOut;

            if (useHudless) {
                bool hTrue = false, hSh = false;
                std::string hErr;
                int32_t hCode = 0;
                if (s.rtxHdr.evaluateHdr(
                        s.ctx, s.ctx.cmdBuf, *frame,
                        s.texHudlessIn, s.texTrueHdrPqTmp, s.texHdrHudlessOut,
                        hTrue, hSh, hErr, hCode, false)) {
                    currentHudlessTex = &s.texHdrHudlessOut;
                }
            }
        } else {
            std::snprintf(s.status.trueHdrStatus, sizeof(s.status.trueHdrStatus), "Off");
        }

        if (wantFg) {
            const bool fgOk = s.dlssFg.evaluate(
                s.ctx, s.ctx.cmdBuf, *frame,
                *currentRealTex, currentHudlessTex, s.texDepthIn, s.texMvecIn,
                s.texInterpOut, genCount, err, errCode);
            if (!fgOk) {
                s.ctx.endAndSubmitCommands(false, true);
                s.setStatusError(errCode, err);
                return -11;
            }
            s.status.dlssgActive = 1;
            s.status.generatedFrameCount = genCount;
            s.lastGeneratedCount = genCount;
        }

        const bool syncOutputsToGl = glPresentActive && (copyRealBackToGl || genCount > 0);
        if (syncOutputsToGl) {
            if (copyRealBackToGl) {
                s.ctx.cmdBarrier(s.ctx.cmdBuf, *currentRealTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
            for (uint32_t i = 0; i < genCount; ++i) {
                s.ctx.cmdBarrier(s.ctx.cmdBuf, s.texInterpOut[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
            if (!s.ctx.endAndSubmitCommands(true, false, &vkSignaledGl)) {
                err = "endAndSubmitCommands failed in Stage-2";
                errCode = -12;
                s.setStatusError(-12, err);
                return -12;
            }
        } else {
            if (!s.ctx.endAndSubmitCommands(false, true, &vkSignaledGl)) {
                err = "endAndSubmitCommands failed for Vulkan present";
                errCode = -12;
                s.setStatusError(-12, err);
                return -12;
            }
        }
        return 0;
    });

    if (postRc < 0) {
        if (errLogCount < 8) {
            ++errLogCount;
            logNative("linrtx_evaluate_post: FAILED postRc=%d errCode=%d err='%s'",
                      postRc, errCode, err.c_str());
        }
        if (postRc == -8) {
            s.setStatusError(errCode, err);
        }
        return postRc;
    }

    if (vkSignaledGl) {
        const SharedTexture* glOutTexs[1 + DlssFgEngine::kMaxGeneratedFrames]{};
        uint32_t glOutCount = 0;
        if (copyRealBackToGl) {
            glOutTexs[glOutCount++] = currentRealTex;
        }
        for (uint32_t i = 0; i < genCount; ++i) {
            glOutTexs[glOutCount++] = &s.texInterpOut[i];
        }
        s.ctx.waitVkToGlSemaphore(glOutTexs, glOutCount);
    }

    const bool copyInterp0BackToGl = wantCopyInterp0BackToGl && (genCount > 0);
    if (copyRealBackToGl) {
        s.ctx.copySharedToGl(*currentRealTex, frame->outHdrOrFgTexGlId, frame->displayWidth, frame->displayHeight);
    }
    if (copyInterp0BackToGl) {
        s.ctx.copySharedToGl(s.texInterpOut[0], frame->outInterpTexGlId, frame->displayWidth, frame->displayHeight);
    }
    if (!loggedFirstSuccess) {
        loggedFirstSuccess = true;
        logNative("linrtx_evaluate_post: first success glSignaled=%d vkSignaledGl=%d genCount=%u trueHdr=%d hdrShader=%d dlssg=%d",
                  glSignaled ? 1 : 0, vkSignaledGl ? 1 : 0, genCount,
                  s.status.trueHdrActive, s.status.hdrShaderActive, s.status.dlssgActive);
    }

    s.lastRealFrameTex = currentRealTex;
    s.clearStatusError();
    return static_cast<int32_t>(genCount);
}

int32_t linrtx_fetch_generated_frame(uint32_t interpIndex, uint32_t dstGlTexId) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    if (!s.initialized || !dstGlTexId || interpIndex >= s.lastGeneratedCount) {
        return -1;
    }
    const SharedTexture& src = s.texInterpOut[interpIndex];
    if (!src.valid()) return -2;

    s.ctx.waitGpuIdleFence();
    return s.ctx.copySharedToGl(src, dstGlTexId, src.width, src.height) ? 0 : -3;
}

int32_t linrtx_vk_present_frame(int32_t interpIndex) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    if (!s.initialized || !s.presenter.active()) return -1;
    SharedTexture* src = nullptr;
    if (interpIndex < 0) {
        src = s.lastRealFrameTex;
    } else if (static_cast<uint32_t>(interpIndex) < s.lastGeneratedCount) {
        src = &s.texInterpOut[static_cast<uint32_t>(interpIndex)];
    }
    if (!src || !src->valid()) return -2;

    return s.presenter.presentTexture(s.ctx, *src) ? 0 : -3;
}

void linrtx_resize_display(uint32_t displayW, uint32_t displayH) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    if (!s.initialized || !s.presenter.active()) return;
    s.presenter.resize(s.ctx, displayW, displayH, 800.0f);
    s.status.vkSwapchainActive = s.presenter.active() ? 1 : 0;
    s.status.vkSwapchainHdr10 = s.presenter.isHdr10() ? 1 : 0;
    std::snprintf(s.status.swapchainStatus, sizeof(s.status.swapchainStatus), "%s", s.presenter.statusString().c_str());
}

void linrtx_get_status(LinRtxStatus* outStatus) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    if (!outStatus) return;
    s.status.structSize = sizeof(LinRtxStatus);
    *outStatus = s.status;
}

void linrtx_shutdown(void) {
    EngineState& s = engine();
    std::lock_guard<std::mutex> lock(s.mu);

    if (!s.initialized) return;
    logNative("linrtx_shutdown: shutting down engine");
    s.presenter.destroy(s.ctx);
    s.destroyAllTextures();
    ngxWorker().run([&]() {
        s.preTonemap.destroy(s.ctx);
        s.svgf.destroy(s.ctx);
        s.dlssNr.destroy(s.ctx);
        s.rtxHdr.destroy(s.ctx);
        s.dlssFg.destroy(s.ctx);
        s.dlssRr.destroy(s.ctx);
        s.dlssSr.destroy(s.ctx);
    });
    s.ctx.shutdown();
    ngxWorker().shutdown();
    s.initialized = false;
    s.status = {};
    closeNativeLog();
}

} // extern "C"
