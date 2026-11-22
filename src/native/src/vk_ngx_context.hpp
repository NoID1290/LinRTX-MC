#pragma once

#include "gl_dispatch.hpp"
#include "linrtx_types.h"

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace linrtx {

struct SharedTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    GLenum glFormat = 0;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    GLuint glTex = 0;
    GLuint glMemObj = 0;

    bool valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }
};

struct SharedSemaphore {
    VkSemaphore vkSem = VK_NULL_HANDLE;
    GLuint glSem = 0;
};

struct VkNgxContext {
    GlDispatch gl;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};

    SharedSemaphore glToVkSem;
    SharedSemaphore vkToGlSem;

    PFN_vkGetMemoryFdKHR pfnGetMemoryFdKHR = nullptr;
    PFN_vkGetSemaphoreFdKHR pfnGetSemaphoreFdKHR = nullptr;
    PFN_vkSetHdrMetadataEXT pfnSetHdrMetadataEXT = nullptr;

    NVSDK_NGX_Parameter* ngxParams = nullptr;
    bool vkReady = false;
    bool ngxReady = false;
    bool dlssSupported = false;
    bool dlssdSupported = false;
    bool dlssgSupported = false;
    bool dlssnrSupported = false;
    bool hasSurfaceExts = false;
    bool hasSwapchainColorspace = false;
    bool hasHdrMetadata = false;
    bool hasCooperativeMatrix = false;
    bool hasFloat8 = false;
    bool waitingOnGl = false;
    bool ownsNgxParams = false;

    uint32_t pciVendorId = 0;
    uint32_t pciDeviceId = 0;
    int32_t maxFgMultiplier = 1;
    std::string gpuName;
    std::string gpuArchName;
    std::string driverVersion;
    std::string lastError;
    int32_t lastErrorCode = 0;

    std::vector<std::wstring> searchWideStrings;
    std::vector<const wchar_t*> searchWidePtrs;
    std::wstring appDataWidePath;

    bool init(const LinRtxInitConfig* cfg);
    void shutdown();

    bool createSharedTexture(SharedTexture& tex,
                             uint32_t w,
                             uint32_t h,
                             VkFormat vkFmt,
                             VkImageUsageFlags usage,
                             GLenum glInternalFmt,
                             bool exportToGl);
    void destroySharedTexture(SharedTexture& tex);

    void cmdBarrier(VkCommandBuffer cmd,
                    SharedTexture& tex,
                    VkImageLayout newLayout,
                    VkAccessFlags srcAccess,
                    VkAccessFlags dstAccess,
                    VkPipelineStageFlags srcStage,
                    VkPipelineStageFlags dstStage);

    bool copyGlToShared(GLuint srcGlTex,
                        const SharedTexture& dstShared,
                        uint32_t w,
                        uint32_t h);

    bool copySharedToGl(const SharedTexture& srcShared,
                        GLuint dstGlTex,
                        uint32_t w,
                        uint32_t h);

    // Must be called on the active OpenGL Render thread after copyGlToShared and before ngxWorker().
    // Returns true when glToVkSem was signaled via glSignalSemaphoreEXT.
    bool signalGlToVkSemaphore(SharedTexture* const* textures, uint32_t count);

    // Must be called on the active OpenGL Render thread after ngxWorker() submits a command
    // buffer with signalGlSemaphore=true (when outSignaledGl is true) and before copySharedToGl.
    void waitVkToGlSemaphore(const SharedTexture* const* textures, uint32_t count);

    // Pure Vulkan command buffer helpers (safe to invoke from LinRTX-NGX worker thread).
    bool beginCommands(bool waitGlSemaphore);
    bool endAndSubmitCommands(bool signalGlSemaphore, bool waitCpuFence, bool* outSignaledGl = nullptr);
    void waitGpuIdleFence();

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
    static std::wstring utf8ToWide(const char* s);
    static std::vector<std::string> splitSearchDirs(const char* raw);

    static inline NVSDK_NGX_Resource_VK makeNgxResource(const SharedTexture& tex, bool readWrite) {
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;
        return NVSDK_NGX_Create_ImageView_Resource_VK(
            tex.view, tex.image, range, tex.vkFormat, tex.width, tex.height, readWrite);
    }
};

} // namespace linrtx
