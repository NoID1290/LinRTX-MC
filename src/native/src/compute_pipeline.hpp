#pragma once

#include "vk_ngx_context.hpp"

#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>

namespace linrtx {

/**
 * Reusable compute pipeline helper supporting 1..N sampled textures (bindings 0..S-1)
 * followed by 1..M storage images (bindings S..S+M-1) and push constants.
 */
class ComputePass {
public:
    bool init(VkDevice device,
              const uint32_t* spirvWords,
              size_t spirvByteSize,
              uint32_t pushConstantSize);

    bool initMulti(VkDevice device,
                   const uint32_t* spirvWords,
                   size_t spirvByteSize,
                   uint32_t samplerCount,
                   uint32_t storageCount,
                   uint32_t pushConstantSize);

    void destroy(VkDevice device);

    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

    void dispatch(VkCommandBuffer cmd,
                  VkDevice device,
                  const SharedTexture& srcTex,
                  const SharedTexture& dstTex,
                  uint32_t width,
                  uint32_t height,
                  const void* pushConstants,
                  uint32_t pushConstantSize);

    void dispatchMulti(VkCommandBuffer cmd,
                       VkDevice device,
                       const SharedTexture* const* samplers,
                       uint32_t samplerCount,
                       const SharedTexture* const* storageImages,
                       uint32_t storageCount,
                       uint32_t width,
                       uint32_t height,
                       const void* pushConstants,
                       uint32_t pushConstantSize);

private:
    static constexpr uint32_t kMaxSets = 16;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSets_[kMaxSets]{};
    uint32_t nextSet_ = 0;
    uint32_t samplerCount_ = 0;
    uint32_t storageCount_ = 0;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

} // namespace linrtx
