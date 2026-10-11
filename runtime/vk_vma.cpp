/* VulkanMemoryAllocator's implementation. Its Vulkan functions are fetched at run time, like the rest of the
   renderer's (volk). */
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include "volk.h"
#include "vk_mem_alloc.h"
