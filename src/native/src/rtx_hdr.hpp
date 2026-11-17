#pragma once

#include "compute_pipeline.hpp"
#include "vk_ngx_context.hpp"

#include <cstdint>
#include <string>

namespace linrtx {

/**
 * Manages GPU compute CAS sharpening + RTX HDR pipeline:
 *  - Optional dynamic NVIDIA TrueHDR NGX Feature (Feature 14, libnvidia-ngx-truehdr.so)
 *  - Built-in Vulkan compute inverse-tonemapper / BT.2020 ST.2084 PQ / scRGB shader (rtx_hdr.comp)
 *  - Built-in Contrast-Adaptive Sharpening shader (cas_sharpen.comp)
 */
class RtxHdrEngine {
public:
    bool init(VkNgxContext& ctx, const char* trueHdrSearchDirs);
    void destroy(VkNgxContext& ctx);

    /**
     * Run CAS sharpening pass on upscaled DLSS output.
     * srcTex must transition cleanly; dstTex receives sharpened result.
     */
    bool applyCasSharpen(VkNgxContext& ctx,
                         VkCommandBuffer cmd,
                         SharedTexture& srcTex,
                         SharedTexture& dstTex,
                         uint32_t width,
                         uint32_t height,
                         float sharpness);

    /**
     * Apply RTX HDR / inverse-tonemap conversion from srcTex -> dstTex.
     * Returns true if TrueHDR NGX or built-in GPU compute shader wrote dstTex.
     */
    bool evaluateHdr(VkNgxContext& ctx,
                     VkCommandBuffer cmd,
                     const LinRtxFrameConfig& frame,
                     SharedTexture& srcTex,
                     SharedTexture& trueHdrTempPqTex,
                     SharedTexture& dstTex,
                     bool& outUsedTrueHdr,
                     bool& outUsedShader,
                     std::string& outError,
                     int32_t& outErrorCode,
                     bool allowTrueHdr = true);

    bool trueHdrLibraryPresent() const { return trueHdrLibPresent_; }
    const std::string& trueHdrLibraryPath() const { return trueHdrLibPath_; }
    const std::string& statusString() const { return statusStr_; }

private:
    ComputePass casPass_;
    ComputePass hdrPass_;

    void* trueHdrDlHandle_ = nullptr;
    bool trueHdrProbed_ = false;
    bool trueHdrLibPresent_ = false;
    bool trueHdrInitFailed_ = false;
    std::string trueHdrSearchDirs_;
    std::string trueHdrLibPath_;
    std::string statusStr_ = "Inactive";

    NVSDK_NGX_Parameter* trueHdrParams_ = nullptr;
    NVSDK_NGX_Handle* trueHdrHandle_ = nullptr;
    uint32_t trueHdrW_ = 0;
    uint32_t trueHdrH_ = 0;
    uint64_t frameCounter_ = 0;

    bool discoverTrueHdr(const char* searchDirs);
    bool evaluateTrueHdrNgx(VkNgxContext& ctx,
                            VkCommandBuffer cmd,
                            const LinRtxFrameConfig& frame,
                            SharedTexture& srcTex,
                            SharedTexture& dstPqTex,
                            int32_t& outErrCode);
};

} // namespace linrtx
