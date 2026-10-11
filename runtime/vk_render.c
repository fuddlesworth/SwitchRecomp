/* The render target set and clears. The 3D registers name up to 8 color targets (RT_CONTROL at 0x487 maps the
   slots, each target's address, size, format and tiling at 0x200 + 0x10 * index) and a depth target (enabled at
   0x54E, address at 0x3F8, size at 0x48A, format and tiling at 0x3FA). A dynamic rendering pass stays open while
   draws and clears keep the same set, and ends before anything that copies or transitions images. */
#include "vk.h"

static struct {
    Surf* rt[8];
    Surf* zt;
    u32 n; /* color slots up to the last bound one */
    VkExtent2D ext;
    int open;
} g_rp;

void vk_render_end(void) {
    if (!g_rp.open) return;
    vkCmdEndRendering(vk_cmd());
    g_rp.open = 0;
}

/* the targets the registers name, without opening anything */
static void read_targets(GpuChannel* ch, Surf** rt, Surf** zt, u32* n) {
    u32* r = ch->r3d;
    u32 ctl = r[0x487], count = ctl & 15;
    if (count > 8) count = 8;
    *n = 0;
    for (u32 i = 0; i < 8; i++) {
        rt[i] = NULL;
        if (i >= count) continue;
        u32* t = &r[0x200 + ((ctl >> (4 + i * 3)) & 7) * 0x10];
        u64 a = ((u64)t[0] << 32) | t[1];
        if (a && t[2] && t[3] && t[4]) {
            rt[i] = vk_rt(a, t[2], t[3], t[4], t[5], 0);
            if (rt[i]) *n = i + 1;
        }
    }
    *zt = NULL;
    if (r[0x54E]) {
        u64 za = ((u64)r[0x3F8] << 32) | r[0x3F9];
        if (za && r[0x48A] && r[0x48B]) *zt = vk_rt(za, r[0x48A], r[0x48B], r[0x3FA], r[0x3FB], 1);
    }
}

/* opens a rendering pass on the registers' targets, or keeps the open one if it is the same set */
static int render_begin(GpuChannel* ch) {
    Surf* rt[8];
    Surf* zt;
    u32 n;
    read_targets(ch, rt, &zt, &n); /* may record uploads: before anything is opened */
    if (!n && !zt) return 0;
    if (g_rp.open && g_rp.n == n && g_rp.zt == zt && !memcmp(g_rp.rt, rt, sizeof rt)) return 1;
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca[8];
    VkExtent2D ext = {~0u, ~0u};
    for (u32 i = 0; i < n; i++) {
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (!rt[i]) continue;
        vk_surf_use(cb, rt[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        ca[i].imageView = rt[i]->att;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (rt[i]->w < ext.width) ext.width = rt[i]->w;
        if (rt[i]->h < ext.height) ext.height = rt[i]->h;
    }
    VkRenderingAttachmentInfo za = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    if (zt) {
        vk_surf_use(cb, zt, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        za.imageView = zt->att;
        za.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (zt->w < ext.width) ext.width = zt->w;
        if (zt->h < ext.height) ext.height = zt->h;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, ext},
                          .layerCount = 1,
                          .colorAttachmentCount = n,
                          .pColorAttachments = ca,
                          .pDepthAttachment = zt ? &za : NULL,
                          .pStencilAttachment = zt && zt->f.stencil ? &za : NULL};
    vkCmdBeginRendering(cb, &ri);
    memcpy(g_rp.rt, rt, sizeof rt);
    g_rp.zt = zt;
    g_rp.n = n;
    g_rp.ext = ext;
    g_rp.open = 1;
    return 1;
}

/* v: bit 0 depth, 1 stencil, 2-5 the RGBA mask, 6-9 the color slot, 10-20 the layer */
void vk_render_clear(GpuChannel* ch, u32 v) {
    u32* r = ch->r3d;
    u32 slot = (v >> 6) & 15, mask = (v >> 2) & 15;
    float cc[4], dz;
    memcpy(cc, &r[0x360], 16);
    memcpy(&dz, &r[0x364], 4);
    if (!render_begin(ch)) return;
    VkClearRect rect = {{{0, 0}, g_rp.ext}, 0, 1};
    if ((r[0x43E] & 0x10) && (r[0x380] & 1)) { /* scissored clear */
        u32 xs = r[0x381], ys = r[0x382];
        i32 x0 = (i32)(xs & 0xFFFF), x1 = (i32)(xs >> 16), y0 = (i32)(ys & 0xFFFF), y1 = (i32)(ys >> 16);
        if (x1 > (i32)g_rp.ext.width) x1 = (i32)g_rp.ext.width;
        if (y1 > (i32)g_rp.ext.height) y1 = (i32)g_rp.ext.height;
        if (x1 <= x0 || y1 <= y0) return;
        rect.rect = (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
    }
    VkClearAttachment at[2];
    u32 na = 0;
    if (mask && slot < g_rp.n && g_rp.rt[slot]) {
        if (mask == 15) {
            at[na] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = slot};
            if (g_rp.rt[slot]->f.is_int)
                for (int i = 0; i < 4; i++) at[na].clearValue.color.uint32[i] = (u32)cc[i];
            else
                memcpy(at[na].clearValue.color.float32, cc, 16);
            na++;
        } else {
            static int said;
            if (!said) {
                said = 1;
                LOG("vulkan: clears with a partial color mask aren't done yet (they need the draw pipeline)\n");
            }
        }
    }
    Surf* zt = g_rp.zt;
    if ((v & 3) && zt) {
        VkImageAspectFlags a = 0;
        if (v & 1) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((v & 2) && zt->f.stencil) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (a) {
            at[na] = (VkClearAttachment){.aspectMask = a};
            at[na].clearValue.depthStencil = (VkClearDepthStencilValue){dz, r[0x368] & 0xFF};
            na++;
        }
    }
    if (na) vkCmdClearAttachments(vk_cmd(), na, at, 1, &rect);
}

/* a draw: nothing is recorded yet (ROADMAP.md, Phase 6), but its targets are bound, so they exist and are presented */
void vk_render_draw(GpuChannel* ch) { render_begin(ch); }
