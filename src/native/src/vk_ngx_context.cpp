#include "vk_ngx_context.hpp"
#include "native_log.hpp"
#include "ngx_worker.hpp"

#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params_dlssg.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <set>
#include <sstream>
#include <unistd.h>

namespace linrtx {

namespace {

constexpr const char* kProjectGuid = "a4c9f8b2-4e7d-4a1f-9c3a-8e62f1d7b042";

bool hasExtension(const std::vector<VkExtensionProperties>& props, const char* name) {
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

void appendUniqueExt(std::vector<const char*>& list, std::set<std::string>& seen, const char* ext) {
    if (ext && *ext) {
        auto [it, inserted] = seen.insert(ext);
        if (inserted) {
            list.push_back(it->c_str());
        }
    }
}

void NVSDK_CONV ngxLogCallback(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature component) {
    std::string msg = message ? message : "";
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
        msg.pop_back();
    }
    logNative("NGX[lvl=%d feat=%d]: %s",
              static_cast<int>(level),
              static_cast<int>(component),
              msg.c_str());
}

} // namespace

std::wstring VkNgxContext::utf8ToWide(const char* s) {
    if (!s || !*s) return L".";
    std::wstring out;
    out.reserve(std::strlen(s));
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        out.push_back(static_cast<wchar_t>(*p));
    }
    return out;
}

