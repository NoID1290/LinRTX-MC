#pragma once

#include "vk_ngx_context.hpp"

#include <cstdint>
#include <string>

namespace linrtx {

bool toNgxPerfQuality(int32_t dlssMode, NVSDK_NGX_PerfQuality_Value& outPq);

/**
 * Resolves the NGX PerfQuality hint for both explicit modes and LINRTX_DLSS_CUSTOM
 * based on the actual render/display scaling ratio so NGX dynamic-res ranges match.
 */
NVSDK_NGX_PerfQuality_Value resolveNgxPerfQuality(int32_t dlssMode,
                                                  uint32_t renderW,
                                                  uint32_t renderH,
                                                  uint32_t displayW,
                                                  uint32_t displayH);

uint32_t clampEvenDim(uint32_t val, uint32_t maxVal);

/**
 * Manages the NVIDIA DLSS 4.5 Super Resolution NGX feature handle, live preset
 * switching (J, K, L, M, E, F, Default), and optimal scaling queries.
 */
class DlssSrEngine {
public:
    static bool queryOptimalScale(VkNgxContext& ctx,
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

    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    FeatureKey key_{};
    bool loggedFirstEval_ = false;
    uint32_t errLogCount_ = 0;
    std::string versionStr_ = "310.9.16";
};

} // namespace linrtx
