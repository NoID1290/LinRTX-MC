#pragma once

#include "compute_pipeline.hpp"
#include "vk_ngx_context.hpp"

#include <cstdint>
#include <string>

namespace linrtx {

/**
 * Shared Vulkan compute helper for Camellia/Iris pre-tonemap linear HDR
 * preparation, progressive F1 sample-count blending, and G-buffer guide
 * completion (world->view normal transform + water/solid specular/roughness).
 */
class PreTonemapHelper {
public:
    bool ensureInit(VkNgxContext& ctx, std::string& outError);
    void destroy(VkNgxContext& ctx);

    bool prepareColorForRr(VkNgxContext& ctx,
                           VkCommandBuffer cmd,
                           uint32_t w,
                           uint32_t h,
                           bool flipY,
                           SharedTexture& rawHdrIn,
                           SharedTexture*& outHdrVkFp16,
                           std::string& outError);

    bool completeGuides(VkNgxContext& ctx,
                        VkCommandBuffer cmd,
                        uint32_t w,
                        uint32_t h,
                        SharedTexture& colorIn,
                        SharedTexture& depthIn,
                        SharedTexture* explicitAlbedo,
                        SharedTexture* explicitNormals,
                        bool normalsAreWorld,
                        bool normalsPacked01,
                        bool reverseZ,
                        float nearPlane,
                        float farPlane,
                        const float worldToView[16],
                        SharedTexture*& outAlb,
                        SharedTexture*& outSpec,
                        SharedTexture*& outNorm,
                        SharedTexture*& outRough,
                        std::string& outError);

    bool resolvePreTonemap(VkNgxContext& ctx,
                           VkCommandBuffer cmd,
                           uint32_t w,
                           uint32_t h,
                           SharedTexture& denoisedSrc,
                           SharedTexture& origRawHdr,
                           SharedTexture& outFinalHdr,
                           bool flipSrcY,
                           bool preserveAlpha,
                           bool f1AccumActive,
                           float f1FadeSamples,
                           std::string& outError);

    bool ensurePreHdrOutVk(VkNgxContext& ctx,
                           uint32_t w,
                           uint32_t h,
                           SharedTexture*& outTex,
                           std::string& outError);

    bool ensureScratchTmp(VkNgxContext& ctx,
                          uint32_t w,
                          uint32_t h,
                          VkFormat vkFmt,
                          SharedTexture*& outTex,
                          std::string& outError);

private:
    bool ensureInternalTex(VkNgxContext& ctx,
                           SharedTexture& tex,
                           uint32_t w,
                           uint32_t h,
                           VkFormat vkFmt,
                           VkImageUsageFlags usage);
    bool ensureFallbackDummy(VkNgxContext& ctx, VkCommandBuffer cmd);

    ComputePass guideCompletePass_;
    ComputePass resolvePass_;
    bool initialized_ = false;

    SharedTexture texPreHdrVk_;
    SharedTexture texPreHdrOutVk_;
    SharedTexture texPreHdrTmp_;
    SharedTexture texGuideAlb_;
    SharedTexture texGuideSpec_;
    SharedTexture texGuideNorm_;
    SharedTexture texGuideRough_;
    SharedTexture fallbackDummy_;
};

} // namespace linrtx