uint32_t VkNgxContext::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (memProps.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VkNgxContext::init(const LinRtxInitConfig* cfg) {
    shutdown();
    lastError.clear();
    lastErrorCode = 0;

    // Step 1: Resolve OpenGL external memory/semaphore dispatch (optional in unit tests)
    const bool glInteropAvailable = gl.init();
    logNative("vkNgx.init: gl.init=%d renderer='%s' memFd=%d semFd=%d copyImg=%d",
              glInteropAvailable ? 1 : 0,
              gl.getRendererString(),
              gl.hasExtMemoryObjectFd ? 1 : 0,
              gl.hasExtSemaphoreFd ? 1 : 0,
              gl.hasCopyImage ? 1 : 0);

    std::array<uint8_t, 16> glUuid{};
    const bool hasGlUuid = glInteropAvailable && gl.queryDeviceUuid(glUuid);
    if (hasGlUuid) {
        logNative("vkNgx.init: glDeviceUuid=%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  glUuid[0], glUuid[1], glUuid[2], glUuid[3], glUuid[4], glUuid[5], glUuid[6], glUuid[7],
                  glUuid[8], glUuid[9], glUuid[10], glUuid[11], glUuid[12], glUuid[13], glUuid[14], glUuid[15]);
    } else {
        logNative("vkNgx.init: glDeviceUuid unavailable (hasGlUuid=0)");
    }

    int glToVkFd = -1;
    int vkToGlFd = -1;

    // Run Vulkan & NGX initialization on an 8 MiB native worker pthread to avoid JVM -Xss1m stack faults.
    const bool vkNgxOk = ngxWorker().run([&]() -> bool {

    // Query NGX feature instance extensions (only stage cfg->ngxLibDir; avoid external CUDA/VFX directories)
    NVSDK_NGX_FeatureCommonInfo commonInfo{};
    searchWideStrings.clear();
    searchWidePtrs.clear();
    if (cfg && cfg->ngxLibDir && *cfg->ngxLibDir) {
        searchWideStrings.push_back(utf8ToWide(cfg->ngxLibDir));
    }
    for (const auto& ws : searchWideStrings) {
        searchWidePtrs.push_back(ws.c_str());
    }
    if (!searchWidePtrs.empty()) {
        commonInfo.PathListInfo.Path = searchWidePtrs.data();
        commonInfo.PathListInfo.Length = static_cast<unsigned int>(searchWidePtrs.size());
    }
    commonInfo.LoggingInfo.LoggingCallback = ngxLogCallback;
    commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    commonInfo.LoggingInfo.DisableOtherLoggingSinks = false;

    // Enumerate available Vulkan instance extensions
    uint32_t availInstExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availInstExtCount, nullptr);
    std::vector<VkExtensionProperties> availInstExts(availInstExtCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availInstExtCount, availInstExts.data());

    std::vector<const char*> instExts;
    std::set<std::string> instSeen;

    uint32_t ngxInstExtCount = 0;
    VkExtensionProperties* ngxInstExts = nullptr;
    NVSDK_NGX_FeatureDiscoveryInfo disc{};
    disc.SDKVersion = NVSDK_NGX_Version_API;
    disc.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    disc.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    disc.Identifier.v.ProjectDesc.ProjectId = kProjectGuid;
    disc.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    disc.Identifier.v.ProjectDesc.EngineVersion = "26.2";
    appDataWidePath = utf8ToWide((cfg && cfg->logDir && *cfg->logDir) ? cfg->logDir : "/tmp");
    disc.ApplicationDataPath = appDataWidePath.c_str();
    disc.FeatureInfo = &commonInfo;

    auto collectNgxInstExts = [&](NVSDK_NGX_Feature featureId, const char* tag) {
        disc.FeatureID = featureId;
        ngxInstExtCount = 0;
        ngxInstExts = nullptr;
        NVSDK_NGX_Result res = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&disc, &ngxInstExtCount, &ngxInstExts);
        logNative("vkNgx.init: GetFeatureInstExt(%s) res=0x%08X count=%u", tag, static_cast<unsigned>(res), ngxInstExtCount);
        if (res == NVSDK_NGX_Result_Success && ngxInstExts) {
            for (uint32_t i = 0; i < ngxInstExtCount; ++i) {
                if (hasExtension(availInstExts, ngxInstExts[i].extensionName)) {
                    appendUniqueExt(instExts, instSeen, ngxInstExts[i].extensionName);
                }
            }
        }
    };
    collectNgxInstExts(NVSDK_NGX_Feature_SuperSampling, "SR");
    collectNgxInstExts(NVSDK_NGX_Feature_RayReconstruction, "RR");
    collectNgxInstExts(NVSDK_NGX_Feature_FrameGeneration, "FG");

    const char* coreInstExts[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
    };
    for (const char* e : coreInstExts) {
        if (hasExtension(availInstExts, e)) {
            appendUniqueExt(instExts, instSeen, e);
        }
    }

    hasSurfaceExts = hasExtension(availInstExts, VK_KHR_SURFACE_EXTENSION_NAME);
    if (hasSurfaceExts) {
        appendUniqueExt(instExts, instSeen, VK_KHR_SURFACE_EXTENSION_NAME);
        if (hasExtension(availInstExts, "VK_KHR_xlib_surface")) {
            appendUniqueExt(instExts, instSeen, "VK_KHR_xlib_surface");
        }
        if (hasExtension(availInstExts, "VK_KHR_wayland_surface")) {
            appendUniqueExt(instExts, instSeen, "VK_KHR_wayland_surface");
        }
        hasSwapchainColorspace = hasExtension(availInstExts, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        if (hasSwapchainColorspace) {
            appendUniqueExt(instExts, instSeen, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "LinRTX-MC";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Minecraft 26.2";
    appInfo.engineVersion = VK_MAKE_VERSION(26, 2, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = static_cast<uint32_t>(instExts.size());
    ici.ppEnabledExtensionNames = instExts.data();

    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    if (cfg && cfg->enableValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availLayers.data());
        for (const auto& lp : availLayers) {
            if (std::strcmp(lp.layerName, layers[0]) == 0) {
                ici.enabledLayerCount = 1;
                ici.ppEnabledLayerNames = layers;
                break;
            }
        }
    }

    logNative("vkNgx.init: calling vkCreateInstance with %zu exts", instExts.size());
    VkResult vkRes = vkCreateInstance(&ici, nullptr, &instance);
    logNative("vkNgx.init: vkCreateInstance vkRes=%d", static_cast<int>(vkRes));
    if (vkRes != VK_SUCCESS) {
        lastErrorCode = static_cast<int32_t>(vkRes);
        lastError = "vkCreateInstance failed (" + std::to_string(vkRes) + ")";
        return false;
    }

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(instance, &pdCount, nullptr);
    if (pdCount == 0) {
        lastErrorCode = -1;
        lastError = "No Vulkan physical devices found";
        return false;
    }
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(instance, &pdCount, pds.data());

    // Select NVIDIA GPU matching OpenGL UUID (or discrete NVIDIA GPU fallback)
    int bestScore = -1;
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceIDProperties idProps{};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &idProps;
        vkGetPhysicalDeviceProperties2(pd, &props2);

        int score = 0;
        if (hasGlUuid && std::memcmp(idProps.deviceUUID, glUuid.data(), 16) == 0) {
            score += 10000;
        }
        if (props2.properties.vendorID == 0x10DE) {
            score += 1000;
        }
        if (props2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 100;
        }
        if (score > bestScore) {
            bestScore = score;
            physicalDevice = pd;
            gpuName = props2.properties.deviceName;
            uint32_t drv = props2.properties.driverVersion;
            // NVIDIA driver version encoding: major (10b), minor (8b), sub (8b), patch (6b)
            uint32_t maj = (drv >> 22) & 0x3FF;
            uint32_t min = (drv >> 14) & 0xFF;
            uint32_t sub = (drv >> 6) & 0xFF;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%u.%02u.%02u", maj, min, sub);
            driverVersion = buf;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        logNative("vkNgx.init: physicalDevice selection failed (pdCount=%u)", pdCount);
        lastError = "Failed to select Vulkan physical device";
        return false;
    }
    logNative("vkNgx.init: selected GPU='%s' driver=%s score=%d", gpuName.c_str(), driverVersion.c_str(), bestScore);

    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    // Find graphics + compute + transfer queue
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qfCount, qfs.data());
    graphicsQueueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i) {
        if ((qfs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            graphicsQueueFamily = i;
            break;
        }
    }
    if (graphicsQueueFamily == UINT32_MAX) {
        lastError = "No graphics+compute Vulkan queue family";
        return false;
    }

    uint32_t availDevExtCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availDevExtCount, nullptr);
    std::vector<VkExtensionProperties> availDevExts(availDevExtCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availDevExtCount, availDevExts.data());

    std::vector<const char*> devExts;
    std::set<std::string> devSeen;

    void* vkNoload = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_NOLOAD);
    void* ngxNoload = dlopen("libnvidia-ngx.so.1", RTLD_LAZY | RTLD_NOLOAD);
    logNative("vkNgx.init: pre-probe noload libvulkan=%p libnvidia-ngx=%p", vkNoload, ngxNoload);
    if (vkNoload) dlclose(vkNoload);
    if (ngxNoload) {
        dlclose(ngxNoload);
    } else {
        dlerror();
        void* ngxPre = dlopen("libnvidia-ngx.so.1", RTLD_NOW);
        const char* dlErr = dlerror();
        void* symDevExt = ngxPre ? dlsym(ngxPre, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements") : nullptr;
        void* symInitExt = ngxPre ? dlsym(ngxPre, "NVSDK_NGX_VULKAN_Init_Ext") : nullptr;
        logNative("vkNgx.init: pre-dlopen(libnvidia-ngx.so.1, RTLD_NOW)=%p err='%s' symDevExt=%p symInitExt=%p",
                  ngxPre, dlErr ? dlErr : "", symDevExt, symInitExt);
    }

    auto collectNgxDevExts = [&](NVSDK_NGX_Feature featureId, const char* tag) {
        disc.FeatureID = featureId;
        uint32_t ngxDevExtCount = 0;
        VkExtensionProperties* ngxDevExts = nullptr;
        logNative("vkNgx.init: calling GetFeatureDeviceExtensionRequirements(%s)", tag);
        NVSDK_NGX_Result res = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physicalDevice, &disc, &ngxDevExtCount, &ngxDevExts);
        logNative("vkNgx.init: GetFeatureDevExt(%s) res=0x%08X count=%u", tag, static_cast<unsigned>(res), ngxDevExtCount);
        if (res == NVSDK_NGX_Result_Success && ngxDevExts) {
            for (uint32_t i = 0; i < ngxDevExtCount; ++i) {
                logNative("  NGX %s devExt: %s", tag, ngxDevExts[i].extensionName);
                if (hasExtension(availDevExts, ngxDevExts[i].extensionName)) {
                    appendUniqueExt(devExts, devSeen, ngxDevExts[i].extensionName);
                }
            }
        }
    };
    collectNgxDevExts(NVSDK_NGX_Feature_SuperSampling, "SR");
    collectNgxDevExts(NVSDK_NGX_Feature_RayReconstruction, "RR");
    collectNgxDevExts(NVSDK_NGX_Feature_FrameGeneration, "FG");

    // Vulkan forbids enabling both VK_KHR_buffer_device_address and VK_EXT_buffer_device_address simultaneously.
    if (devSeen.count(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) &&
        devSeen.count(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
        std::erase_if(devExts, [](const char* s) {
            return std::strcmp(s, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0;
        });
        logNative("vkNgx.init: filtered conflicting %s (keeping %s)",
                  VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                  VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }

    const char* desiredDevExts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        "VK_NVX_binary_import",
        "VK_NVX_image_view_handle",
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    for (const char* e : desiredDevExts) {
        if (hasExtension(availDevExts, e)) {
            appendUniqueExt(devExts, devSeen, e);
        }
    }
    hasHdrMetadata = hasExtension(availDevExts, VK_EXT_HDR_METADATA_EXTENSION_NAME);
    if (hasHdrMetadata) {
        appendUniqueExt(devExts, devSeen, VK_EXT_HDR_METADATA_EXTENSION_NAME);
    }

    VkPhysicalDeviceVulkan12Features vk12{};
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 feats2{};
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats2.pNext = &vk12;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &feats2);

    // Enable safe device features needed by NGX and compute shaders
    VkPhysicalDeviceVulkan12Features enableVk12{};
    enableVk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enableVk12.bufferDeviceAddress = vk12.bufferDeviceAddress;
    enableVk12.timelineSemaphore = vk12.timelineSemaphore;
    enableVk12.descriptorIndexing = vk12.descriptorIndexing;

    VkPhysicalDeviceFeatures2 enableFeats{};
    enableFeats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enableFeats.pNext = &enableVk12;
    enableFeats.features.shaderStorageImageWriteWithoutFormat = feats2.features.shaderStorageImageWriteWithoutFormat;
    enableFeats.features.shaderStorageImageExtendedFormats = feats2.features.shaderStorageImageExtendedFormats;
    enableFeats.features.shaderInt16 = feats2.features.shaderInt16;

    float qPriority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphicsQueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &qPriority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enableFeats;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();

    logNative("vkNgx.init: calling vkCreateDevice with %zu exts (bda=%u timeline=%u descIdx=%u)",
              devExts.size(),
              enableVk12.bufferDeviceAddress,
              enableVk12.timelineSemaphore,
              enableVk12.descriptorIndexing);
    vkRes = vkCreateDevice(physicalDevice, &dci, nullptr, &device);
    logNative("vkNgx.init: vkCreateDevice vkRes=%d", static_cast<int>(vkRes));
    if (vkRes != VK_SUCCESS) {
        lastErrorCode = static_cast<int32_t>(vkRes);
        lastError = "vkCreateDevice failed (" + std::to_string(vkRes) + ")";
        return false;
    }

    vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
    pfnGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
    pfnGetSemaphoreFdKHR = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
    pfnSetHdrMetadataEXT = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(vkGetDeviceProcAddr(device, "vkSetHdrMetadataEXT"));

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = graphicsQueueFamily;
    if (vkCreateCommandPool(device, &cpci, nullptr, &cmdPool) != VK_SUCCESS) {
        lastError = "vkCreateCommandPool failed";
        return false;
    }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &cbai, &cmdBuf) != VK_SUCCESS) {
        lastError = "vkAllocateCommandBuffers failed";
        return false;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device, &fci, nullptr, &frameFence) != VK_SUCCESS) {
        lastError = "vkCreateFence failed";
        return false;
    }

    if (glInteropAvailable && pfnGetSemaphoreFdKHR) {
        auto makeExportSemVk = [&](SharedSemaphore& out, int& outFd) -> bool {
            VkExportSemaphoreCreateInfo esci{};
            esci.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
            esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            VkSemaphoreCreateInfo sci{};
            sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            sci.pNext = &esci;
            if (vkCreateSemaphore(device, &sci, nullptr, &out.vkSem) != VK_SUCCESS) return false;
            VkSemaphoreGetFdInfoKHR fgfi{};
            fgfi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
            fgfi.semaphore = out.vkSem;
            fgfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            int fd = -1;
            if (pfnGetSemaphoreFdKHR(device, &fgfi, &fd) != VK_SUCCESS || fd < 0) return false;
            outFd = fd;
            return true;
        };
        const bool s1 = makeExportSemVk(glToVkSem, glToVkFd);
        const bool s2 = makeExportSemVk(vkToGlSem, vkToGlFd);
        logNative("vkNgx.init: exportVkSem glToVk=%d vkToGl=%d", s1 ? 1 : 0, s2 ? 1 : 0);
    }

    vkReady = true;

    // Initialize NGX SDK
    logNative("vkNgx.init: calling NVSDK_NGX_VULKAN_Init_with_ProjectID");
    NVSDK_NGX_Result ngxRes = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectGuid,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "26.2",
        appDataWidePath.c_str(),
        instance,
        physicalDevice,
        device,
        vkGetInstanceProcAddr,
        vkGetDeviceProcAddr,
        &commonInfo,
        NVSDK_NGX_Version_API);
    logNative("vkNgx.init: NVSDK_NGX_VULKAN_Init_with_ProjectID res=0x%08X", static_cast<unsigned>(ngxRes));

    if (NVSDK_NGX_FAILED(ngxRes)) {
        uint64_t appId = (cfg && cfg->appId != 0) ? cfg->appId : 0x4C494E5254580001ULL;
        logNative("vkNgx.init: fallback NVSDK_NGX_VULKAN_Init appId=0x%llx", static_cast<unsigned long long>(appId));
        ngxRes = NVSDK_NGX_VULKAN_Init(
            appId,
            appDataWidePath.c_str(),
            instance,
            physicalDevice,
            device,
            vkGetInstanceProcAddr,
            vkGetDeviceProcAddr,
            &commonInfo,
            NVSDK_NGX_Version_API);
        logNative("vkNgx.init: NVSDK_NGX_VULKAN_Init res=0x%08X", static_cast<unsigned>(ngxRes));
    }

    if (NVSDK_NGX_FAILED(ngxRes)) {
        lastErrorCode = static_cast<int32_t>(ngxRes);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "NVSDK_NGX_VULKAN_Init failed: 0x%08X", static_cast<unsigned>(ngxRes));
        lastError = buf;
        ngxReady = false;
        return true; // Vulkan compute shaders & presenter can still run!
    }

    ngxReady = true;

    ownsNgxParams = false;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&ngxParams)) && ngxParams) {
        ownsNgxParams = true;
    } else if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_AllocateParameters(&ngxParams)) && ngxParams) {
        ownsNgxParams = true;
    }

    if (ngxParams) {
        int availDlss = 0;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(ngxParams, NVSDK_NGX_Parameter_SuperSampling_Available, &availDlss))) {
            dlssSupported = (availDlss != 0);
        }
        int availDlssd = 0;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(ngxParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &availDlssd))) {
            dlssdSupported = (availDlssd != 0);
        }
        int availDlssg = 0;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(ngxParams, NVSDK_NGX_Parameter_FrameGeneration_Available, &availDlssg))) {
            dlssgSupported = (availDlssg != 0);
        }
        unsigned int maxMulti = 1;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(ngxParams, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &maxMulti))) {
            maxFgMultiplier = static_cast<int32_t>(maxMulti + 1); // MultiFrameCount = generated frames, total = 1 + generated
        } else if (dlssgSupported) {
            maxFgMultiplier = 2;
        }
    }

    logNative("vkNgx.init: capabilities dlss=%d dlssd=%d dlssg=%d maxFg=%d",
              dlssSupported ? 1 : 0, dlssdSupported ? 1 : 0, dlssgSupported ? 1 : 0, maxFgMultiplier);
    return true;
    }); // end ngxWorker().run

    if (!vkNgxOk) {
        if (glToVkFd >= 0) close(glToVkFd);
        if (vkToGlFd >= 0) close(vkToGlFd);
        shutdown();
        return false;
    }

    if (glInteropAvailable) {
        auto importSemGl = [&](SharedSemaphore& out, int& fd) -> bool {
            if (fd < 0) return false;
            gl.pGenSemaphoresEXT(1, &out.glSem);
            if (out.glSem == 0) {
                close(fd);
                fd = -1;
                return false;
            }
            gl.pImportSemaphoreFdEXT(out.glSem, kGlHandleTypeOpaqueFdExt, fd);
            fd = -1;
            return out.glSem != 0;
        };
        const bool s1 = importSemGl(glToVkSem, glToVkFd);
        const bool s2 = importSemGl(vkToGlSem, vkToGlFd);
        logNative("vkNgx.init: importExportSem glToVk=%d vkToGl=%d", s1 ? 1 : 0, s2 ? 1 : 0);
    } else {
        if (glToVkFd >= 0) close(glToVkFd);
        if (vkToGlFd >= 0) close(vkToGlFd);
    }
    return true;
}

