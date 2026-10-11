/* Vulkan renderer: shared device state. Functions are loaded through volk from the loader the window backend gives
   (SDL's), so nothing links against a Vulkan library. */
#ifndef SWR_VK_H
#define SWR_VK_H
#include "rt.h"
#include "volk.h"

#define VK_FRAMES 2 /* frames in flight */

typedef struct {
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    VkDevice dev;
    u32 gfx_family, compute_family; /* compute_family == gfx_family when there is no separate compute queue */
    VkQueue gfx, compute;
    PMutex gfx_lock; /* vkQueueSubmit/vkQueuePresentKHR on gfx are externally synchronized */
    /* swapchain */
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    VkPresentModeKHR sc_mode;
    u32 sc_count;
    VkImage sc_images[8];
    VkSemaphore sc_done[8]; /* per swapchain image: rendering into it finished, it can be presented */
} VkState;
extern VkState g_vk;

/* the window backend's side (backend_sdl3.c) */
void* backend_vk_loader(void); /* vkGetInstanceProcAddr */
const char* const* backend_vk_extensions(u32* n);
int backend_vk_surface(VkInstance instance, VkSurfaceKHR* surface);
void backend_window_size(int* w, int* h);

int vk_init(void);
int vk_swapchain_create(void);
void vk_swapchain_destroy(void);
u32 vk_memory_type(u32 bits, VkMemoryPropertyFlags want);
const char* vk_result_str(VkResult r);
/* logs a failed call with where it happened; evaluates to 1 on success */
#define VK_OK(call) vk_ok_((call), #call, __FILE__, __LINE__)
int vk_ok_(VkResult r, const char* what, const char* file, int line);

#endif
