/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* Vulkan instance, device, queues and swapchain. Vulkan 1.3 is required: timeline semaphores (1.2),
   synchronization2 and dynamic rendering (1.3) are what the renderer is built on. */
#include <stdlib.h>
#include <string.h>
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#include "gpu/vk/vk.h"

VkState g_vk;
/* reentrant on the thread that holds it: landing compute results writes guest memory, which can come back here */
static PMutex g_vklock;
static _Thread_local int t_vkdepth;
void vk_lock(void) {
    if (!t_vkdepth++) pm_lock(&g_vklock);
}
void vk_unlock(void) {
    if (!--t_vkdepth) pm_unlock(&g_vklock);
}

static const char* vk_result_str(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VkResult";
    }
}
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    if (r >= 0) return 1;
    WARN("vulkan: %s failed: %s (%d) at %s:%d\n", what, vk_result_str(r), (int)r, file, line);
    return 0;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT* d, void* user) {
    (void)type;
    (void)user;
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        WARN("vulkan validation: %s\n", d->pMessage);
    else
        LOG("vulkan validation: %s\n", d->pMessage);
    return VK_FALSE;
}

static int has_layer(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties* l = calloc(n ? n : 1, sizeof *l);
    vkEnumerateInstanceLayerProperties(&n, l);
    int found = 0;
    for (u32 i = 0; i < n && !found; i++) found = !strcmp(l[i].layerName, name);
    free(l);
    return found;
}

static int create_instance(void) {
    u32 nbe = 0;
    const char* const* be = backend_vk_extensions(&nbe);
    const char* ext[16];
    u32 next = 0;
    for (u32 i = 0; i < nbe && next < 15; i++) ext[next++] = be[i];
    if (nbe > 15) WARN("vulkan: the window backend wants %u instance extensions, only 15 are taken\n", nbe);
    const char* layers[1];
    u32 nlayers = 0;
    int validation = getenv("SWR_VK_VALIDATION") != NULL;
    if (validation) {
        if (has_layer("VK_LAYER_KHRONOS_validation")) {
            layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
            ext[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        } else {
            WARN("vulkan: SWR_VK_VALIDATION is set but VK_LAYER_KHRONOS_validation is not installed\n");
            validation = 0;
        }
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "SwitchRecomp",
                             .pEngineName = "SwitchRecomp",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app,
                               .enabledLayerCount = nlayers,
                               .ppEnabledLayerNames = layers,
                               .enabledExtensionCount = next,
                               .ppEnabledExtensionNames = ext};
    if (!VK_OK(vkCreateInstance(&ci, NULL, &g_vk.instance))) return 0;
    volkLoadInstance(g_vk.instance);
    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT di = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                                                 .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                                 .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                                                 .pfnUserCallback = debug_cb};
        VK_OK(vkCreateDebugUtilsMessengerEXT(g_vk.instance, &di, NULL, &g_vk.debug));
    }
    return 1;
}

/* a device that has Vulkan 1.3, the swapchain extension and a queue that can draw and present to our surface.
   *gfx_queues: how many queues the graphics family has */
static int device_score(VkPhysicalDevice pd, u32* gfx, u32* compute, u32* gfx_queues) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    if (p.apiVersion < VK_API_VERSION_1_3) return -1;
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties* e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int swapchain = 0;
    for (u32 i = 0; i < n; i++) swapchain |= !strcmp(e[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    free(e);
    if (!swapchain) return -1;
    u32 nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties* q = calloc(nq ? nq : 1, sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, q);
    *gfx = *compute = ~0u;
    for (u32 i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, g_vk.surface, &present);
        if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present && *gfx == ~0u) *gfx = i;
    }
    /* a compute family without graphics runs alongside the graphics queue */
    for (u32 i = 0; i < nq; i++)
        if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && *compute == ~0u)
            *compute = i;
    if (*compute == ~0u) *compute = *gfx;
    *gfx_queues = *gfx != ~0u ? q[*gfx].queueCount : 0;
    free(q);
    if (*gfx == ~0u) return -1;
    return p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
           : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                    : 1;
}

