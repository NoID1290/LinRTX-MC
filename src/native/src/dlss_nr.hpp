#pragma once

#include "compute_pipeline.hpp"
#include "vk_ngx_context.hpp"

#include <cstdint>
#include <string>

namespace linrtx {

/**
 * Compute working resolution for the DLSS 5 Neural Engine analysis pass
 * (inspired by OptiScaler_DLSSNR / DLSSNR-Cost-Scaler working scale).
 */
void computeNeuralWorkingSize(uint32_t targetW,
                              uint32_t targetH,
                              float workingScale,
                              uint32_t& outWorkW,
                              uint32_t& outWorkH);

unsigned int normalizeNeuralPreset(int32_t neuralPreset);

/**
 * Manages the DLSS 5 Neural Engine Filter (DLSS-NR / NGX Feature 18):
 *  - Optional NVIDIA NGX Feature 18 (NVSDK_NGX_Feature_Reserved18) with
 *    RTX 20/30/40/50 capability & architecture unlock hints
 *  - Built-in Vulkan Compute Neural Engine Filter (neural_analysis.comp +
 *    neural_filter.comp) unlocked for any RTX / Vulkan 1.2 GPU
 */
class DlssNrEngine {
public:
    bool init(VkNgxContext& ctx, const char* neuralSearchDirs);
    void destroy(VkNgxContext& ctx);

    bool evaluate(VkNgxContext& ctx,
                  VkCommandBuffer cmd,
                  const LinRtxFrameConfig& frame,
                  SharedTexture& srcColor,
                  SharedTexture& depthIn,
                  SharedTexture& mvecIn,
                  SharedTexture& dstColor,
                  int32_t& outActivePreset,
                  bool& outUsedNgx,
                  std::string& outError,
                  int32_t& outErrorCode);

    bool ngxLibraryPresent() const { return dlssnrLibPresent_; }
    const std::string& versionString() const { return versionStr_; }
    const std::string& statusString() const { return statusStr_; }

private:
    struct NgxKey {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t vkFormat = 0;
        uint32_t outVkFormat = 0;
        uint32_t preset = 0;
        int32_t style = 0;
        int32_t unlockAnyRtx = 1;
        bool operator==(const NgxKey& o) const = default;
    };

    ComputePass analysisPass_;
    ComputePass filterPass_;
    SharedTexture workStats_[2]{};
    uint32_t statsPing_ = 0;
    uint32_t statsW_ = 0;
    uint32_t statsH_ = 0;
    bool historyValid_ = false;
    bool loggedFirstEval_ = false;
    uint64_t frameCounter_ = 0;

    void* dlssnrDlHandle_ = nullptr;
    bool dlssnrProbed_ = false;
    bool dlssnrLibPresent_ = false;
    bool windowsDllPresent_ = false;
    bool ngxAttemptFailed_ = false;
    int32_t lastFailedUnlockFlag_ = -1;
    int32_t lastFailedPreset_ = -1;
    std::string neuralSearchDirs_;
    std::string dlssnrLibPath_;

    NVSDK_NGX_Parameter* ngxParams_ = nullptr;
    NVSDK_NGX_Handle* ngxHandle_ = nullptr;
    NgxKey ngxKey_{};

    std::string versionStr_ = "Vulkan Neural Engine 5";
    std::string statusStr_ = "Ready";

    void discoverNgxLibrary(const char* searchDirs);
    bool ensureWorkTextures(VkNgxContext& ctx, uint32_t workW, uint32_t workH);
    void releaseNgxFeature(VkNgxContext& ctx);

    bool evaluateNgxFeature18(VkNgxContext& ctx,
                              VkCommandBuffer cmd,
                              const LinRtxFrameConfig& frame,
                              SharedTexture& srcColor,
                              SharedTexture& depthIn,
                              SharedTexture& mvecIn,
                              SharedTexture& dstColor,
                              int32_t& outActivePreset,
                              std::string& outError,
                              int32_t& outErrorCode);

    bool evaluateVulkanNeural(VkNgxContext& ctx,
                              VkCommandBuffer cmd,
                              const LinRtxFrameConfig& frame,
                              SharedTexture& srcColor,
                              SharedTexture& depthIn,
                              SharedTexture& mvecIn,
                              SharedTexture& dstColor,
                              int32_t& outActivePreset,
                              std::string& outError,
                              int32_t& outErrorCode);
};

} // namespace linrtx