void VkNgxContext::shutdown() {
    auto destroyGlSem = [&](SharedSemaphore& s) {
        if (s.glSem && gl.pDeleteSemaphoresEXT) {
            gl.pDeleteSemaphoresEXT(1, &s.glSem);
            s.glSem = 0;
        }
    };
    destroyGlSem(glToVkSem);
    destroyGlSem(vkToGlSem);

    if (device != VK_NULL_HANDLE || instance != VK_NULL_HANDLE || ngxReady || ngxParams) {
        ngxWorker().run([&]() {
            if (device != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(device);
            }
            if (ngxParams) {
                if (ownsNgxParams) {
                    NVSDK_NGX_VULKAN_DestroyParameters(ngxParams);
                }
                ngxParams = nullptr;
                ownsNgxParams = false;
            }
            if (ngxReady) {
                NVSDK_NGX_VULKAN_Shutdown1(device);
                ngxReady = false;
            }
            if (glToVkSem.vkSem != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, glToVkSem.vkSem, nullptr);
                glToVkSem.vkSem = VK_NULL_HANDLE;
            }
            if (vkToGlSem.vkSem != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, vkToGlSem.vkSem, nullptr);
                vkToGlSem.vkSem = VK_NULL_HANDLE;
            }
            if (frameFence != VK_NULL_HANDLE) {
                vkDestroyFence(device, frameFence, nullptr);
                frameFence = VK_NULL_HANDLE;
            }
            if (cmdPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, cmdPool, nullptr);
                cmdPool = VK_NULL_HANDLE;
                cmdBuf = VK_NULL_HANDLE;
            }
            if (device != VK_NULL_HANDLE) {
                vkDestroyDevice(device, nullptr);
                device = VK_NULL_HANDLE;
            }
            if (instance != VK_NULL_HANDLE) {
                vkDestroyInstance(instance, nullptr);
                instance = VK_NULL_HANDLE;
            }
        });
    }
    vkReady = false;
    dlssSupported = false;
    dlssdSupported = false;
    dlssgSupported = false;
}