static int create_device(void) {
    u32 n = 0;
    vkEnumeratePhysicalDevices(g_vk.instance, &n, NULL);
    VkPhysicalDevice* pds = calloc(n ? n : 1, sizeof *pds);
    vkEnumeratePhysicalDevices(g_vk.instance, &n, pds);
    int best = -1;
    u32 gfx_queues = 0;
    for (u32 i = 0; i < n; i++) {
        u32 gf, cf, gq;
        int s = device_score(pds[i], &gf, &cf, &gq);
        if (s > best) {
            best = s;
            g_vk.phys = pds[i];
            g_vk.gfx_family = gf;
            g_vk.compute_family = cf;
            gfx_queues = gq;
        }
    }
    free(pds);
    if (best < 0) {
        WARN("vulkan: no device with Vulkan 1.3 that can show our window\n");
        return 0;
    }
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);

    /* compute gets a queue of its own: of a compute-only family, or else a second one of the graphics family */
    int second = g_vk.compute_family == g_vk.gfx_family && gfx_queues >= 2;
    float prio[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci[2] = {{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = g_vk.gfx_family,
                                       .queueCount = second ? 2 : 1,
                                       .pQueuePriorities = prio},
                                      {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = g_vk.compute_family,
                                       .queueCount = 1,
                                       .pQueuePriorities = prio}};
    /* the features drawing uses, where the device has them */
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &have);
    VkPhysicalDeviceFeatures* want = &g_vk.feat;
#define WANT(f) want->f = have.f
    /* constant buffers are bound with the bytes the shader reads, not the 64K it declares: reads past them stay in
       bounds (their values are undefined) */
    WANT(robustBufferAccess);
    WANT(imageCubeArray);
    WANT(independentBlend);
    WANT(dualSrcBlend);
    WANT(logicOp);
    WANT(depthClamp);
    WANT(depthBiasClamp);
    WANT(fillModeNonSolid);
    WANT(depthBounds);
    WANT(wideLines);
    WANT(largePoints);
    WANT(samplerAnisotropy);
    WANT(textureCompressionBC);
    WANT(vertexPipelineStoresAndAtomics);
    WANT(fragmentStoresAndAtomics);
    WANT(shaderImageGatherExtended);
    WANT(shaderStorageImageExtendedFormats);
    WANT(shaderStorageImageWriteWithoutFormat);
    WANT(shaderStorageImageReadWithoutFormat);
    WANT(shaderClipDistance);
    WANT(shaderCullDistance);
    WANT(shaderFloat64);
    WANT(shaderInt64);
    WANT(shaderInt16);
    WANT(geometryShader);
    WANT(tessellationShader);
