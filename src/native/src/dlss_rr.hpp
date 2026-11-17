#pragma once

#include "compute_pipeline.hpp"
#include "vk_ngx_context.hpp"

#include <cstdint>
#include <string>

namespace linrtx {

/**
 * Manages the NVIDIA DLSS Ray Reconstruction (DLSSD / NVSDK_NGX_Feature_RayReconstruction)
 * feature handle, live preset switching (Default/D/E/F), and automatic Vulkan compute
 * G-buffer guide synthesis (diffuse albedo, specular albedo, view-space normals, roughness).
 */
class DlssRrEngine {
public:
    bool queryOptimalScale(VkNgxContext& ctx,
                           int32_t dlssMode,
                           uint32_t displayW,
                           uint32_t displayH,
                           uint32_t* outRenderW,
                           uint32_t* outRenderH,
                           float* outSharpness);

    bool evaluate(VkNgxContext& ctx,
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
                  int32_t& outErrorCode);

    void destroy(VkNgxContext& ctx);

    const std::string& versionString() const { return versionStr_; }

private:
    struct FeatureKey {
        uint32_t renderW = 0;
        uint32_t renderH = 0;
        uint32_t displayW = 0;
        uint32_t displayH = 0;
        int32_t perfQuality = -1;
        int32_t createFlags = 0;
        int32_t preset = -1;

        bool operator==(const FeatureKey& o) const = default;
    };

    bool ensureSynthResources(VkNgxContext& ctx, uint32_t w, uint32_t h);
    bool ensureHdrConvertResources(VkNgxContext& ctx, uint32_t renderW, uint32_t renderH,
                                   uint32_t displayW, uint32_t displayH);
    void destroySynthResources(VkNgxContext& ctx);

    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    FeatureKey key_{};

    ComputePass guideSynthPass_;
    ComputePass colorConvertPass_;
    SharedTexture synthAlbedo_;
    SharedTexture synthSpecular_;
    SharedTexture synthNormals_;
    SharedTexture synthRoughness_;
    SharedTexture synthColorHdr_;
    SharedTexture synthOutHdr_;
    SharedTexture dummyGuideTex_;

    bool loggedFirstEval_ = false;
    uint32_t errLogCount_ = 0;
    std::string versionStr_ = "310.9.16";
};

} // namespace linrtx
