#include "compute_pipeline.hpp"

#include <vector>

namespace linrtx {

bool ComputePass::init(VkDevice device,
                       const uint32_t* spirvWords,
                       size_t spirvByteSize,
                       uint32_t pushConstantSize) {
    return initMulti(device, spirvWords, spirvByteSize, 1u, 1u, pushConstantSize);
}

bool ComputePass::initMulti(VkDevice device,
                            const uint32_t* spirvWords,
                            size_t spirvByteSize,
                            uint32_t samplerCount,
                            uint32_t storageCount,
                            uint32_t pushConstantSize) {
    destroy(device);
    if (device == VK_NULL_HANDLE || !spirvWords || spirvByteSize == 0 ||
        (samplerCount == 0 && storageCount == 0)) {
        return false;
    }
    samplerCount_ = samplerCount;
    storageCount_ = storageCount;

    if (samplerCount > 0) {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &sci, nullptr, &sampler_) != VK_SUCCESS) {
            destroy(device);
            return false;
        }
    }

    const uint32_t totalBindings = samplerCount + storageCount;
    std::vector<VkDescriptorSetLayoutBinding> bindings(totalBindings);
    for (uint32_t i = 0; i < totalBindings; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = (i < samplerCount)
                                         ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                         : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = totalBindings;
    dslci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &setLayout_) != VK_SUCCESS) {
        destroy(device);
        return false;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = pushConstantSize;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    plci.pushConstantRangeCount = pushConstantSize > 0 ? 1u : 0u;
    plci.pPushConstantRanges = pushConstantSize > 0 ? &pcr : nullptr;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &pipeLayout_) != VK_SUCCESS) {
        destroy(device);
        return false;
    }

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spirvByteSize;
    smci.pCode = spirvWords;
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smci, nullptr, &shaderModule) != VK_SUCCESS) {
        destroy(device);
        return false;
    }

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shaderModule;
    cpci.stage.pName = "main";
    cpci.layout = pipeLayout_;

    VkResult res = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline_);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    if (res != VK_SUCCESS) {
        destroy(device);
        return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    uint32_t poolSizeCount = 0;
    if (samplerCount > 0) {
        poolSizes[poolSizeCount].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[poolSizeCount].descriptorCount = kMaxSets * samplerCount;
        ++poolSizeCount;
    }
    if (storageCount > 0) {
        poolSizes[poolSizeCount].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[poolSizeCount].descriptorCount = kMaxSets * storageCount;
        ++poolSizeCount;
    }

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = kMaxSets;
    dpci.poolSizeCount = poolSizeCount;
    dpci.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device, &dpci, nullptr, &pool_) != VK_SUCCESS) {
        destroy(device);
        return false;
    }

    VkDescriptorSetLayout layouts[kMaxSets];
    for (uint32_t i = 0; i < kMaxSets; ++i) layouts[i] = setLayout_;

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_;
    dsai.descriptorSetCount = kMaxSets;
    dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device, &dsai, descSets_) != VK_SUCCESS) {
        destroy(device);
        return false;
    }
    nextSet_ = 0;

    return true;
}

void ComputePass::destroy(VkDevice device) {
    if (device != VK_NULL_HANDLE) {
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, pool_, nullptr);
        }
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline_, nullptr);
        }
        if (pipeLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeLayout_, nullptr);
        }
        if (setLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
        }
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler_, nullptr);
        }
    }
    pool_ = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < kMaxSets; ++i) descSets_[i] = VK_NULL_HANDLE;
    nextSet_ = 0;
    pipeline_ = VK_NULL_HANDLE;
    pipeLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    samplerCount_ = 0;
    storageCount_ = 0;
}

void ComputePass::dispatch(VkCommandBuffer cmd,
                           VkDevice device,
                           const SharedTexture& srcTex,
                           const SharedTexture& dstTex,
                           uint32_t width,
                           uint32_t height,
                           const void* pushConstants,
                           uint32_t pushConstantSize) {
    const SharedTexture* samplers[1] = {&srcTex};
    const SharedTexture* storages[1] = {&dstTex};
    dispatchMulti(cmd, device, samplers, 1u, storages, 1u, width, height, pushConstants, pushConstantSize);
}

void ComputePass::dispatchMulti(VkCommandBuffer cmd,
                                VkDevice device,
                                const SharedTexture* const* samplers,
                                uint32_t samplerCount,
                                const SharedTexture* const* storageImages,
                                uint32_t storageCount,
                                uint32_t width,
                                uint32_t height,
                                const void* pushConstants,
                                uint32_t pushConstantSize) {
    if (!valid() || width == 0 || height == 0 ||
        samplerCount != samplerCount_ || storageCount != storageCount_) {
        return;
    }

    const uint32_t total = samplerCount + storageCount;
    std::vector<VkDescriptorImageInfo> infos(total);
    std::vector<VkWriteDescriptorSet> writes(total);

    VkDescriptorSet set = descSets_[nextSet_];
    nextSet_ = (nextSet_ + 1u) % kMaxSets;

    for (uint32_t i = 0; i < samplerCount; ++i) {
        if (!samplers || !samplers[i] || !samplers[i]->valid()) return;
        infos[i].sampler = sampler_;
        infos[i].imageView = samplers[i]->view;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }

    for (uint32_t i = 0; i < storageCount; ++i) {
        const uint32_t idx = samplerCount + i;
        if (!storageImages || !storageImages[i] || !storageImages[i]->valid()) return;
        infos[idx].imageView = storageImages[i]->view;
        infos[idx].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        writes[idx].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[idx].dstSet = set;
        writes[idx].dstBinding = idx;
        writes[idx].descriptorCount = 1;
        writes[idx].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[idx].pImageInfo = &infos[idx];
    }

    vkUpdateDescriptorSets(device, total, writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set, 0, nullptr);
    if (pushConstants && pushConstantSize > 0) {
        vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantSize, pushConstants);
    }
    const uint32_t groupsX = (width + 15u) / 16u;
    const uint32_t groupsY = (height + 15u) / 16u;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
}

} // namespace linrtx