#undef WANT
    const char* ext[4] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    u32 next = 1;
    {
        u32 n = 0;
        vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &n, NULL);
        VkExtensionProperties* e = calloc(n ? n : 1, sizeof *e);
        vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &n, e);
        for (u32 i = 0; i < n; i++) {
            if (!strcmp(e[i].extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) g_vk.has_divisor = 1;
            if (!strcmp(e[i].extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)) g_vk.has_border_color = 1;
        }
        free(e);
    }
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fdiv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features q12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features q11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .pNext = &q12};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q11};
    void** tail = &q12.pNext;
    if (g_vk.has_divisor) {
        *tail = &fdiv;
        tail = &fdiv.pNext;
    }
    if (g_vk.has_border_color) {
        *tail = &fbc;
        tail = &fbc.pNext;
    }
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
    g_vk.has_divisor = g_vk.has_divisor && fdiv.vertexAttributeInstanceRateDivisor;
    g_vk.has_border_color = g_vk.has_border_color && fbc.customBorderColors && fbc.customBorderColorWithoutFormat;
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .shaderDrawParameters = q11.shaderDrawParameters};
    /* also where the device has them: the mirror-once address mode (texture.c), and gl_Layer from vertex shaders
       (shader.c's GL_ARB_shader_viewport_layer_array) */
    g_vk.has_mirror_clamp = q12.samplerMirrorClampToEdge;
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &f11,
                                            .samplerMirrorClampToEdge = q12.samplerMirrorClampToEdge,
                                            .timelineSemaphore = VK_TRUE,
                                            .shaderOutputViewportIndex = q12.shaderOutputViewportIndex,
                                            .shaderOutputLayer = q12.shaderOutputLayer};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = &f12,
                                            .synchronization2 = VK_TRUE,
                                            .dynamicRendering = VK_TRUE};
    void* chain = &f13;
    if (g_vk.has_divisor) {
        fdiv = (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
            .pNext = chain,
            .vertexAttributeInstanceRateDivisor = VK_TRUE};
        chain = &fdiv;
        ext[next++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    }
    if (g_vk.has_border_color) {
        fbc = (VkPhysicalDeviceCustomBorderColorFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
            .pNext = chain,
            .customBorderColors = VK_TRUE,
            .customBorderColorWithoutFormat = VK_TRUE};
        chain = &fbc;
        ext[next++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    }
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pNext = chain,
                              .queueCreateInfoCount = g_vk.compute_family == g_vk.gfx_family ? 1 : 2,
                              .pQueueCreateInfos = qci,
                              .enabledExtensionCount = next,
                              .ppEnabledExtensionNames = ext,
                              .pEnabledFeatures = &g_vk.feat};
    if (!VK_OK(vkCreateDevice(g_vk.phys, &dci, NULL, &g_vk.dev))) return 0;
    volkLoadDevice(g_vk.dev);
    vkGetDeviceQueue(g_vk.dev, g_vk.gfx_family, 0, &g_vk.gfx);
    vkGetDeviceQueue(g_vk.dev, g_vk.compute_family, second ? 1 : 0, &g_vk.compute);
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
                             .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    VmaAllocatorCreateInfo vai = {.physicalDevice = g_vk.phys,
                                  .device = g_vk.dev,
                                  .instance = g_vk.instance,
                                  .vulkanApiVersion = VK_API_VERSION_1_3,
                                  .pVulkanFunctions = &vf};
    if (!VK_OK(vmaCreateAllocator(&vai, &g_vk.vma))) return 0;
    LOG("vulkan: %s, Vulkan %u.%u.%u, driver %08x, graphics queue family %u, compute %u%s\n", g_vk.props.deviceName,
        VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion),
        VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.props.driverVersion, g_vk.gfx_family, g_vk.compute_family,
        g_vk.compute_family != g_vk.gfx_family ? " (separate family)"
        : second                               ? " (a second queue)"
                                               : " (the graphics queue)");
    return 1;
}

