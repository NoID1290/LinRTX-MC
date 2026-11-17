#ifndef LINRTX_API_H
#define LINRTX_API_H

#include "linrtx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the native LinRTX engine from the active OpenGL render thread.
 * Queries the current OpenGL GPU UUID (GL_EXT_memory_object) to bind a matching
 * NVIDIA Vulkan device, initializes NGX core & libraries, and sets up interop.
 * Returns 0 on success, non-zero negative error code on fatal setup failure.
 */
int32_t linrtx_init(const LinRtxInitConfig* config);

/**
 * Query optimal internal render resolution for the requested DLSS quality mode.
 * Returns 0 if NGX settings were queried, or 1 if analytical fallback ratios were used.
 */
int32_t linrtx_get_optimal_scale(
    int32_t dlssMode,
    uint32_t displayW,
    uint32_t displayH,
    uint32_t* outRenderW,
    uint32_t* outRenderH,
    float* outRecommendedSharpness);

/**
 * Stage 1 (pre-HUD): Run DLSS 4.5 Super Resolution / DLAA and optional CAS sharpen.
 * Reads GL textures (color, R32F depth, RG16F MVs) via zero-copy Vulkan interop
 * and writes upscaled RGBA result into frame->outUpscaledTexGlId.
 * Returns 0 on success.
 */
int32_t linrtx_evaluate_dlss(const LinRtxFrameConfig* frame);

/**
 * Optional Pre-Tonemap SVGF + Atrous compute denoiser pass.
 * Denoises linear HDR radiance in-place or into outHdrColorTexGlId using depth,
 * motion vectors, and optional G-buffer albedo/normal guides.
 * Returns 0 on success.
 */
int32_t linrtx_evaluate_denoise(const LinRtxDenoiseConfig* config);

/**
 * Stage 2 (post-HUD): Run DLSS-G Frame Generation and/or RTX HDR processing.
 * Also drives optional Vulkan HDR10 swapchain presentation if configured.
 * Returns >= 0 on success (writes status.generatedFrameCount).
 */
int32_t linrtx_evaluate_post(const LinRtxFrameConfig* frame);

/**
 * Copy a specific DLSS-G generated frame (0 .. generatedFrameCount-1) into dstGlTexId
 * for multi-frame OpenGL presentation pacing (2x / 3x / 4x).
 * Returns 0 on success.
 */
int32_t linrtx_fetch_generated_frame(uint32_t interpIndex, uint32_t dstGlTexId);

/**
 * Present a Vulkan swapchain frame index (-1 = real frame, >= 0 = generated frame index)
 * when presentBackend == LINRTX_PRESENT_VULKAN_HDR.
 */
int32_t linrtx_vk_present_frame(int32_t interpIndex);

/**
 * Resize/reconfigure Vulkan presenter surface if active.
 */
void linrtx_resize_display(uint32_t displayW, uint32_t displayH);

/**
 * Copy current diagnostic state into outStatus.
 */
void linrtx_get_status(LinRtxStatus* outStatus);

/**
 * Cleanly release all NGX features, Vulkan resources, and GL external memory objects.
 */
void linrtx_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* LINRTX_API_H */