bool VkNgxContext::createSharedTexture(SharedTexture& tex,
                                       uint32_t w,
                                       uint32_t h,
                                       VkFormat vkFmt,
                                       VkImageUsageFlags usage,
                                       GLenum glInternalFmt,
                                       bool exportToGl) {
    destroySharedTexture(tex);
    if (!vkReady || w == 0 || h == 0) return false;

    VkExternalMemoryImageCreateInfo emici{};
    emici.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emici.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = exportToGl ? &emici : nullptr;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vkFmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &ici, nullptr, &tex.image) != VK_SUCCESS) {
        destroySharedTexture(tex);
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, tex.image, &req);
    uint32_t memType = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memType == UINT32_MAX) {
        destroySharedTexture(tex);
        return false;
    }

    VkMemoryDedicatedAllocateInfo mdai{};
    mdai.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    mdai.image = tex.image;

    VkExportMemoryAllocateInfo emai{};
    emai.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    emai.pNext = &mdai;
    emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = exportToGl ? static_cast<void*>(&emai) : static_cast<void*>(&mdai);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType;

    if (vkAllocateMemory(device, &mai, nullptr, &tex.memory) != VK_SUCCESS) {
        destroySharedTexture(tex);
        return false;
    }

    if (vkBindImageMemory(device, tex.image, tex.memory, 0) != VK_SUCCESS) {
        destroySharedTexture(tex);
        return false;
    }

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = tex.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = vkFmt;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.baseMipLevel = 0;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &ivci, nullptr, &tex.view) != VK_SUCCESS) {
        destroySharedTexture(tex);
        return false;
    }

    tex.width = w;
    tex.height = h;
    tex.vkFormat = vkFmt;
    tex.glFormat = glInternalFmt;
    tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (exportToGl && gl.hasExtMemoryObjectFd && pfnGetMemoryFdKHR && glInternalFmt != 0) {
        VkMemoryGetFdInfoKHR mgfi{};
        mgfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        mgfi.memory = tex.memory;
        mgfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (pfnGetMemoryFdKHR(device, &mgfi, &fd) != VK_SUCCESS || fd < 0) {
            destroySharedTexture(tex);
            return false;
        }
        gl.pCreateMemoryObjectsEXT(1, &tex.glMemObj);
        GLint dedicated = 1;
        gl.pMemoryObjectParameterivEXT(tex.glMemObj, kGlDedicatedMemoryObjectExt, &dedicated);
        gl.pImportMemoryFdEXT(tex.glMemObj, req.size, kGlHandleTypeOpaqueFdExt, fd);
        // fd ownership transferred to OpenGL per GL_EXT_memory_object_fd
        gl.pGenTextures(1, &tex.glTex);
        gl.pBindTexture(kGlTexture2D, tex.glTex);
        gl.pTexParameteri(kGlTexture2D, 0x2801 /*GL_TEXTURE_MIN_FILTER*/, 0x2601 /*GL_LINEAR*/);
        gl.pTexParameteri(kGlTexture2D, 0x2800 /*GL_TEXTURE_MAG_FILTER*/, 0x2601 /*GL_LINEAR*/);
        gl.pTexParameteri(kGlTexture2D, 0x2802 /*GL_TEXTURE_WRAP_S*/, 0x812F /*GL_CLAMP_TO_EDGE*/);
        gl.pTexParameteri(kGlTexture2D, 0x2803 /*GL_TEXTURE_WRAP_T*/, 0x812F /*GL_CLAMP_TO_EDGE*/);
        gl.pTextureStorageMem2DEXT(tex.glTex, 1, glInternalFmt,
                                   static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                                   tex.glMemObj, 0);
        gl.pBindTexture(kGlTexture2D, 0);
    }

    return true;
}

