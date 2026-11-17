#include "vk_presenter.hpp"

#include <dlfcn.h>
#include <algorithm>
#include <vector>

// Include platform Vulkan/X11 headers only inside this TU after project headers
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <vulkan/vulkan_xlib.h>
#ifdef Success
#undef Success
#endif
#ifdef None
#undef None
#endif

namespace linrtx {
namespace {

using PFN_XCreateWindow = Window (*)(Display*, Window, int, int, unsigned int, unsigned int, unsigned int, int, unsigned int, Visual*, unsigned long, XSetWindowAttributes*);
using PFN_XDestroyWindow = int (*)(Display*, Window);
using PFN_XMapWindow = int (*)(Display*, Window);
using PFN_XResizeWindow = int (*)(Display*, Window, unsigned int, unsigned int);
using PFN_XFlush = int (*)(Display*);
using PFN_XDefaultScreen = int (*)(Display*);
using PFN_XDefaultDepth = int (*)(Display*, int);
using PFN_XDefaultVisual = Visual* (*)(Display*, int);
using PFN_XShapeCombineRectangles = void (*)(Display*, Window, int, int, int, XRectangle*, int, int, int);

} // namespace

bool VkPresenter::createChildX11Surface(VkNgxContext& ctx,
                                        uint64_t displayPtr,
                                        uint64_t parentWindow,
                                        uint32_t w,
                                        uint32_t h) {
    if (!ctx.hasSurfaceExts || !displayPtr || !parentWindow) {
        status_ = "Vulkan surface unavailable (missing X11 window handle)";
        return false;
    }

    auto pfnCreateXlibSurface = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
        vkGetInstanceProcAddr(ctx.instance, "vkCreateXlibSurfaceKHR"));
    if (!pfnCreateXlibSurface) {
        status_ = "vkCreateXlibSurfaceKHR unavailable";
        return false;
    }

    x11Lib_ = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!x11Lib_) {
        status_ = "libX11.so.6 unavailable";
        return false;
    }
    xextLib_ = dlopen("libXext.so.6", RTLD_LAZY | RTLD_LOCAL);

    auto pXCreateWindow = reinterpret_cast<PFN_XCreateWindow>(dlsym(x11Lib_, "XCreateWindow"));
    auto pXMapWindow = reinterpret_cast<PFN_XMapWindow>(dlsym(x11Lib_, "XMapWindow"));
    auto pXFlush = reinterpret_cast<PFN_XFlush>(dlsym(x11Lib_, "XFlush"));
    auto pXDefaultScreen = reinterpret_cast<PFN_XDefaultScreen>(dlsym(x11Lib_, "XDefaultScreen"));
    auto pXDefaultDepth = reinterpret_cast<PFN_XDefaultDepth>(dlsym(x11Lib_, "XDefaultDepth"));
    auto pXDefaultVisual = reinterpret_cast<PFN_XDefaultVisual>(dlsym(x11Lib_, "XDefaultVisual"));

    if (!pXCreateWindow || !pXMapWindow || !pXFlush || !pXDefaultScreen || !pXDefaultDepth || !pXDefaultVisual) {
        status_ = "Missing X11 symbols for child swapchain window";
        return false;
    }

    Display* dpy = reinterpret_cast<Display*>(static_cast<uintptr_t>(displayPtr));
    Window parent = static_cast<Window>(parentWindow);
    int screen = pXDefaultScreen(dpy);
    int depth = pXDefaultDepth(dpy, screen);
    Visual* visual = pXDefaultVisual(dpy, screen);

    XSetWindowAttributes swa{};
    swa.background_pixmap = 0L;
    swa.border_pixel = 0;
    swa.event_mask = 0;

    Window child = pXCreateWindow(
        dpy, parent, 0, 0, std::max(1u, w), std::max(1u, h), 0,
        depth, InputOutput, visual, CWBackPixmap | CWBorderPixel | CWEventMask, &swa);
    if (!child) {
        status_ = "XCreateWindow failed";
        return false;
    }

    // Make child window input-transparent so mouse & keyboard events hit Minecraft's parent GLFW window
    if (xextLib_) {
        auto pShape = reinterpret_cast<PFN_XShapeCombineRectangles>(dlsym(xextLib_, "XShapeCombineRectangles"));
        if (pShape) {
            pShape(dpy, child, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
        }
    }

    pXMapWindow(dpy, child);
    pXFlush(dpy);

    x11Display_ = dpy;
    childWindow_ = static_cast<unsigned long>(child);

    VkXlibSurfaceCreateInfoKHR xsci{};
    xsci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    xsci.dpy = dpy;
    xsci.window = child;

    VkResult res = pfnCreateXlibSurface(ctx.instance, &xsci, nullptr, &surface_);
    if (res != VK_SUCCESS || surface_ == VK_NULL_HANDLE) {
        status_ = "vkCreateXlibSurfaceKHR failed (" + std::to_string(res) + ")";
        return false;
    }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physicalDevice, ctx.graphicsQueueFamily, surface_, &supported);
    if (!supported) {
        status_ = "Queue family does not support Vulkan surface present";
        return false;
    }
    return true;
}

