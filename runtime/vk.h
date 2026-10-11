/* Vulkan renderer: shared state and the interfaces between its parts. Functions are loaded through volk from the
   loader the window backend gives (SDL's), so nothing links against a Vulkan library.

   vk_device.c   instance, device, queues, swapchain, memory allocator (VMA)
   vk_cmd.c      frames in flight: the command buffer being recorded, a staging ring, deferred destruction
   vk_format.c   guest render target and texture formats -> Vulkan formats
   vk_surface.c  render targets and textures by guest address, uploads from and write-backs to guest memory
   vk_render.c   the render target set (dynamic rendering) and clears
   vk_shader.c   GLSL to SPIR-V
   gpu_vk.c      the renderer interface the GPU front end calls (gpu_gl_*, r3d_*_gl) and presenting

   Everything that records commands runs under vk_lock(): submits come from the game's GPU channel threads and
   presents from the thread that queues a buffer. */
#ifndef SWR_VK_H
#define SWR_VK_H
#include "rt.h"
#include "gpu.h"
#include "volk.h"
#include "vk_mem_alloc.h"

#define VK_FRAMES 2 /* frames in flight */

typedef struct {
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    VkDevice dev;
    VmaAllocator vma;
    u32 gfx_family, compute_family; /* compute_family == gfx_family when there is no separate compute queue */
    VkQueue gfx, compute;
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

/* vk_device.c */
int vk_init(void);
int vk_swapchain_create(void);
void vk_swapchain_destroy(void);
const char* vk_result_str(VkResult r);
/* logs a failed call with where it happened; evaluates to 1 on success */
#define VK_OK(call) vk_ok_((call), #call, __FILE__, __LINE__)
int vk_ok_(VkResult r, const char* what, const char* file, int line);
void vk_lock(void);
void vk_unlock(void);

/* vk_cmd.c */
void vk_cmd_init(void);
VkCommandBuffer vk_cmd(void); /* the current frame's command buffer, begun on first use */
/* size bytes of the current frame's staging memory (host visible, coherent), valid until that frame's work is done */
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off);
/* ends and submits the current frame's commands, waiting for wait (at wait_stage) and signaling signal, then starts
   the next frame. Either semaphore may be VK_NULL_HANDLE */
void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal);
void vk_finish(void); /* submits and waits until the GPU has done everything */
/* a buffer for GPU -> host copies: record the copy into it, vk_finish(), vk_readback_ready(), then read */
u8* vk_readback_buffer(u64 size, VkBuffer* buf);
void vk_readback_ready(void);
void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2);
void vk_defer_buffer(VkBuffer b, VmaAllocation a);
u64 vk_frame_number(void);
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to);

/* vk_format.c */
typedef struct {
    VkFormat vk;
    u8 bpp; /* bytes per pixel (per block for compressed formats) */
    u8 bw, bh;
    u8 depth, stencil, is_int;
    u8 raw; /* guest memory holds exactly the Vulkan format's bytes: it can be copied both ways */
} VkFmt;
VkFmt vk_rt_format(u32 fmt);
VkFmt vk_zt_format(u32 fmt);

/* vk_surface.c: a render target or a texture */
typedef struct Surf {
    u64 gva;
    u32 w, h, layers, levels;
    u32 fkey; /* guest format | 0x10000 for depth */
    VkFmt f;
    u32 bl, bh, pitch; /* guest layout: block linear with block height 2^bh GOBs, or pitch linear */
    int is_rt;
    VkImage img;
    VmaAllocation mem;
    VkImageView view;     /* all levels and layers, for sampling */
    VkImageView att;      /* level 0, layer 0: the attachment */
    VkImageLayout layout; /* the whole image is kept in one layout */
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    u64 last_used;
    struct Surf* next;
} Surf;
Surf* vk_rt(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth);
Surf* vk_rt_find(u64 gva);
Surf* vk_rt_for_cpu(u64 cpu_addr, u32 w, u32 h); /* the render target the guest presents from that memory */
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
void vk_rt_flush(u64 gva);
void vk_rt_reload(u64 gva);

/* vk_render.c */
void vk_render_end(void); /* ends the dynamic rendering pass, if one is open */
void vk_render_clear(GpuChannel* ch, u32 v);
void vk_render_draw(GpuChannel* ch);

#endif
