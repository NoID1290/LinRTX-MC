#pragma once

#include "vk_ngx_context.hpp"

#include <nvsdk_ngx_helpers_dlssg_vk.h>

#include <array>
#include <cstdint>
#include <string>

namespace linrtx {

/**
 * Manages NVIDIA DLSS-G Frame Generation (2x / 3x / 4x multi-frame) via Vulkan NGX.
 * Produces 1..3 intermediate frames stored in dedicated SharedTextures between real frames.
 */
class DlssFgEngine {
public:
    static constexpr uint32_t kMaxGeneratedFrames = 3;

    bool evaluate(VkNgxContext& ctx,
                  VkCommandBuffer cmd,
                  const LinRtxFrameConfig& frame,
                  SharedTexture& backbuffer,
                  SharedTexture* hudlessOpt,
                  SharedTexture& depth,
                  SharedTexture& mvec,
                  std::array<SharedTexture, kMaxGeneratedFrames>& interpOutputs,
                  uint32_t& outGeneratedCount,
                  std::string& outError,
                  int32_t& outErrorCode);

    void resetHistory();
    void destroy(VkNgxContext& ctx);

    const std::string& versionString() const { return versionStr_; }

private:
    struct FeatureKey {
        uint32_t displayW = 0;
        uint32_t displayH = 0;
        uint32_t renderW = 0;
        uint32_t renderH = 0;
        uint32_t vkFormat = 0;
        bool useHudless = false;

        bool operator==(const FeatureKey& o) const = default;
    };

    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    FeatureKey key_{};
    uint64_t frameCounter_ = 0;
    uint32_t historyFrames_ = 0;
    bool loggedFirstEval_ = false;
    uint32_t errLogCount_ = 0;
    std::string versionStr_ = "310.9.16";
};

} // namespace linrtx