void VkNgxContext::destroySharedTexture(SharedTexture& tex) {
    if (tex.glTex && gl.pDeleteTextures) {
        gl.pDeleteTextures(1, &tex.glTex);
        tex.glTex = 0;
    }
    if (tex.glMemObj && gl.pDeleteMemoryObjectsEXT) {
        gl.pDeleteMemoryObjectsEXT(1, &tex.glMemObj);
        tex.glMemObj = 0;
    }
    if (device != VK_NULL_HANDLE) {
        if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(device, tex.view, nullptr);
        if (tex.image != VK_NULL_HANDLE) vkDestroyImage(device, tex.image, nullptr);
        if (tex.memory != VK_NULL_HANDLE) vkFreeMemory(device, tex.memory, nullptr);
    }
    tex.view = VK_NULL_HANDLE;
    tex.image = VK_NULL_HANDLE;
    tex.memory = VK_NULL_HANDLE;
    tex.width = 0;
    tex.height = 0;
    tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VkNgxContext::cmdBarrier(VkCommandBuffer cmd,
                              SharedTexture& tex,
                              VkImageLayout newLayout,
                              VkAccessFlags srcAccess,
                              VkAccessFlags dstAccess,
                              VkPipelineStageFlags srcStage,
                              VkPipelineStageFlags dstStage) {
    if (tex.image == VK_NULL_HANDLE) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = tex.layout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex.image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    tex.layout = newLayout;
}

bool VkNgxContext::copyGlToShared(GLuint srcGlTex,
                                  const SharedTexture& dstShared,
                                  uint32_t w,
                                  uint32_t h) {
    if (!srcGlTex || !dstShared.glTex || !gl.hasCopyImage) return false;
    gl.pCopyImageSubData(srcGlTex, kGlTexture2D, 0, 0, 0, 0,
                         dstShared.glTex, kGlTexture2D, 0, 0, 0, 0,
                         static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1);
    return true;
}

bool VkNgxContext::copySharedToGl(const SharedTexture& srcShared,
                                  GLuint dstGlTex,
                                  uint32_t w,
                                  uint32_t h) {
    if (!srcShared.glTex || !dstGlTex || !gl.hasCopyImage) return false;
    gl.pCopyImageSubData(srcShared.glTex, kGlTexture2D, 0, 0, 0, 0,
                         dstGlTex, kGlTexture2D, 0, 0, 0, 0,
                         static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1);
    return true;
}

void VkNgxContext::waitGpuIdleFence() {
    if (vkReady && frameFence != VK_NULL_HANDLE) {
        vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX);
    }
}

