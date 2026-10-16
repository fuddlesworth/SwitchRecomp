/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan instance, device, queues and swapchain. Vulkan 1.3 is required: timeline semaphores (1.2),
   synchronization2 and dynamic rendering (1.3) are what the renderer is built on. */
#include <stdlib.h>
#include <string.h>
#include "runtime/options.h"
#include "runtime/platform/backend.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include "gpu/vk/vk.h"

int g_vk_preserve_infnan;
int g_vk_flush_denorm;
VkShaderStageFlags g_vk_subgroup_stages;

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
int vk_trylock(void) {
    if (!t_vkdepth && !pm_trylock(&g_vklock)) return 0;
    ++t_vkdepth;
    return 1;
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
    const char* const* be = g_opt_headless ? NULL : backend_vk_extensions(&nbe);
    const char* ext[16];
    u32 next = 0;
    for (u32 i = 0; i < nbe && next < 15; i++) ext[next++] = be[i];
    if (nbe > 15) WARN("vulkan: the window backend wants %u instance extensions, only 15 are taken\n", nbe);
    const char* layers[1];
    u32 nlayers = 0;
    int validation = getenv("SNX_VK_VALIDATION") != NULL;
    if (validation) {
        if (has_layer("VK_LAYER_KHRONOS_validation")) {
            layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
            ext[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        } else {
            WARN("vulkan: SNX_VK_VALIDATION is set but VK_LAYER_KHRONOS_validation is not installed\n");
            validation = 0;
        }
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "ScarletNX",
                             .pEngineName = "ScarletNX",
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

/* Vulkan 1.3 and a graphics queue. Only windowed runs need WSI and presentation support.
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
    if (!g_opt_headless && !swapchain) return -1;
    u32 nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties* q = calloc(nq ? nq : 1, sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, q);
    *gfx = *compute = ~0u;
    for (u32 i = 0; i < nq; i++) {
        VkBool32 present = g_opt_headless; /* a query that fails is no support */
        if (!g_opt_headless && vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, g_vk.surface, &present) != VK_SUCCESS)
            present = VK_FALSE;
        VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT; /* ASTC uploads use compute */
        if ((q[i].queueFlags & required) == required && present && *gfx == ~0u) *gfx = i;
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
        WARN("vulkan: no Vulkan 1.3 device with a suitable %s queue\n", g_opt_headless ? "graphics" : "presentation");
        return 0;
    }
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    VkPhysicalDeviceSubgroupProperties subgroup = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
                                              .pNext = &subgroup};
    VkPhysicalDeviceProperties2 pp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &pp2);
    g_vk_subgroup_stages =
        subgroup.subgroupSize >= 32 &&
                (subgroup.supportedOperations & (VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                                 VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)) ==
                    (VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                     VK_SUBGROUP_FEATURE_ARITHMETIC_BIT)
            ? subgroup.supportedStages
            : 0;
    g_vk_preserve_infnan = p12.shaderSignedZeroInfNanPreserveFloat32 != 0;
    /* 32 bit alone: a device whose denorm modes can't differ per width would need 16 and 64 bit declared too */
    g_vk_flush_denorm = p12.shaderDenormFlushToZeroFloat32 &&
                        p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
    if (!g_vk_preserve_infnan || !g_vk_flush_denorm)
        WARN("vulkan: some direct shaders require FP32 float controls unavailable on this GPU\n");

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
    WANT(shaderTessellationAndGeometryPointSize);