int vk_swapchain_create(int srgb) {
    VkSurfaceCapabilitiesKHR caps;
    if (!VK_OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps))) return 0;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        WARN("vulkan: the window's surface can't be drawn to by copies, which presenting needs\n");
        return 0;
    }
    int ww, wh; /* what presents compare against, whatever the surface makes of it */
    backend_window_size(&ww, &wh);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { /* Wayland: the swapchain decides, so the window's size */
        ext.width = ww > 0 ? (u32)ww : 0;
        ext.height = wh > 0 ? (u32)wh : 0;
    }
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
    if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (!ext.width || !ext.height) return 0;

    u32 nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR* fs = calloc(nf ? nf : 1, sizeof *fs);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fs);
    /* 8-bit, in the encoding of the frames presented: a blit from an sRGB target to a UNORM image decodes the bytes,
       where the console's display shows them as they are */
    VkSurfaceFormatKHR fmt = fs[0];
    int found = 0;
    for (int pass = srgb ? 0 : 1; pass < 2 && !found; pass++)
        for (u32 i = 0; i < nf && !found; i++) {
            VkFormat a = pass ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_B8G8R8A8_SRGB,
                     b = pass ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
            if ((fs[i].format == a || fs[i].format == b) && fs[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                fmt = fs[i];
                found = 1;
            }
        }
    free(fs);
    if (srgb && fmt.format != VK_FORMAT_B8G8R8A8_SRGB && fmt.format != VK_FORMAT_R8G8B8A8_SRGB) {
        static int said;
        if (!said) {
            said = 1;
            LOG("vulkan: the window has no sRGB format: frames in sRGB show darker\n");
        }
    }

    /* no vsync: the runtime paces frames itself (fps_limit) */
    u32 nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, NULL);
    VkPresentModeKHR* ms = calloc(nm ? nm : 1, sizeof *ms);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, ms);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (u32 i = 0; i < nm; i++)
        if (ms[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = ms[i];
    if (mode == VK_PRESENT_MODE_FIFO_KHR)
        for (u32 i = 0; i < nm; i++)
            if (ms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = ms[i];
    free(ms);

    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > VK_SC_MAX) count = VK_SC_MAX > caps.minImageCount ? VK_SC_MAX : caps.minImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) /* the lowest one it has */
        alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    VkSwapchainKHR old = g_vk.swapchain;
    if (old) vkDeviceWaitIdle(g_vk.dev);
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .surface = g_vk.surface,
                                   .minImageCount = count,
                                   .imageFormat = fmt.format,
                                   .imageColorSpace = fmt.colorSpace,
                                   .imageExtent = ext,
                                   .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform = caps.currentTransform,
                                   .compositeAlpha = alpha,
                                   .presentMode = mode,
                                   .clipped = VK_TRUE,
                                   .oldSwapchain = old};
    VkSwapchainKHR sc;
    if (!VK_OK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &sc))) return 0;
    u32 nimg = 0;
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &nimg, NULL);
    if (nimg > VK_SC_MAX) {
        WARN("vulkan: the swapchain has %u images, more than the %d taken\n", nimg, VK_SC_MAX);
        vkDestroySwapchainKHR(g_vk.dev, sc, NULL);
        return 0;
    }
    /* the old images' semaphores: a present may still wait on one after the device is idle (the presentation engine
       isn't covered by vkDeviceWaitIdle), so they go a swapchain later, when those presents are long done */
    static VkSemaphore retired[VK_SC_MAX];
    static u32 nretired;
    for (u32 i = 0; i < nretired; i++) vkDestroySemaphore(g_vk.dev, retired[i], NULL);
    nretired = 0;
    if (old) {
        for (u32 i = 0; i < g_vk.sc_count; i++) retired[nretired++] = g_vk.sc_done[i];
        vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    }
    g_vk.swapchain = sc;
    g_vk.sc_extent = ext;
    g_vk.sc_win_w = ww;
    g_vk.sc_win_h = wh;
    g_vk.sc_srgb = fmt.format == VK_FORMAT_B8G8R8A8_SRGB || fmt.format == VK_FORMAT_R8G8B8A8_SRGB;
    g_vk.sc_count = nimg;
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &g_vk.sc_count, g_vk.sc_images);
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (u32 i = 0; i < g_vk.sc_count; i++) vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.sc_done[i]);
    LOG("vulkan: swapchain %ux%u, %u images%s, %s\n", ext.width, ext.height, g_vk.sc_count,
        g_vk.sc_srgb ? ", sRGB" : "",
        mode == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
        : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                : "fifo");
    return 1;
}

int vk_init(void) {
    pm_init(&g_vklock);
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)backend_vk_loader();
    if (!gipa) {
        WARN("vulkan: no Vulkan loader (is a Vulkan driver installed?)\n");
        return 0;
    }
    volkInitializeCustom(gipa);
    if (!create_instance()) return 0;
    if (!backend_vk_surface(g_vk.instance, &g_vk.surface)) {
        WARN("vulkan: the window backend could not create a surface\n");
        return 0;
    }
    if (!create_device()) return 0;
    return vk_swapchain_create(0);
}
