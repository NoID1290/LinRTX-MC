#pragma once

#include "compute_pipeline.hpp"
#include "vk_ngx_context.hpp"

#include <cstdint>
#include <string>

namespace linrtx {

/**
 * Temporal variance-guided SVGF + edge-aware Atrous wavelet GPU denoiser running
 * natively on Vulkan Compute. Supports two independent temporal history banks
 * (one for Iris pre-tonemap linear HDR, one for Stage 1 world denoising).
 */
class SvgfDenoiser {
public:
    struct Params {
        uint32_t width = 0;
        uint32_t height = 0;
        bool resetHistory = false;
        bool hasAlbedo = false;
        bool hasNormals = false;
        bool normalsPacked01 = false;
        bool demodulateAlbedo = false;
        bool inputIsSrgb = false;
        bool preserveAlpha = false;
        bool reverseZ = true;
        float nearPlane = 0.05f;
        float farPlane = 256.0f;
        float temporalBlend = 0.88f;
        float strength = 1.0f;
        uint32_t atrousPasses = 4;
        uint32_t bankIndex = 0; // 0 = Stage 1, 1 = Pre-tonemap Iris HDR
    };

    bool init(VkNgxContext& ctx);
    void destroy(VkNgxContext& ctx);

    bool evaluate(VkNgxContext& ctx,
                  VkCommandBuffer cmd,
                  const Params& params,
                  SharedTexture& colorIn,
                  SharedTexture& depthIn,
                  SharedTexture& mvecIn,
                  SharedTexture* albedoOpt,
                  SharedTexture* normalsOpt,
                  SharedTexture& colorOut,
                  std::string& outError);

    void resetHistory(uint32_t bankIndex);

private:
    struct HistoryBank {
        SharedTexture pingA;
        SharedTexture pingB;
        SharedTexture histColor[2];
        SharedTexture histMoments[2];
        uint32_t currIdx = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool validHistory = false;

        bool ensureSize(VkNgxContext& ctx, uint32_t w, uint32_t h);
        void destroy(VkNgxContext& ctx);
    };

    ComputePass temporalPass_;
    ComputePass atrousPass_;
    SharedTexture fallbackTex_;
    HistoryBank banks_[2]{};
    bool initialized_ = false;
};

} // namespace linrtx