#undef WANT
    /* points out of tessellation and geometry programs take the size those write, as the vertex program's do */
    g_shader_stage_point_size = want->shaderTessellationAndGeometryPointSize != 0;
    g_shader_geometry = want->geometryShader != 0;
    const char* ext[4];
    u32 next = 0;
    if (!g_opt_headless) ext[next++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
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
    if (g_vk.has_border_color) { /* how many samplers may have a custom border color: as few as 32 */
        VkPhysicalDeviceCustomBorderColorPropertiesEXT cbp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 cp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cbp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &cp2);
        g_vk.border_color_max = cbp.maxCustomBorderColorSamplers;
    }
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .shaderDrawParameters = q11.shaderDrawParameters};
    /* Enable mirror clamp and vertex shader Layer output when the device supports them. */
    g_vk.has_mirror_clamp = q12.samplerMirrorClampToEdge;
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &f11,
                                            .samplerMirrorClampToEdge = q12.samplerMirrorClampToEdge,
                                            .timelineSemaphore = VK_TRUE,
                                            .shaderFloat16 = q12.shaderFloat16,
                                            .shaderOutputViewportIndex = q12.shaderOutputViewportIndex,
                                            .shaderOutputLayer = q12.shaderOutputLayer,
                                            .shaderSampledImageArrayNonUniformIndexing =
                                                q12.shaderSampledImageArrayNonUniformIndexing};
    /* shaders pick an indexed texture per invocation from an array (SNX_VKB_TEXARRAY) */
    g_shader_texture_arrays = q12.shaderSampledImageArrayNonUniformIndexing != 0;
    g_shader_float16 = q12.shaderFloat16 != 0;
    g_shader_output_layer = q12.shaderOutputLayer != 0; /* ShaderLayer: a vertex or tessellation program's layer */
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

/* the swapchain destroyed. Its images' semaphores: a present may still wait on one after the device is idle (the
   presentation engine isn't covered by vkDeviceWaitIdle), so they go a swapchain later, when those presents are long
   done */
static void swapchain_retire(void) {
    static VkSemaphore retired[VK_SC_MAX];
    static u32 nretired;
    for (u32 i = 0; i < nretired; i++) vkDestroySemaphore(g_vk.dev, retired[i], NULL);
    nretired = 0;
    for (u32 i = 0; i < g_vk.sc_count; i++) retired[nretired++] = g_vk.sc_done[i];
    vk_ui_swapchain_gone();
    vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    g_vk.sc_count = 0;
}

/* the window's surface made anew, its swapchain to follow: Android destroys a window's surface while the app is in
   the background, and the Vulkan surface on it is lost for good */
int vk_surface_recreate(void) {
    vkDeviceWaitIdle(g_vk.dev);
    if (g_vk.swapchain) swapchain_retire();
    vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (backend_vk_surface(g_vk.instance, &g_vk.surface)) {
        VkBool32 present = VK_FALSE; /* the queue chosen for the old surface, asked again for the new one */
        if (vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, g_vk.gfx_family, g_vk.surface, &present) == VK_SUCCESS &&
            present)
            return 1;
        WARN_LIMIT(4, "vulkan: the window's new surface can't be presented to from the graphics queue\n");
        return 0;
    }
    WARN_LIMIT(4, "vulkan: the window's surface could not be made again\n");
    return 0;
}

int vk_window_release(void) {
    int waited = 0;
    /* a thread that holds the renderer and doesn't let go (stuck on a lost device) must not keep the error unseen */
    while (!vk_trylock()) {
        if (++waited == 200) return 0;
        plat_sleep_ns(10000000);
    }
    vkDeviceWaitIdle(g_vk.dev); /* a lost device answers at once */
    if (g_vk.swapchain) swapchain_retire();
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    /* every hold of this thread's at once: it won't return to the callers that took them */
    t_vkdepth = 0;
    pm_unlock(&g_vklock);
    return 1;
}

