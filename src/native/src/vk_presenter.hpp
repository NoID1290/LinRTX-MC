#pragma once

#include "vk_ngx_context.hpp"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace linrtx {

/**
 * Optional Vulkan HDR10 swapchain presenter for native KDE Plasma 6 HDR.
 * Uses an input-transparent X11 child window (via dlopen'd libX11 + libXext)
 * so it overlays cleanly over GLFW's OpenGL window without GLX surface collisions
 * and forwards all input events directly to Minecraft.
 */
class VkPresenter {
public:
    bool init(VkNgxContext& ctx, const LinRtxInitConfig* cfg, uint32_t w, uint32_t h, float maxNits);
    void resize(VkNgxContext& ctx, uint32_t w, uint32_t h, float maxNits);
    void destroy(VkNgxContext& ctx);

    bool presentTexture(VkNgxContext& ctx, SharedTexture& srcTex);

    bool active() const { return swapchain_ != VK_NULL_HANDLE; }
    bool isHdr10() const { return hdr10_; }
    const std::string& statusString() const { return status_; }

private:
    void* x11Lib_ = nullptr;
    void* xextLib_ = nullptr;
    void* x11Display_ = nullptr; // Borrowed Display* from GLFW
    unsigned long childWindow_ = 0;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapColorspace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{0, 0};
    std::vector<VkImage> swapImages_;

    VkCommandPool presentCmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer presentCmd_ = VK_NULL_HANDLE;
    VkSemaphore acquireSem_ = VK_NULL_HANDLE;
    VkSemaphore renderSem_ = VK_NULL_HANDLE;
    VkFence presentFence_ = VK_NULL_HANDLE;

    bool hdr10_ = false;
    bool initialized_ = false;
    bool warnedUnsupported_ = false;
    std::string status_ = "OpenGL Swap (Default)";

    bool createChildX11Surface(VkNgxContext& ctx, uint64_t displayPtr, uint64_t parentWindow, uint32_t w, uint32_t h);
    bool createOrRecreateSwapchain(VkNgxContext& ctx, uint32_t w, uint32_t h, float maxNits);
    void destroySwapchain(VkNgxContext& ctx);
};

} // namespace linrtx
