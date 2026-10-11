/* Vulkan renderer: shared state and the interfaces between its parts. Functions are loaded through volk from the
   loader the window backend gives (SDL's), so nothing links against a Vulkan library.

   vk_device.c   instance, device, queues, swapchain, memory allocator (VMA)
   vk_cmd.c      frames in flight: the command buffer being recorded, a staging ring, deferred destruction
   vk_format.c   guest render target and texture formats -> Vulkan formats
   vk_surface.c  render targets and textures by guest address, uploads from and write-backs to guest memory
   vk_texture.c  textures from the guest's texture headers (TIC), views and samplers (TSC)
   vk_buffer.c   vertex, index, constant and storage buffers from guest memory
   vk_pipeline.c translated shaders, the pipeline layout and pipelines by state
   vk_render.c   the render target set (dynamic rendering), clears and draws
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
#include "shader.h"

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
    /* optional features that were enabled */
    VkPhysicalDeviceFeatures feat;
    int has_divisor;      /* VK_EXT_vertex_attribute_divisor: instance divisors above 1 */
    int has_border_color; /* VK_EXT_custom_border_color */
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
/* a descriptor set from the current frame's pools, freed when the frame is reused */
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout);
/* the staging alignment that suits every use: uniform, storage, vertex and index buffers */
u64 vk_stage_align(void);
u64 vk_hash(const void* p, u64 size);
/* guest submits processed so far, and frames presented: what the caches check memory against */
extern u32 g_vk_epoch, g_vk_present;

/* vk_format.c */
typedef struct {
    VkFormat vk;
    u8 bpp; /* bytes per pixel (per block for compressed formats) */
    u8 bw, bh;
    u8 depth, stencil, is_int;
    u8 raw;  /* guest memory holds exactly the Vulkan format's bytes: it can be copied both ways */
    u8 sint; /* signed integer (is_int) */
    u8 conv; /* textures: converted on upload (VKC_*), the Vulkan format is what it converts to */
    u8 bgra; /* stored B, G, R, A in memory: a texture header's swizzle swaps R and B */
} VkFmt;
enum { VKC_NONE, VKC_ASTC, VKC_RGBA4, VKC_RGB5A1, VKC_RGB32F };
VkFmt vk_rt_format(u32 fmt);
VkFmt vk_zt_format(u32 fmt);
VkFmt vk_tic_format(u32 w0, int srgb); /* bpp 0: not supported */

/* vk_surface.c: a render target or a texture */
typedef struct {
    u64 key; /* view type | swizzle | format */
    VkImageView v;
} SurfView;
typedef struct Surf {
    u64 gva;
    u32 w, h, layers, levels;
    u32 depth3d; /* 3D textures: depth (layers is 1) */
    u32 fkey;    /* guest format | 0x10000 for depth */
    VkFmt f;
    u32 bl, bh, bd, pitch; /* guest layout: block linear with block height 2^bh GOBs, or pitch linear */
    int is_rt;
    VkImageViewType type; /* what it was created as */
    VkImage img;
    VmaAllocation mem;
    VkImageView view;     /* all levels and layers, for sampling */
    VkImageView att;      /* level 0, layer 0: the attachment */
    VkImageLayout layout; /* the whole image is kept in one layout */
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    u64 last_used;
    u32 wgen; /* render targets: bumped when drawn to */
    /* textures: views by type and swizzle, and the change check (vk_texture.c) */
    SurfView sv[6];
    int nsv;
    u64 size, hash, shash;
    u32 checked, next_check, stable, tfull;
    struct Surf* crop; /* a smaller texture header over this render target: a copy of that region */
    u32 crop_gen;
    struct Surf* next;
} Surf;
Surf* vk_rt(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth);
Surf* vk_rt_find(u64 gva);
Surf* vk_rt_for_cpu(u64 cpu_addr, u32 w, u32 h); /* the render target the guest presents from that memory */
/* moves a surface into a layout for a use, with the barrier that needs (which ends an open rendering pass) */
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
void vk_rt_flush(u64 gva);
void vk_rt_reload(u64 gva);
/* textures share the surface table */
Surf* vk_surf_first(u64 gva);
void vk_surf_add(Surf* s);
VkImageAspectFlags vk_aspect(const VkFmt* f);
void vk_surf_evict(u32 before); /* textures last used before that present */
u64 vk_bl_size(u32 w_bytes, u32 h, u32 bh);

/* vk_texture.c */
Surf* vk_tex_from_tic(const u32* tic);
/* a view of s as the shader declares it, sRGB or linear (color formats that have both), with a component mapping;
   VK_NULL_HANDLE when the image can't be viewed that way */
VkImageView vk_tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map);
/* an empty texture of a view type: num 0 float, 1 unsigned, 2 signed integer */
VkImageView vk_null_view(VkImageViewType type, int num);
VkSampler vk_sampler(const u32* tsc);
void vk_tex_frame(void); /* a frame was presented: evicts what wasn't used for a while */

/* vk_buffer.c: guest memory [gva, gva + len) as a buffer the GPU reads, valid until the next present */
int vk_buf_get(u64 gva, u64 len, VkBuffer* buf, u64* off);
/* a copy of len bytes made now (data that changes within a frame), or of zeros when gva is 0 */
int vk_buf_copy(u64 gva, u64 len, VkBuffer* buf, u64* off);
void vk_buf_frame(void);

/* vk_pipeline.c */
typedef struct VkSh {
    u64 hash;
    u32 key2;
    int stage;
    ShaderInfo si;
    VkShaderModule mod;
    int failed;
    u32 clen, clen_a;
    u64 qh, qh_a;
    struct VkSh* next;
} VkSh;
extern VkPipelineLayout g_vk_playout;
extern VkDescriptorSetLayout g_vk_dslayout;
void vk_pipeline_init(void);
/* the shader at gva (after a VertexA program at gva_a, or 0). The masks key integer inputs: attributes for the
   vertex stage, textures and render target outputs for the fragment stage */
VkSh* vk_shader_get(u64 gva, u64 gva_a, int stage, u32 int_mask, u32 sint_mask, u32 rt_int, u32 rt_sint);
/* the bound constant buffers per stage, which the translator also reads to resolve indirect branches */
extern u64 g_vk_cb[5][18];
extern u32 g_vk_cbsize[5][18];
typedef struct {
    VkShaderModule vs, fs;
    u32 topo_class; /* POINT_LIST, LINE_LIST or TRIANGLE_LIST: the topology is dynamic within its class */
    u32 polygon;
    u32 nattr, nbind;
    struct {
        u32 loc, binding, format, offset;
    } attr[16];
    struct {
        u32 binding, divisor; /* divisor 0: per vertex */
    } bind[17];               /* a binding per attribute, and one for constant attributes */
    u32 ncolor;
    u32 color[8];
    u32 depth, stencil;
    struct {
        u32 en, sc, dc, cop, sa, da, aop, mask;
    } blend[8];
} PipeKey;
VkPipeline vk_pipeline_get(const PipeKey* k);

/* vk_render.c */
void vk_render_end(void); /* ends the dynamic rendering pass, if one is open */
void vk_render_clear(GpuChannel* ch, u32 v);
void vk_render_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
                    u32 instances);

#endif