int vk_swapchain_create(int srgb) {
    if (g_opt_headless) return 0; /* no WSI calls, even on a resize/recreate path */
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    if (cr != VK_SUCCESS) { /* asked again on each present while it fails: said a few times */
        WARN_LIMIT(4, "vulkan: the window's surface could not be queried: %s (%d)\n", vk_result_str(cr), (int)cr);
        if (cr == VK_ERROR_SURFACE_LOST_KHR) g_vk.sc_lost = 1;
        return 0;
    }
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
        LOG_LIMIT(1, "vulkan: the window has no sRGB format: frames in sRGB show darker\n");
    }

    /* vsync=1: FIFO, which every device has. Otherwise mailbox, else immediate, else FIFO all the same. Read once: the
       main thread's .ini reload writes it */
    int vsync = __atomic_load_n(&g_opt_vsync, __ATOMIC_RELAXED);
    u32 nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, NULL);
    VkPresentModeKHR* ms = calloc(nm ? nm : 1, sizeof *ms);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, ms);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (u32 i = 0; i < nm && !vsync; i++)
        if (ms[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = ms[i];
    if (mode == VK_PRESENT_MODE_FIFO_KHR && !vsync)
        for (u32 i = 0; i < nm; i++)
            if (ms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = ms[i];
    free(ms);

    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > VK_SC_MAX) count = VK_SC_MAX > caps.minImageCount ? VK_SC_MAX : caps.minImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) /* the lowest one it has */
        alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    /* frames are drawn upright: a display held other than its own way (a phone held wide) has its compositor turn
       them, where its current transform would say they come turned already */
    VkSurfaceTransformFlagBitsKHR transform = caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                  ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                  : caps.currentTransform;
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
                                   .preTransform = transform,
                                   .compositeAlpha = alpha,
                                   .presentMode = mode,
                                   .clipped = VK_TRUE,
                                   .oldSwapchain = old};
    /* old is retired by the create, made or not: on any failure it goes too, and the next present starts anew */
    VkSwapchainKHR sc;
    VkSemaphore done[VK_SC_MAX];
    u32 nimg = 0, ndone = 0;
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &sc);
    if (cr != VK_SUCCESS) { /* tried again on each present while it fails: said a few times */
        WARN_LIMIT(4, "vulkan: the swapchain could not be made: %s (%d)\n", vk_result_str(cr), (int)cr);
        if (cr == VK_ERROR_SURFACE_LOST_KHR) g_vk.sc_lost = 1;
        if (old) swapchain_retire();
        return 0;
    }
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &nimg, NULL);
    if (nimg > VK_SC_MAX) WARN("vulkan: the swapchain has %u images, more than the %d taken\n", nimg, VK_SC_MAX);
    while (nimg <= VK_SC_MAX && ndone < nimg && VK_OK(vkCreateSemaphore(g_vk.dev, &si, NULL, &done[ndone]))) ndone++;
    if (nimg > VK_SC_MAX || ndone < nimg) {
        while (ndone) vkDestroySemaphore(g_vk.dev, done[--ndone], NULL);
        vkDestroySwapchainKHR(g_vk.dev, sc, NULL);
        if (old) swapchain_retire();
        return 0;
    }
    if (old) swapchain_retire();
    g_vk.swapchain = sc;
    g_vk.sc_extent = ext;
    g_vk.sc_win_w = ww;
    g_vk.sc_win_h = wh;
    g_vk.sc_srgb = fmt.format == VK_FORMAT_B8G8R8A8_SRGB || fmt.format == VK_FORMAT_R8G8B8A8_SRGB;
    g_vk.sc_format = fmt.format;
    g_vk.sc_turned = transform != caps.currentTransform;
    g_vk.sc_vsync = vsync;
    g_vk.sc_count = nimg;
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &g_vk.sc_count, g_vk.sc_images);
    memcpy(g_vk.sc_done, done, sizeof done[0] * nimg);
    LOG("vulkan: swapchain %ux%u, %u images%s, %s\n", ext.width, ext.height, g_vk.sc_count,
        g_vk.sc_srgb ? ", sRGB" : "",
        mode == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
        : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                : "fifo");
    return 1;
}

int vk_init(void) {
    pm_init(&g_vklock);
    if (g_opt_headless) {
        /* Load libvulkan/vulkan-1 directly. No SDL video driver or headless-surface extension is needed. */
        if (!VK_OK(volkInitialize())) return 0;
    } else {
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)backend_vk_loader();
        if (!gipa) {
            WARN("vulkan: no Vulkan loader (is a Vulkan driver installed?)\n");
            return 0;
        }
        volkInitializeCustom(gipa);
    }
    if (!create_instance()) return 0;
    if (!g_opt_headless && !backend_vk_surface(g_vk.instance, &g_vk.surface)) {
        WARN("vulkan: the window backend could not create a surface\n");
        return 0;
    }
    if (!create_device()) return 0;
    return g_opt_headless ? 1 : vk_swapchain_create(0);
}