bool VkPresenter::createOrRecreateSwapchain(VkNgxContext& ctx, uint32_t w, uint32_t h, float maxNits) {
    if (surface_ == VK_NULL_HANDLE || !ctx.vkReady) return false;
    w = std::max(1u, w);
    h = std::max(1u, h);

    if (x11Display_ && childWindow_ && x11Lib_) {
        auto pXResizeWindow = reinterpret_cast<PFN_XResizeWindow>(dlsym(x11Lib_, "XResizeWindow"));
        auto pXFlush = reinterpret_cast<PFN_XFlush>(dlsym(x11Lib_, "XFlush"));
        if (pXResizeWindow) pXResizeWindow(static_cast<Display*>(x11Display_), static_cast<Window>(childWindow_), w, h);
        if (pXFlush) pXFlush(static_cast<Display*>(x11Display_));
    }

    vkDeviceWaitIdle(ctx.device);

    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice, surface_, &caps) != VK_SUCCESS) {
        status_ = "Failed to query surface capabilities";
        return false;
    }

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, surface_, &fmtCount, fmts.data());
    if (fmts.empty()) {
        status_ = "No Vulkan surface formats available";
        return false;
    }

    VkSurfaceFormatKHR chosen = fmts[0];
    hdr10_ = false;
    auto findFmt = [&](VkFormat f, VkColorSpaceKHR cs) -> bool {
        for (const auto& sf : fmts) {
            if (sf.format == f && sf.colorSpace == cs) {
                chosen = sf;
                return true;
            }
        }
        return false;
    };

    if (findFmt(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT)) {
        hdr10_ = true;
    } else if (findFmt(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)) {
        hdr10_ = true;
    } else if (!findFmt(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) {
        findFmt(VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    }

    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, surface_, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, surface_, &pmCount, pms.data());

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::find(pms.begin(), pms.end(), preferred) != pms.end()) {
            presentMode = preferred;
            break;
        }
    }

    VkExtent2D targetExtent{w, h};
    if (caps.currentExtent.width != UINT32_MAX) {
        targetExtent = caps.currentExtent;
    } else {
        targetExtent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        targetExtent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imgCount = std::max(2u, caps.minImageCount + 1u);
    if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount) {
        imgCount = caps.maxImageCount;
    }

    VkSwapchainKHR oldSwap = swapchain_;
    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = imgCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = targetExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                           ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                           : caps.currentTransform;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                             ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                             : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = oldSwap;

    VkSwapchainKHR newSwap = VK_NULL_HANDLE;
    VkResult res = vkCreateSwapchainKHR(ctx.device, &sci, nullptr, &newSwap);
    if (oldSwap != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx.device, oldSwap, nullptr);
    }
    if (res != VK_SUCCESS || newSwap == VK_NULL_HANDLE) {
        swapchain_ = VK_NULL_HANDLE;
        status_ = "vkCreateSwapchainKHR failed (" + std::to_string(res) + ")";
        return false;
    }

    swapchain_ = newSwap;
    swapFormat_ = chosen.format;
    swapColorspace_ = chosen.colorSpace;
    extent_ = targetExtent;

    uint32_t scImgCount = 0;
    vkGetSwapchainImagesKHR(ctx.device, swapchain_, &scImgCount, nullptr);
    swapImages_.resize(scImgCount);
    vkGetSwapchainImagesKHR(ctx.device, swapchain_, &scImgCount, swapImages_.data());

    if (hdr10_ && ctx.pfnSetHdrMetadataEXT) {
        VkHdrMetadataEXT meta{};
        meta.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
        meta.displayPrimaryRed = {0.708f, 0.292f};
        meta.displayPrimaryGreen = {0.170f, 0.797f};
        meta.displayPrimaryBlue = {0.131f, 0.046f};
        meta.whitePoint = {0.3127f, 0.3290f};
        meta.maxLuminance = maxNits;
        meta.minLuminance = 0.005f;
        meta.maxContentLightLevel = maxNits;
        meta.maxFrameAverageLightLevel = std::min(400.0f, maxNits * 0.4f);
        ctx.pfnSetHdrMetadataEXT(ctx.device, 1, &swapchain_, &meta);
    }

    status_ = hdr10_ ? "Vulkan HDR10 Swapchain Active" : "Vulkan SDR Swapchain Active (display non-HDR)";
    return true;
}