bool VkNgxContext::signalGlToVkSemaphore(SharedTexture* const* textures, uint32_t count) {
    waitingOnGl = false;
    if (!vkReady || !glToVkSem.glSem || glToVkSem.vkSem == VK_NULL_HANDLE || !gl.pSignalSemaphoreEXT) {
        return false;
    }

    std::array<GLuint, 16> glTexIds{};
    std::array<GLenum, 16> dstLayouts{};
    uint32_t texCount = 0;
    if (textures) {
        for (uint32_t i = 0; i < count && texCount < glTexIds.size(); ++i) {
            SharedTexture* tex = textures[i];
            if (!tex || tex->glTex == 0) continue;
            bool dup = false;
            for (uint32_t j = 0; j < texCount; ++j) {
                if (glTexIds[j] == tex->glTex) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            glTexIds[texCount] = tex->glTex;
            dstLayouts[texCount] = kGlLayoutTransferDstExt;
            tex->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ++texCount;
        }
    }

    gl.pSignalSemaphoreEXT(
        glToVkSem.glSem,
        0,
        nullptr,
        texCount,
        texCount > 0 ? glTexIds.data() : nullptr,
        texCount > 0 ? dstLayouts.data() : nullptr);
    if (gl.pFlush) {
        gl.pFlush();
    }
    waitingOnGl = true;
    return true;
}

void VkNgxContext::waitVkToGlSemaphore(const SharedTexture* const* textures, uint32_t count) {
    if (!vkReady || !vkToGlSem.glSem || !gl.pWaitSemaphoreEXT) {
        return;
    }

    std::array<GLuint, 16> glTexIds{};
    std::array<GLenum, 16> srcLayouts{};
    uint32_t texCount = 0;
    if (textures) {
        for (uint32_t i = 0; i < count && texCount < glTexIds.size(); ++i) {
            const SharedTexture* tex = textures[i];
            if (!tex || tex->glTex == 0) continue;
            bool dup = false;
            for (uint32_t j = 0; j < texCount; ++j) {
                if (glTexIds[j] == tex->glTex) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            glTexIds[texCount] = tex->glTex;
            srcLayouts[texCount] = kGlLayoutTransferSrcExt;
            ++texCount;
        }
    }

    gl.pWaitSemaphoreEXT(
        vkToGlSem.glSem,
        0,
        nullptr,
        texCount,
        texCount > 0 ? glTexIds.data() : nullptr,
        texCount > 0 ? srcLayouts.data() : nullptr);
}

bool VkNgxContext::beginCommands(bool waitGlSemaphore) {
    if (!vkReady) return false;
    vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &frameFence);
    vkResetCommandBuffer(cmdBuf, 0);

    waitingOnGl = waitGlSemaphore && (glToVkSem.glSem != 0) && (glToVkSem.vkSem != VK_NULL_HANDLE);

    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(cmdBuf, &cbi) == VK_SUCCESS;
}

bool VkNgxContext::endAndSubmitCommands(bool signalGlSemaphore, bool waitCpuFence, bool* outSignaledGl) {
    if (outSignaledGl) {
        *outSignaledGl = false;
    }
    if (vkEndCommandBuffer(cmdBuf) != VK_SUCCESS) {
        waitingOnGl = false;
        return false;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBuf;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore waitSems[1] = {glToVkSem.vkSem};
    VkSemaphore sigSems[1] = {vkToGlSem.vkSem};

    if (waitingOnGl && glToVkSem.vkSem != VK_NULL_HANDLE) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = waitSems;
        si.pWaitDstStageMask = &waitStage;
    }
    const bool doSignalGl = signalGlSemaphore && vkToGlSem.vkSem != VK_NULL_HANDLE && vkToGlSem.glSem && gl.pWaitSemaphoreEXT;
    if (doSignalGl) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = sigSems;
    }

    waitingOnGl = false;
    if (vkQueueSubmit(graphicsQueue, 1, &si, frameFence) != VK_SUCCESS) {
        return false;
    }

    if (outSignaledGl) {
        *outSignaledGl = doSignalGl;
    }

    if (waitCpuFence) {
        vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX);
    }
    return true;
}

} // namespace linrtx
