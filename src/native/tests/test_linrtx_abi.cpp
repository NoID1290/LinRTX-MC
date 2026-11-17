#include "linrtx_api.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

int main() {
    // 1. Verify C ABI struct sizes and offsets match Java FFM MemoryLayout
    static_assert(sizeof(LinRtxInitConfig) == 72);
    static_assert(offsetof(LinRtxInitConfig, structSize) == 0);
    static_assert(offsetof(LinRtxInitConfig, enableValidation) == 4);
    static_assert(offsetof(LinRtxInitConfig, appId) == 8);
    static_assert(offsetof(LinRtxInitConfig, windowHandle) == 16);
    static_assert(offsetof(LinRtxInitConfig, nativeDisplayPtr) == 24);
    static_assert(offsetof(LinRtxInitConfig, nativeWindowHandle) == 32);
    static_assert(offsetof(LinRtxInitConfig, isWayland) == 40);
    static_assert(offsetof(LinRtxInitConfig, ngxLibDir) == 48);
    static_assert(offsetof(LinRtxInitConfig, trueHdrSearchDirs) == 56);
    static_assert(offsetof(LinRtxInitConfig, logDir) == 64);

    static_assert(sizeof(LinRtxFrameConfig) == 600);
    static_assert(offsetof(LinRtxFrameConfig, colorInTexGlId) == 108);
    static_assert(offsetof(LinRtxFrameConfig, viewToClip) == 208);
    static_assert(offsetof(LinRtxFrameConfig, prevClipToClip) == 400);
    static_assert(offsetof(LinRtxFrameConfig, denoiseMode) == 464);
    static_assert(offsetof(LinRtxFrameConfig, diffuseAlbedoInTexGlId) == 496);
    static_assert(offsetof(LinRtxFrameConfig, worldToView) == 532);

    static_assert(sizeof(LinRtxDenoiseConfig) == 96);

    static_assert(sizeof(LinRtxStatus) == 1104);
    static_assert(offsetof(LinRtxStatus, gpuName) == 64);
    static_assert(offsetof(LinRtxStatus, lastErrorMsg) == 640);
    static_assert(offsetof(LinRtxStatus, dlssdSupported) == 896);
    static_assert(offsetof(LinRtxStatus, dlssdVersion) == 912);
    static_assert(offsetof(LinRtxStatus, denoiserStatus) == 976);

    std::printf("LinRtxInitConfig sizeof=%zu logDir_off=%zu\n",
                sizeof(LinRtxInitConfig), offsetof(LinRtxInitConfig, logDir));
    std::printf("LinRtxFrameConfig sizeof=%zu denoiseMode_off=%zu worldToView_off=%zu\n",
                sizeof(LinRtxFrameConfig),
                offsetof(LinRtxFrameConfig, denoiseMode),
                offsetof(LinRtxFrameConfig, worldToView));
    std::printf("LinRtxDenoiseConfig sizeof=%zu\n", sizeof(LinRtxDenoiseConfig));
    std::printf("LinRtxStatus sizeof=%zu dlssdSupported_off=%zu denoiserStatus_off=%zu\n",
                sizeof(LinRtxStatus),
                offsetof(LinRtxStatus, dlssdSupported),
                offsetof(LinRtxStatus, denoiserStatus));

    // Print critical offsets so Java FFM MemoryLayout stays 100% locked
    std::printf("FrameConfig offsets: displayWidth=%zu sharpness=%zu hdrContrast=%zu colorIn=%zu cameraNear=%zu viewToClip=%zu prevClipToClip=%zu\n",
                offsetof(LinRtxFrameConfig, displayWidth),
                offsetof(LinRtxFrameConfig, sharpness),
                offsetof(LinRtxFrameConfig, hdrContrast),
                offsetof(LinRtxFrameConfig, colorInTexGlId),
                offsetof(LinRtxFrameConfig, cameraNear),
                offsetof(LinRtxFrameConfig, viewToClip),
                offsetof(LinRtxFrameConfig, prevClipToClip));

    // 2. Verify invalid init struct guard returns clean error without crashing
    LinRtxInitConfig bad{};
    bad.structSize = 12;
    assert(linrtx_init(&bad) == -10);

    // 3. Verify analytical optimal resolution calculation (works even without NVIDIA Vulkan ICD)
    uint32_t rw = 0, rh = 0;
    float sharp = -1.0f;
    int32_t scaleRc = linrtx_get_optimal_scale(LINRTX_DLSS_QUALITY, 2560, 1440, &rw, &rh, &sharp);
    assert(scaleRc == 0 || scaleRc == 1);
    assert(rw > 0 && rw < 2560);
    assert(rh > 0 && rh < 1440);
    assert((rw & 1u) == 0u && (rh & 1u) == 0u);
    std::printf("DLSS Quality 1440p -> %ux%u (rc=%d)\n", rw, rh, scaleRc);

    rw = rh = 0;
    linrtx_get_optimal_scale(LINRTX_DLSS_DLAA, 1920, 1080, &rw, &rh, &sharp);
    assert(rw == 1920 && rh == 1080);

    rw = rh = 0;
    linrtx_get_optimal_scale(LINRTX_DLSS_ULTRA_PERFORMANCE, 3840, 2160, &rw, &rh, &sharp);
    assert(rw >= 1278 && rw <= 1282 && rh >= 718 && rh <= 722);

    // 4. Verify status readout & safe shutdown without prior successful GPU init
    LinRtxStatus st{};
    linrtx_get_status(&st);
    assert(st.structSize == sizeof(LinRtxStatus));
    linrtx_shutdown();

    std::printf("PASS: test_linrtx_abi\n");
    return 0;
}