bool VkPresenter::init(VkNgxContext& ctx, const LinRtxInitConfig* cfg, uint32_t w, uint32_t h, float maxNits) {
    destroy(ctx);
    if (!ctx.vkReady || !cfg) return false;

    if (cfg->isWayland) {
        status_ = "Wayland native subsurface HDR10 requires XWayland or Gamescope --hdr-enabled";
        warnedUnsupported_ = true;
        return false;
    }

    if (!createChildX11Surface(ctx, cfg->nativeDisplayPtr, cfg->nativeWindowHandle, w, h)) {
        destroy(ctx);
        return false;
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = ctx.graphicsQueueFamily;
    if (vkCreateCommandPool(ctx.device, &cpci, nullptr, &presentCmdPool_) != VK_SUCCESS) {
        destroy(ctx);
        return false;
    }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = presentCmdPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx.device, &cbai, &presentCmd_) != VK_SUCCESS) {
        destroy(ctx);
        return false;
    }

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(ctx.device, &sci, nullptr, &acquireSem_);
    vkCreateSemaphore(ctx.device, &sci, nullptr, &renderSem_);

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(ctx.device, &fci, nullptr, &presentFence_);

    if (!createOrRecreateSwapchain(ctx, w, h, maxNits)) {
        destroy(ctx);
        return false;
    }

    initialized_ = true;
    return true;
}

void VkPresenter::resize(VkNgxContext& ctx, uint32_t w, uint32_t h, float maxNits) {
    if (!initialized_ || surface_ == VK_NULL_HANDLE) return;
    if (w == extent_.width && h == extent_.height) return;
    createOrRecreateSwapchain(ctx, w, h, maxNits);
}

void VkPresenter::destroySwapchain(VkNgxContext& ctx) {
    if (ctx.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(ctx.device);
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(ctx.device, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }
    swapImages_.clear();
    hdr10_ = false;
}

void VkPresenter::destroy(VkNgxContext& ctx) {
    destroySwapchain(ctx);

    if (ctx.device != VK_NULL_HANDLE) {
        if (presentFence_ != VK_NULL_HANDLE) {
            vkDestroyFence(ctx.device, presentFence_, nullptr);
            presentFence_ = VK_NULL_HANDLE;
        }
        if (acquireSem_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(ctx.device, acquireSem_, nullptr);
            acquireSem_ = VK_NULL_HANDLE;
        }
        if (renderSem_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(ctx.device, renderSem_, nullptr);
            renderSem_ = VK_NULL_HANDLE;
        }
        if (presentCmdPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(ctx.device, presentCmdPool_, nullptr);
            presentCmdPool_ = VK_NULL_HANDLE;
            presentCmd_ = VK_NULL_HANDLE;
        }
    }

    if (surface_ != VK_NULL_HANDLE && ctx.instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(ctx.instance, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (x11Display_ && childWindow_ && x11Lib_) {
        auto pDestroy = reinterpret_cast<PFN_XDestroyWindow>(dlsym(x11Lib_, "XDestroyWindow"));
        auto pFlush = reinterpret_cast<PFN_XFlush>(dlsym(x11Lib_, "XFlush"));
        if (pDestroy) pDestroy(static_cast<Display*>(x11Display_), static_cast<Window>(childWindow_));
        if (pFlush) pFlush(static_cast<Display*>(x11Display_));
    }
    childWindow_ = 0;
    x11Display_ = nullptr;

    if (xextLib_) {
        dlclose(xextLib_);
        xextLib_ = nullptr;
    }
    if (x11Lib_) {
        dlclose(x11Lib_);
        x11Lib_ = nullptr;
    }

    initialized_ = false;
}

bool VkPresenter::presentTexture(VkNgxContext& ctx, SharedTexture& srcTex) {
    if (!initialized_ || swapchain_ == VK_NULL_HANDLE || !srcTex.valid()) return false;

    vkWaitForFences(ctx.device, 1, &presentFence_, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx.device, 1, &presentFence_);

    uint32_t imgIdx = 0;
    VkResult acq = vkAcquireNextImageKHR(ctx.device, swapchain_, UINT64_MAX, acquireSem_, VK_NULL_HANDLE, &imgIdx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        createOrRecreateSwapchain(ctx, extent_.width, extent_.height, 800.0f);
        return false;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        return false;
    }

    vkResetCommandBuffer(presentCmd_, 0);
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(presentCmd_, &cbi);

    ctx.cmdBarrier(presentCmd_, srcTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImage swapImg = swapImages_[imgIdx];
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = swapImg;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(presentCmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(srcTex.width), static_cast<int32_t>(srcTex.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(extent_.width), static_cast<int32_t>(extent_.height), 1};
    vkCmdBlitImage(presentCmd_,
                   srcTex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);

    VkImageMemoryBarrier toPresent{};
    toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.image = swapImg;
    toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(presentCmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toPresent);

    vkEndCommandBuffer(presentCmd_);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquireSem_;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &presentCmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderSem_;

    if (vkQueueSubmit(ctx.graphicsQueue, 1, &si, presentFence_) != VK_SUCCESS) {
        return false;
    }

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderSem_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imgIdx;

    VkResult pres = vkQueuePresentKHR(ctx.graphicsQueue, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        createOrRecreateSwapchain(ctx, extent_.width, extent_.height, 800.0f);
    }
    return pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR;
}

} // namespace linrtx
