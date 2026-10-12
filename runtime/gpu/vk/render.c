/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* The render target set, clears and draws. The 3D registers name up to 8 color targets (RT_CONTROL at 0x487 maps
   the slots, each target's address, size, format and tiling at 0x200 + 0x10 * index) and a depth target (enabled
   at 0x54E, address at 0x3F8, size at 0x48A, format and tiling at 0x3FA). A dynamic rendering pass stays open while
   draws and clears keep the same set, and ends before anything that copies or transitions images.

   A draw reads the 3D engine's registers for its state. The viewport maps the guest's
   transform as is (y down, like the hardware), so no shader flips y; facing follows from that (see the raster
   state below). */
#include <stdio.h>
#include <string.h>
#include "core/options.h"
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include "platform/log.h"
#include "gpu/vk/vk.h"

static struct {
    Surf* rt[8];
    Surf* zt;
    u32 n; /* color slots up to the last bound one */
    VkExtent2D ext;
    int open;
    int zt_ro; /* the depth target is read-only in this pass: draws also sample it */
} g_rp;

void vk_render_end(void) {
    if (!g_rp.open) return;
    g_rp.open = 0;
    vkCmdEndRendering(vk_cmd());
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
            /* a pitch-linear target (bit 12 of the memory layout) holds its row pitch in bytes where the width is */
            u32 bpp = vk_rt_format(t[4]).bpp, linear = (t[5] >> 12) & 1, w = linear ? t[2] / bpp : t[2];
            if (w) rt[i] = vk_rt(a, w, t[3], t[4], t[5], 0, linear ? t[2] : w * bpp);
            if (rt[i]) *n = i + 1;
        }
    }
    *zt = NULL;
    if (r[0x54E]) {
        u64 za = ((u64)r[0x3F8] << 32) | r[0x3F9];
        if (za && r[0x48A] && r[0x48B])
            *zt = vk_rt(za, r[0x48A], r[0x48B], r[0x3FA], r[0x3FB], 1, r[0x48A] * vk_zt_format(r[0x3FA]).bpp);
    }
}

/* opens a rendering pass on these targets, or keeps the open one if it is the same set. zt_ro: the depth target is
   bound read-only, so draws can sample it too (soft particles test against depth they also read) */
static int render_begin(Surf** rt, Surf* zt, u32 n, int zt_ro) {
    if (!n && !zt) return 0;
    if (!zt) zt_ro = 0;
    if (g_rp.open && g_rp.n == n && g_rp.zt == zt && g_rp.zt_ro == zt_ro && !memcmp(g_rp.rt, rt, sizeof g_rp.rt))
        return 1;
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
        if (rt[i]->sw < ext.width) ext.width = rt[i]->sw;
        if (rt[i]->sh < ext.height) ext.height = rt[i]->sh;
    }
    VkRenderingAttachmentInfo za = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkImageLayout zl =
        zt_ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (zt) {
        if (zt_ro)
            vk_surf_use(cb, zt, zl,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        else
            vk_surf_use(cb, zt, zl,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        za.imageView = zt->att;
        za.imageLayout = zl;
        if (zt->sw < ext.width) ext.width = zt->sw;
        if (zt->sh < ext.height) ext.height = zt->sh;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, ext},
                          .layerCount = 1,
                          .colorAttachmentCount = n,
                          .pColorAttachments = ca,
                          .pDepthAttachment = zt ? &za : NULL,
                          .pStencilAttachment = zt && zt->f.stencil ? &za : NULL};
    vkCmdBeginRendering(cb, &ri);
    memcpy(g_rp.rt, rt, sizeof g_rp.rt);
    g_rp.zt = zt;
    g_rp.n = n;
    g_rp.ext = ext;
    g_rp.zt_ro = zt_ro;
    g_rp.open = 1;
    return 1;
}
static void targets_written(void) {
    for (u32 i = 0; i < g_rp.n; i++)
        if (g_rp.rt[i]) g_rp.rt[i]->wgen++;
    if (g_rp.zt) g_rp.zt->wgen++;
}

/* every dynamic state at a neutral value, for draws of our own */
static void plain_state(VkCommandBuffer cb, VkRect2D sc) {
    VkViewport vp = {0, 0, (float)g_rp.ext.width, (float)g_rp.ext.height, 0, 1};
    vkCmdSetViewportWithCount(cb, 1, &vp);
    vkCmdSetScissorWithCount(cb, 1, &sc);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
    vkCmdSetLineWidth(cb, 1.0f);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    vkCmdSetDepthBias(cb, 0, 0, 0);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    vkCmdSetDepthBounds(cb, 0.0f, 1.0f);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
}

/* an integer target's clear value: the float clear color register converted to the target's type */
static u32 clear_int(float f, int sint) {
    if (sint) return f >= 2147483520.0f ? 0x7FFFFFFFu : f <= -2147483648.0f ? 0x80000000u : f == f ? (u32)(i32)f : 0;
    return f >= 4294967040.0f ? 0xFFFFFFFFu : f > 0 ? (u32)f : 0;
}

/* a clear of some color components only: a full-screen triangle with that write mask, whose fragment shader writes
   the cleared slot alone, in the target's number type */
static void clear_masked(u32 slot, u32 mask, const float* cc, VkRect2D rect) {
    static VkShaderModule vs, fs[8][3];
    const VkFmt* f = &g_rp.rt[slot]->f;
    int num = f->is_int ? (f->sint ? 2 : 1) : 0;
    if (!vs)
        vs = vk_module("#version 450\nvoid main(){ vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
                       "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n",
                       ST_VS);
    if (!fs[slot][num]) {
        static const char* ty[3] = {"vec4", "uvec4", "ivec4"};
        char src[256];
        snprintf(src, sizeof src,
                 "#version 450\nlayout(push_constant) uniform PC { layout(offset=16) %s c; };\n"
                 "layout(location=%u) out %s o;\nvoid main(){ o = c; }\n",
                 ty[num], slot, ty[num]);
        fs[slot][num] = vk_module(src, ST_FS);
    }
    if (!vs || !fs[slot][num]) return;
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs;
    k.fs = fs[slot][num];
    k.topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k.polygon = VK_POLYGON_MODE_FILL;
    k.ncolor = g_rp.n;
    for (u32 i = 0; i < g_rp.n; i++) k.color[i] = g_rp.rt[i] ? g_rp.rt[i]->f.vk : VK_FORMAT_UNDEFINED;
    k.blend[slot].mask = ((mask & 1) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((mask & 2) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                         ((mask & 4) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((mask & 8) ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (g_rp.zt) {
        k.depth = g_rp.zt->f.vk;
        k.stencil = g_rp.zt->f.stencil ? g_rp.zt->f.vk : VK_FORMAT_UNDEFINED;
    }
    VkPipeline p = vk_pipeline_get(&k);
    if (!p) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    plain_state(cb, rect);
    u32 pc[8] = {0};
    for (int i = 0; i < 4; i++)
        if (num)
            pc[4 + i] = clear_int(cc[i], num == 2);
        else
            memcpy(&pc[4 + i], &cc[i], 4);
    vkCmdPushConstants(cb, g_vk_playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

/* v: bit 0 depth, 1 stencil, 2-5 the RGBA mask, 6-9 the color slot, 10-20 the layer */
void vk_render_clear(GpuChannel* ch, u32 v) {
    u32* r = ch->r3d;
    u32 slot = (v >> 6) & 15, mask = (v >> 2) & 15;
    float cc[4], dz;
    memcpy(cc, &r[0x360], 16);
    memcpy(&dz, &r[0x364], 4);
    dz = dz > 1 ? 1 : dz >= 0 ? dz : 0; /* Vulkan takes 0..1 only */
    Surf* rt[8];
    Surf* zt;
    u32 n;
    read_targets(ch, rt, &zt, &n); /* may record uploads: before anything is opened */
    if (mask) {
        /* the slot is taken through RT_CONTROL's count and permutation, like draws do. Whether the hardware takes the
           target registers of that index directly isn't settled: the first clear where the two differ is logged */
        u32 ctl = r[0x487];
        if (slot >= (ctl & 15) || ((ctl >> (4 + slot * 3)) & 7) != slot)
            LOG_LIMIT(1, "vulkan: a clear of color slot %u under RT_CONTROL %08x\n", slot, ctl);
    }
    if (!render_begin(rt, zt, n, 0)) return;
    VkClearRect rect = {{{0, 0}, g_rp.ext}, 0, 1};
    if ((r[0x43E] & 0x10) && (r[0x380] & 1)) { /* scissored clear */
        u32 xs = r[0x381], ys = r[0x382];
        i32 x0 = (i32)vk_scf(xs & 0xFFFF), x1 = (i32)vk_scf(xs >> 16), y0 = (i32)vk_scf(ys & 0xFFFF),
            y1 = (i32)vk_scf(ys >> 16);
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
            if (g_rp.rt[slot]->f.is_int) {
                /* the float register converted. Whether it holds the integers' raw bits instead isn't settled: the
                   first such clear is logged with both readings */
                LOG_LIMIT(1, "vulkan: integer clear: %08x %08x %08x %08x (as floats %g %g %g %g)\n", r[0x360], r[0x361],
                          r[0x362], r[0x363], cc[0], cc[1], cc[2], cc[3]);
                for (int i = 0; i < 4; i++) at[na].clearValue.color.uint32[i] = clear_int(cc[i], g_rp.rt[slot]->f.sint);
            } else
                memcpy(at[na].clearValue.color.float32, cc, 16);
            na++;
        } else {
            clear_masked(slot, mask, cc, rect.rect);
            targets_written();
        }
    }
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
    if (na) {
        vkCmdClearAttachments(vk_cmd(), na, at, 1, &rect);
        targets_written();
    }
}

/* ------------------------------------------------------------ register values -> Vulkan */
static VkBlendFactor blend_factor(u32 v) {
    if (v & 0x4000)
        v &= ~0x4000u; /* a GL enum */
    else
        switch (v) { /* the D3D-style numbering */
            case 1: return VK_BLEND_FACTOR_ZERO;
            case 2: return VK_BLEND_FACTOR_ONE;
            case 3: return VK_BLEND_FACTOR_SRC_COLOR;
            case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
            case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case 7: return VK_BLEND_FACTOR_DST_ALPHA;
            case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case 9: return VK_BLEND_FACTOR_DST_COLOR;
            case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
            case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
            case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
            case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
            default: return VK_BLEND_FACTOR_ONE;
        }
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        /* the dual-source factors as the hardware numbers them (0xC900-0xC903), not by their GL values */
        case 0x8900: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 0x8901: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x8902: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 0x8903: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x88F9: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 0x88FA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x8589: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 0x88FB: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(u32 v) {
    switch (v) {
        case 2:
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 3:
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4:
        case 0x8007: return VK_BLEND_OP_MIN;
        case 5:
        case 0x8008: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}
static VkCompareOp cmp_op(u32 v) {
    if (v >= 0x200 && v <= 0x207) return (VkCompareOp)(v - 0x200);
    if (v >= 1 && v <= 8) return (VkCompareOp)(v - 1);
    return VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp stencil_op(u32 v) {
    switch (v) {
        case 1:
        case 0x1E00: return VK_STENCIL_OP_KEEP;
        case 3:
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 4:
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 5:
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 6:
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 7:
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 8:
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_ZERO;
}

/* vertex attribute format from the size code (bits 21-26) and component type (27-29): 1 snorm, 2 unorm, 3 sint,
   4 uint, 5 uscaled, 6 sscaled, 7 float */
static VkFormat attr_format(u32 size, u32 ty, int bgra) {
#define BY(sn, un, si, ui, us, ss, fl)                                                                                 \
    (ty == 1 ? sn : ty == 2 ? un : ty == 3 ? si : ty == 4 ? ui : ty == 5 ? us : ty == 6 ? ss : fl)
    /* Vulkan has no 32-bit scaled formats, and an integer format would hand the float input the integers' bits */
    if ((ty == 5 || ty == 6) && (size == 0x01 || size == 0x02 || size == 0x04 || size == 0x12)) {
        LOG_LIMIT(1, "vulkan: 32-bit scaled vertex attributes aren't done yet (read as 0,0,0,1)\n");
        return VK_FORMAT_UNDEFINED;
    }
    switch (size) {
        case 0x01:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SINT,
                      VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                      VK_FORMAT_R32G32B32A32_SFLOAT);
        case 0x02:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_UINT,
                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_SFLOAT);
        case 0x04:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_UINT,
                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_SFLOAT);
        case 0x12:
            return BY(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SINT, VK_FORMAT_R32_UINT,
                      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT);
        case 0x03:
            return BY(VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SINT,
                      VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                      VK_FORMAT_R16G16B16A16_SFLOAT);
        case 0x05:
            return BY(VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SINT,
                      VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED,
                      VK_FORMAT_R16G16B16_SFLOAT);
        case 0x0F:
            return BY(VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_UINT,
                      VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_SFLOAT);
        case 0x1B:
            return BY(VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SINT, VK_FORMAT_R16_UINT,
                      VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16_SFLOAT);
        case 0x0A:
            if (bgra && (ty == 2 || ty == 7)) return VK_FORMAT_B8G8R8A8_UNORM;
            return BY(VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SINT,
                      VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                      VK_FORMAT_R8G8B8A8_UNORM);
        case 0x13:
            return BY(VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8_UINT,
                      VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8_UNORM);
        case 0x18:
            return BY(VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_UINT,
                      VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8_UNORM);
        case 0x1D:
            return BY(VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SINT, VK_FORMAT_R8_UINT,
                      VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R8_UNORM);
        case 0x30: /* the type decides the shader's input type too (integer or float): each gets its own format */
            return BY(VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                      VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32,
                      VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                      VK_FORMAT_A2B10G10R10_UNORM_PACK32);
        case 0x31: return ty == 3 || ty == 4 ? VK_FORMAT_UNDEFINED : VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
#undef BY
    return VK_FORMAT_UNDEFINED;
}
static int vertex_format_ok(VkFormat f) {
    static VkFormat ok[64], bad[64];
    static int nok, nbad;
    if (f == VK_FORMAT_UNDEFINED) return 0;
    for (int i = 0; i < nok; i++)
        if (ok[i] == f) return 1;
    for (int i = 0; i < nbad; i++)
        if (bad[i] == f) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    int r = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    if (r && nok < 64) ok[nok++] = f;
    if (!r && nbad < 64) {
        bad[nbad++] = f;
        WARN("vulkan: the GPU can't read vertex format %d (those attributes read as 0,0,0,1)\n", (int)f);
    }
    return r;
}

/* bytes an attribute of a size code (bits 21-26) reads */
static u32 attr_bytes(u32 size) {
    switch (size) {
        case 0x01: return 16;
        case 0x02: return 12;
        case 0x03:
        case 0x04: return 8;
        case 0x05: return 6;
        case 0x13: return 3;
        case 0x18:
        case 0x1B: return 2;
        case 0x1D: return 1;
    }
    return 4; /* 0x0A, 0x0F, 0x12, 0x30, 0x31 */
}

/* ------------------------------------------------------------ resources of a draw */
static u32 tex_handle(int stage, u32 tcb, const ShTex* st) {
    u32 ci = (st->bindless ? st->bl_cbuf : tcb) & 31, c2i = st->bl2_cbuf & 31;
    u64 cba = ci < 18 ? g_vk_cb[stage][ci] : 0;
    u32 h = cba ? gmem_r32(cba + st->cbuf_off * 4) : 0;
    if (st->bindless && st->bl2) {
        u64 c2 = c2i < 18 ? g_vk_cb[stage][c2i] : 0;
        if (c2) h |= gmem_r32(c2 + st->bl2_off * 4);
    }
    return h;
}

/* a texture binding of a draw: the headers its handle names, and the texture */
typedef struct {
    u32 handle, have;
    u32 tic[8], tsc[8];
    Surf* t;
} TexRes;
static void tex_resolve(GpuChannel* ch, u32 handle, TexRes* x) {
    u32* r = ch->r3d;
    if (r[0x48D]) {
        /* samplers are taken by the handle's sampler index. Ryujinx reads this register as the sampler index mode,
           where one mode takes the texture's index instead: whether a game uses it isn't known yet */
        LOG_LIMIT(1, "vulkan: sampler index mode %u\n", r[0x48D]);
    }
    u64 tic_base = ((u64)r[0x55D] << 32) | r[0x55E], tsc_base = ((u64)r[0x557] << 32) | r[0x558];
    gmem_read(tic_base + (handle & 0xFFFFF) * 32ull, x->tic, 32);
    gmem_read(tsc_base + ((handle >> 20) & 0xFFF) * 32ull, x->tsc, 32);
    x->t = vk_tex_from_tic(x->tic);
    x->handle = handle;
    x->have = 1;
}

/* the fragment shader keyed on which of its textures and outputs are integer. The textures it resolves for that are
   kept in res (SH_MAXTEX entries), for stage_set */
static VkSh* fs_get_keyed(GpuChannel* ch, u64 addr, TexRes* res) {
    u32* r = ch->r3d;
    u32 ctl = r[0x487], count = ctl & 15;
    if (count > 8) count = 8;
    u32 rm = 0, rs = 0;
    for (u32 i = 0; i < count; i++) {
        u32* t = &r[0x200 + ((ctl >> (4 + i * 3)) & 7) * 0x10];
        if (!t[4]) continue;
        VkFmt f = vk_rt_format(t[4]);
        if (f.is_int) {
            rm |= 1u << i;
            if (f.sint) rs |= 1u << i;
        }
    }
    VkSh* s0 = vk_shader_get(addr, 0, ST_FS, 0, 0, 0, 0, 0);
    if (!s0) return s0;
    u32 tm = 0, ts = 0;
    int nt = s0->si.ntex > SH_MAXTEX ? SH_MAXTEX : s0->si.ntex;
    for (int i = 0; i < nt; i++) {
        ShTex* st = &s0->si.tex[i];
        if (st->shadow) continue;
        tex_resolve(ch, tex_handle(4, r[0x982], st), &res[i]);
        Surf* t = res[i].t;
        u32 ty = (res[i].tic[0] >> 7) & 7;
        if (t ? t->f.is_int : (ty == 3 || ty == 4)) {
            tm |= 1u << i;
            if (t ? t->f.sint : ty == 3) ts |= 1u << i;
        }
    }
    rm &= s0->si.sph[18] ? 0xFF : 0;
    if (!tm && !rm) return s0;
    return vk_shader_get(addr, 0, ST_FS, tm, ts, rm, rs, 0);
}

VkImageViewType vk_tex_view_type(const ShTex* st) {
    return st->type == 2   ? VK_IMAGE_VIEW_TYPE_3D
           : st->type == 3 ? (st->array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
           : st->array     ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                           : VK_IMAGE_VIEW_TYPE_2D;
}

VkImageView vk_tex_bind_view(Surf* t, VkImageViewType want, const u32* tic, int num) {
    if (!t || (t->f.is_int ? (t->f.sint ? 2 : 1) : 0) != num) return VK_NULL_HANDLE;
    static const VkComponentSwizzle sw[8] = {
        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R,   VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A,    VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE};
    VkComponentSwizzle m[4] = {sw[(tic[0] >> 19) & 7], sw[(tic[0] >> 22) & 7], sw[(tic[0] >> 25) & 7],
                               sw[(tic[0] >> 28) & 7]};
    /* the header's swizzle is in memory component order: B, G, R, A targets swap R and B */
    if (t->f.bgra)
        for (int k = 0; k < 4; k++)
            m[k] = m[k] == VK_COMPONENT_SWIZZLE_R   ? VK_COMPONENT_SWIZZLE_B
                   : m[k] == VK_COMPONENT_SWIZZLE_B ? VK_COMPONENT_SWIZZLE_R
                                                    : m[k];
    if (t->f.depth) { /* depth in r, g and b, alpha 1 */
        m[0] = m[1] = m[2] = VK_COMPONENT_SWIZZLE_R;
        m[3] = VK_COMPONENT_SWIZZLE_ONE;
    }
    VkComponentMapping map = {m[0], m[1], m[2], m[3]};
    return vk_tex_view(t, want, (tic[4] >> 22) & 1, &map);
}

VkSampler vk_tex_bind_sampler(const u32* tsc, const ShTex* st, int num) {
    u32 t[8];
    memcpy(t, tsc, sizeof t);
    /* the compare mode is what the shader declares */
    if (st->shadow) {
        if (!((t[0] >> 9) & 1)) t[0] = (t[0] & ~(7u << 10)) | 1u << 9 | 3u << 10; /* LEQUAL */
    } else
        t[0] &= ~(1u << 9);
    /* integer textures can't be filtered: nearest, and a linear mip filter becomes nearest */
    if (num) {
        u32 mip = (t[1] >> 6) & 3;
        t[1] = (t[1] & ~0xF3u) | 0x11u | (mip == 3 ? 2u : mip) << 6;
    }
    return vk_sampler(t);
}

/* a constant buffer's bytes for this draw, reused while the guest can't have changed them */
static int cb_get(int stage, int i, u32 sz, VkBuffer* buf, u64* off) {
    static struct {
        u64 a, gw, frame;
        u32 sz, epoch;
        VkBuffer buf;
        u64 off;
    } c[5][18];
    u64 a = g_vk_cb[stage][i];
    /* pending compute results there land first: the checks below only see memory */
    if (g_cs_any && a) vk_compute_mem_need(a, sz);
    if (a && c[stage][i].a == a && c[stage][i].epoch == g_vk_epoch && c[stage][i].frame == vk_frame_number() &&
        c[stage][i].sz >= sz && !gpu_mem_dirty_since(c[stage][i].gw, a, sz)) {
        *buf = c[stage][i].buf;
        *off = c[stage][i].off;
        return 1;
    }
    if (!vk_buf_copy(a, sz, buf, off)) return 0;
    c[stage][i].a = a;
    c[stage][i].gw = g_gw_gen;
    c[stage][i].frame = vk_frame_number();
    c[stage][i].sz = sz;
    c[stage][i].epoch = g_vk_epoch;
    c[stage][i].buf = *buf;
    c[stage][i].off = *off;
    return 1;
}

/* small constants in this frame's staging: 1/resolution scale (SWR_RS) at 0, the 0,0,0,1 of attributes without
   data at 16 (float) and 32 (integer) */
static int frame_consts(VkBuffer* buf, u64* off) {
    static u64 frame = ~0ull, o;
    static VkBuffer b;
    if (frame != vk_frame_number()) {
        u8* p = vk_stage(64, vk_stage_align(), &b, &o);
        if (!p) return 0;
        float rs[4] = {1 / g_vk_scale, 1 / g_vk_scale, g_vk_scale, 0}, fa[4] = {0, 0, 0, 1};
        u32 ia[4] = {0, 0, 0, 1};
        memcpy(p, rs, 16);
        memcpy(p + 16, fa, 16);
        memcpy(p + 32, ia, 16);
        frame = vk_frame_number();
    }
    *buf = b;
    *off = o;
    return 1;
}

typedef struct {
    VkWriteDescriptorSet w[SWR_VKB_FMT + 1];
    VkDescriptorBufferInfo bi[SWR_VKB_FMT + 1];
    VkDescriptorImageInfo ii[SWR_VKB_FMT + 1];
    u32 n;
} DescWrites;
static void dw_buffer(DescWrites* d, VkDescriptorSet set, u32 binding, VkDescriptorType type, VkBuffer b, u64 off,
                      u64 range) {
    d->bi[binding] = (VkDescriptorBufferInfo){b, off, range};
    d->w[d->n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = set,
                                          .dstBinding = binding,
                                          .descriptorCount = 1,
                                          .descriptorType = type,
                                          .pBufferInfo = &d->bi[binding]};
}

static int is_target(Surf* s, Surf** rt, Surf* zt, u32 n) {
    if (s == zt) return 1;
    for (u32 i = 0; i < n; i++)
        if (rt[i] == s) return 1;
    return 0;
}

/* a stage's descriptor set: constant buffers, storage buffers, textures. *zt_ro is set when it samples the depth
   target, which the pass then binds read-only. res: textures fs_get_keyed resolved for this stage, or NULL */
static VkDescriptorSet stage_set(GpuChannel* ch, int stage, VkSh* s, Surf** rt, Surf* zt, u32 n, int* zt_ro,
                                 TexRes* res) {
    u32* r = ch->r3d;
    VkDescriptorSet set = vk_desc_alloc(g_vk_dslayout[shader_vk_set(stage)]);
    if (!set) return VK_NULL_HANDLE;
    DescWrites d;
    d.n = 0;
    for (int i = 0; i < 18; i++) {
        if (!(s->si.cbuf_mask & (1u << i))) continue;
        u32 sz = g_vk_cbsize[stage][i];
        if (sz > 65536 || !sz) sz = 65536;
        if (s->si.cbuf_max[i] && sz > s->si.cbuf_max[i]) sz = s->si.cbuf_max[i]; /* only the bytes the shader reads */
        sz = (sz + 15) & ~15u;
        if (sz > g_vk.props.limits.maxUniformBufferRange) sz = g_vk.props.limits.maxUniformBufferRange;
        VkBuffer b;
        u64 o;
        if (!cb_get(stage, i, sz, &b, &o)) return VK_NULL_HANDLE;
        dw_buffer(&d, set, SWR_VKB_CB + i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b, o, sz);
    }
    if (s->si.gbuf_wmask) {
        LOG_LIMIT(1, "vulkan: a draw's shader writes global memory, which stays on the GPU (not done yet)\n");
    }
    u64 cb0 = g_vk_cb[stage][0];
    for (int i = 0; i < s->si.ngbuf && i < 16; i++) {
        u64 addr = 0;
        u32 size = 16;
        if (cb0) {
            u32 v[3];
            gmem_read(cb0 + s->si.gbuf_cbuf_off[i], v, 12);
            addr = ((u64)v[1] << 32) | v[0];
            size = v[2];
        }
        if (!size) size = 16;
        if (size > (16u << 20)) size = 16u << 20;
        size = (size + 3) & ~3u;
        VkBuffer b;
        u64 o;
        /* written ones get a copy of their own (the writes stay on the GPU) */
        int ok =
            addr && !((s->si.gbuf_wmask >> i) & 1) ? vk_buf_get(addr, size, &b, &o) : vk_buf_copy(addr, size, &b, &o);
        if (!ok) return VK_NULL_HANDLE;
        dw_buffer(&d, set, SWR_VKB_SSBO + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b, o, size);
    }
    VkBuffer cb;
    u64 co;
    if (stage == 4 && frame_consts(&cb, &co))
        dw_buffer(&d, set, SWR_VKB_RS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cb, co, 16);
    u32 tcb = r[0x982];
    int nt = s->si.ntex > SH_MAXTEX ? SH_MAXTEX : s->si.ntex;
    for (int i = 0; i < nt; i++) {
        ShTex* st = &s->si.tex[i];
        u32 handle = tex_handle(stage, tcb, st);
        /* what fs_get_keyed resolved, unless it is a render target or a copy of one: a crop of the same target at
           another size, for another stage, replaces it. Other textures stay until the next present */
        TexRes own, *x = res ? &res[i] : NULL;
        if (!x || !x->have || x->handle != handle || (x->t && x->t->is_rt)) {
            x = &own;
            tex_resolve(ch, handle, x);
        }
        Surf* t = x->t;
        VkImageViewType want = vk_tex_view_type(st);
        int num = st->shadow || !((s->si.tex_int_mask >> i) & 1) ? 0 : ((s->si.tex_sint_mask >> i) & 1) ? 2 : 1;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        /* sampling the depth target needs it read-only: no depth writes, and no stencil writes on either face */
        int sten_w = zt && zt->f.stencil && r[0x4E0] && ((r[0x4E7] & 0xFF) || (r[0x565] && (r[0x3D6] & 0xFF)));
        int depth_ro = t && t == zt && !r[0x4BA] && !sten_w;
        if (t && is_target(t, rt, zt, n) && !depth_ro) {
            LOG_LIMIT(1, "vulkan: a draw samples a color target it renders to (read as empty)\n");
        } else
            view = vk_tex_bind_view(t, want, x->tic, num);
        if (view && depth_ro) {
            *zt_ro = 1; /* render_begin moves it to the read-only layout */
            layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        } else if (view)
            vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        else
            view = vk_null_view(want, st->shadow ? 3 : num);
        if (!view) return VK_NULL_HANDLE;
        VkSampler smp = vk_tex_bind_sampler(x->tsc, st, num);
        if (!smp) return VK_NULL_HANDLE;
        u32 b = SWR_VKB_TEX + i;
        d.ii[b] = (VkDescriptorImageInfo){smp, view, layout};
        d.w[d.n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = set,
                                            .dstBinding = b,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            .pImageInfo = &d.ii[b]};
    }
    if (d.n) vkUpdateDescriptorSets(g_vk.dev, d.n, d.w, 0, NULL);
    return set;
}

/* ------------------------------------------------------------ draws */
u64 g_vk_drawn, g_vk_skipped;

void vk_render_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
                    u32 instances) {
    u32* r = ch->r3d;
    if (!count || !instances) return;
    /* shaders: VertexB always runs (after VertexA when that is on), the fragment stage when enabled */
    u64 code = ((u64)r[0x582] << 32) | r[0x583];
    u32 attr_int = 0, attr_sint = 0;
    for (int i = 0; i < 32; i++) {
        u32 ty = (r[0x458 + i] >> 27) & 7;
        if (ty == 3 || ty == 4) {
            attr_int |= 1u << i;
            if (ty == 3) attr_sint |= 1u << i;
        }
    }
    if ((r[0x820] | r[0x830] | r[0x840]) & 1) {
        LOG_LIMIT(1, "vulkan: draws with tessellation or geometry stages are drawn without them\n");
    }
    if (topology == 14 || (topology >= 10 && topology <= 13)) {
        g_vk_skipped++;
        return; /* patches and adjacency need the stages above */
    }
    /* SetUserClipEnable (0x544): a clip distance the program writes clips only when its bit is set */
    VkSh* vs = vk_shader_get(code + r[0x811], (r[0x800] & 1) ? code + r[0x801] : 0, ST_VS, attr_int, attr_sint, 0, 0,
                             r[0x544] & 0xFF);
    TexRes fres[SH_MAXTEX];
    for (int i = 0; i < SH_MAXTEX; i++) fres[i].have = 0;
    VkSh* fs = (r[0x850] & 1) ? fs_get_keyed(ch, code + r[0x851], fres) : NULL;
    if (!vs || vs->failed || (fs && fs->failed)) {
        g_vk_skipped++;
        return;
    }
    Surf* rt[8];
    Surf* zt;
    u32 n;
    read_targets(ch, rt, &zt, &n);
    if (!n && !zt) {
        g_vk_skipped++;
        return;
    }
    static u32 last_cw, last_ch; /* the size of the last color target drawn to */
    if (n && rt[0]) {
        last_cw = rt[0]->w;
        last_ch = rt[0]->h;
    }
    if (!g_opt_shadows && !n && zt && (zt->w != last_cw || zt->h != last_ch)) {
        /* shadows=0: depth-only passes at another size (shadow maps) are skipped, and the map is cleared to far once
           a frame so it reads as fully lit */
        static u32 cleared_frame = ~0u;
        static Surf* cleared;
        if (cleared_frame != g_vk_present || cleared != zt) {
            cleared_frame = g_vk_present;
            cleared = zt;
            if (render_begin(rt, zt, 0, 0)) {
                VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                                     (zt->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)};
                a.clearValue.depthStencil = (VkClearDepthStencilValue){1.0f, 0};
                VkClearRect cr = {{{0, 0}, g_rp.ext}, 0, 1};
                vkCmdClearAttachments(vk_cmd(), 1, &a, 1, &cr);
            }
        }
        g_vk_skipped++;
        return;
    }

    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs->mod;
    k.fs = fs ? fs->mod : VK_NULL_HANDLE;
    VkPrimitiveTopology topo;
    switch (topology) {
        case 0: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case 1: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case 2: /* line loop: drawn open */
        case 3: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case 5:
        case 8: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break; /* quad strips are triangle strips */
        case 6:
        case 9: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;   /* polygons are fans */
        default: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break; /* triangles, and quads made into them */
    }
    k.topo_class = topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST   ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                   : topo <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                              : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k.polygon = !g_vk.feat.fillModeNonSolid ? VK_POLYGON_MODE_FILL
                : r[0x36B] == 0x1B01        ? VK_POLYGON_MODE_LINE
                : r[0x36B] == 0x1B00        ? VK_POLYGON_MODE_POINT
                                            : VK_POLYGON_MODE_FILL;

    /* vertex streams: each used guest stream is a binding, and so is an attribute at an offset past the device's
       limit or reaching past its stream's stride (it is read from its own offset there); one more holds the 0,0,0,1
       of attributes without data */
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    u32 maxoff = lim->maxVertexInputAttributeOffset;
    u32 nloc = lim->maxVertexInputAttributes < 32 ? lim->maxVertexInputAttributes : 32;
    u32 maxbind = lim->maxVertexInputBindings < 33 ? lim->maxVertexInputBindings : 33;
    if (nloc < 32 && (vs->si.in_mask >> nloc)) {
        LOG_LIMIT(1, "vulkan: a vertex shader reads attributes past the %u this GPU has (draw skipped)\n", nloc);
        g_vk_skipped++;
        return;
    }
    VkBuffer vbuf[33];
    VkDeviceSize voff[33], vstride[33];
    u32 vkey[33]; /* guest stream | base offset << 5 */
    int cbind = -1, over = 0;
    for (u32 i = 0; i < nloc && !over; i++) {
        if (!(vs->si.in_mask & (1u << i))) continue;
        u32 a = r[0x458 + i];
        u32 sbuf = a & 31, off = (a >> 7) & 0x3FFF, size = (a >> 21) & 0x3F, ty = (a >> 27) & 7;
        u32* va = &r[0x700 + sbuf * 4];
        VkFormat fmt = attr_format(size, ty, (a >> 31) & 1);
        u64 start = ((u64)va[1] << 32) | va[2];
        u64 end = ((u64)r[0x7C0 + sbuf * 2] << 32) | r[0x7C1 + sbuf * 2];
        u64 len = end >= start ? end - start + 1 : 0;
        if (len > (64u << 20)) len = 64u << 20;
        int is_int = ty == 3 || ty == 4;
        if (((a >> 6) & 1) || !(va[0] & 0x1000) || !len || !vertex_format_ok(fmt)) {
            if (cbind < 0) {
                if (k.nbind == maxbind) {
                    over = 1;
                    continue;
                }
                if (!frame_consts(&vbuf[k.nbind], &voff[k.nbind])) continue;
                voff[k.nbind] += 16;
                vstride[k.nbind] = 0;
                vkey[k.nbind] = ~0u;
                cbind = (int)k.nbind;
                k.nbind++;
            }
            k.attr[k.nattr].loc = i;
            k.attr[k.nattr].binding = (u32)cbind;
            k.attr[k.nattr].format = is_int ? (ty == 3 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                                            : VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr[k.nattr].offset = is_int ? 16 : 0;
            k.nattr++;
            continue;
        }
        u32 stride = va[0] & 0xFFF, asz = attr_bytes(size);
        if (stride > lim->maxVertexInputBindingStride || (stride && asz > stride)) {
            LOG_LIMIT(1,
                      "vulkan: a vertex stream with stride %u (attribute of %u bytes) can't be read (draw skipped)\n",
                      stride, asz);
            g_vk_skipped++;
            return;
        }
        /* Vulkan wants every attribute of a binding within its stride */
        u32 base = off > maxoff || (stride && off + asz > stride) ? off : 0;
        u32 key = sbuf | base << 5, b = 0;
        while (b < k.nbind && vkey[b] != key) b++;
        if (b == k.nbind) {
            if (b == maxbind) {
                over = 1;
                continue;
            }
            if (!vk_buf_get(start, len, &vbuf[b], &voff[b])) {
                g_vk_skipped++;
                return;
            }
            voff[b] += base;
            vstride[b] = stride;
            vkey[b] = key;
            u32 div = r[0x620 + sbuf] ? (va[3] ? va[3] : 1) : 0;
            if (div > 1 && !g_vk.has_divisor) div = 1;
            k.bind[b].divisor = div;
            k.nbind++;
        }
        k.attr[k.nattr].loc = i;
        k.attr[k.nattr].binding = b;
        k.attr[k.nattr].format = fmt;
        k.attr[k.nattr].offset = off - base;
        k.nattr++;
    }
    if (over) {
        LOG_LIMIT(1, "vulkan: a draw needs more vertex bindings than the %u this GPU has (draw skipped)\n", maxbind);
        g_vk_skipped++;
        return;
    }

    /* attachments and blending */
    k.ncolor = n;
    for (u32 i = 0; i < n; i++) {
        if (!rt[i]) continue; /* format UNDEFINED, nothing written */
        k.color[i] = rt[i]->f.vk;
        u32 cm = r[0x3E4] ? r[0x680] : r[0x680 + i];
        k.blend[i].mask = ((cm & 0xF) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((cm & 0xF0) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                          ((cm & 0xF00) ? VK_COLOR_COMPONENT_B_BIT : 0) |
                          ((cm & 0xF000) ? VK_COLOR_COMPONENT_A_BIT : 0);
        if (r[0x4D8 + i] && !rt[i]->f.is_int) {
            /* the alpha fields are used even without separate alpha, as on hardware */
            u32 *b = &r[0x780 + i * 8], eq, sc, dc, eqa, sa, da;
            if (r[0x4B9]) {
                eq = b[1], sc = b[2], dc = b[3];
                eqa = b[4] ? b[4] : b[1], sa = b[5] ? b[5] : b[2], da = b[6] ? b[6] : b[3];
            } else {
                eq = r[0x4D0], sc = r[0x4D1], dc = r[0x4D2];
                eqa = r[0x4D3] ? r[0x4D3] : r[0x4D0], sa = r[0x4D4] ? r[0x4D4] : r[0x4D1];
                da = r[0x4D6] ? r[0x4D6] : r[0x4D2];
            }
            k.blend[i].en = 1;
            k.blend[i].cop = blend_op(eq);
            k.blend[i].sc = blend_factor(sc);
            k.blend[i].dc = blend_factor(dc);
            k.blend[i].aop = blend_op(eqa);
            k.blend[i].sa = blend_factor(sa);
            k.blend[i].da = blend_factor(da);
        }
    }
    if (zt) {
        k.depth = zt->f.vk;
        k.stencil = zt->f.stencil ? zt->f.vk : VK_FORMAT_UNDEFINED;
    }

    /* descriptor sets (may record uploads and barriers, so before the pass opens) */
    int zt_ro = 0;
    VkDescriptorSet vset = stage_set(ch, 0, vs, rt, zt, n, &zt_ro, NULL);
    VkDescriptorSet fset = fs ? stage_set(ch, 4, fs, rt, zt, n, &zt_ro, fres) : VK_NULL_HANDLE;
    if (!vset || (fs && !fset)) {
        g_vk_skipped++;
        return;
    }
    /* indices, up to the end of the index buffer (0x5F4, inclusive). What is made into other indices on the CPU (quads,
       8-bit indices, another restart index) is capped */
    VkBuffer ibuf = VK_NULL_HANDLE;
    u64 ioff = 0;
    VkIndexType itype = VK_INDEX_TYPE_UINT32;
    u32 ifmt = r[0x5F6], isz = ifmt == 0 ? 1 : ifmt == 1 ? 2 : 4;
    u64 ia = ((u64)r[0x5F2] << 32) | r[0x5F3], iend = ((u64)r[0x5F4] << 32) | r[0x5F5];
    if (indexed && iend >= ia && (u64)first + count > (iend - ia + 1) / isz) {
        u64 avail = (iend - ia + 1) / isz;
        LOG_LIMIT(1, "vulkan: a draw reads indices %u..%u of an index buffer of %llu (cut there)\n", first,
                  first + count - 1, (unsigned long long)avail);
        if (first >= avail) return;
        count = (u32)(avail - first);
    }
    if ((topology == 7 || (indexed && ifmt == 0)) && count > (1u << 24)) {
        LOG_LIMIT(1, "vulkan: a draw of %u vertices converted on the CPU (cut to %u)\n", count, 1u << 24);
        count = 1u << 24;
    }
    /* Vulkan restarts at all ones of the index type, in strips and fans; the guest's restart index is 0x592 */
    int restart = r[0x591] && topo != VK_PRIMITIVE_TOPOLOGY_POINT_LIST && topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST &&
                  topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    u32 icount = count;
    int quads_indexed = 0;
    if (topology == 7) { /* quads -> two triangles each */
        u32 nq = count / 4;
        const u8* src = NULL;
        if (indexed) src = gmem_ptr(ia + (u64)first * isz, (u64)count * isz);
        u32* idx = (u32*)vk_stage((u64)nq * 24 + 4, vk_stage_align(), &ibuf, &ioff);
        if (!idx) return;
        for (u32 q = 0; q < nq; q++) {
            u32 v[4];
            for (int j = 0; j < 4; j++) {
                u32 e = q * 4 + j;
                if (!indexed)
                    v[j] = first + e;
                else if (src)
                    v[j] = isz == 1 ? src[e] : isz == 2 ? ((const u16*)src)[e] : ((const u32*)src)[e];
                else {
                    v[j] = 0;
                    gmem_read(ia + ((u64)first + e) * isz, &v[j], isz);
                }
            }
            u32 t6[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
            memcpy(idx + q * 6, t6, 24);
        }
        icount = nq * 6;
        quads_indexed = indexed;
        indexed = 2; /* drawn indexed, with the first vertex in the indices */
    } else if (indexed) {
        if (ifmt == 0) { /* 8-bit indices need an extension: widen them */
            const u8* src = gmem_ptr(ia + first, count);
            u16* o = (u16*)vk_stage((u64)count * 2, vk_stage_align(), &ibuf, &ioff);
            if (!o) return;
            for (u32 j = 0; j < count; j++) {
                u8 v = 0;
                if (src)
                    v = src[j];
                else
                    gmem_read(ia + first + j, &v, 1);
                o[j] = r[0x591] && v == (r[0x592] & 0xFF) ? 0xFFFF : v; /* the index type's bits, as below */
            }
            itype = VK_INDEX_TYPE_UINT16;
        } else {
            u32 ones = isz == 2 ? 0xFFFFu : 0xFFFFFFFFu, ri = r[0x592] & ones;
            u64 bytes = (u64)count * isz;
            if (restart && ri != ones && count <= (1u << 24)) { /* the guest's restart index becomes Vulkan's */
                u8* o = vk_stage(bytes, vk_stage_align(), &ibuf, &ioff);
                if (!o) return;
                gmem_read(ia + (u64)first * isz, o, bytes);
                for (u32 j = 0; j < count; j++)
                    if (isz == 2 && ((u16*)o)[j] == ri)
                        ((u16*)o)[j] = 0xFFFF;
                    else if (isz == 4 && ((u32*)o)[j] == ri)
                        ((u32*)o)[j] = 0xFFFFFFFFu;
            } else if (!vk_buf_get(ia + (u64)first * isz, bytes, &ibuf, &ioff))
                return;
            itype = isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        }
    }

    VkPipeline pipe = vk_pipeline_get(&k);
    if (!pipe) {
        g_vk_skipped++;
        return;
    }
    if (!render_begin(rt, zt, n, zt_ro)) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorSet sets[2] = {vset, fset};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk_playout, 0, fs ? 2 : 1, sets, 0, NULL);
    if (k.nbind) vkCmdBindVertexBuffers2(cb, 0, k.nbind, vbuf, voff, NULL, vstride);

    /* viewport: the guest's transform as is, with a negative height when it flips y */
    float vt[6];
    memcpy(vt, &r[0x280], 24);
    float sx = vt[0], sy = vt[1], tx = vt[3], ty = vt[4], ax = sx < 0 ? -sx : sx, ay = sy < 0 ? -sy : sy;
    VkViewport vp;
    int yflip = sy < 0, small = 2 * ax < 1 || 2 * ay < 1;
    {
        /* the transform is taken as off when its scale is that small. 0x64B is the register that turns it off
           (Ryujinx), and 0x4EB (YControl) can negate y and flip facing. Neither is read yet: the first draw where
           they would matter is logged */
        if (small != !(r[0x64B] & 1))
            LOG_LIMIT(1, "vulkan: viewport scale %g x %g with transform enable %x\n", sx, sy, r[0x64B]);
        if (r[0x4EB]) LOG_LIMIT(1, "vulkan: YControl %x\n", r[0x4EB]);
    }
    if (small) {
        u32 vx = r[0x300], vy = r[0x301];
        vp = (VkViewport){vk_scf(vx & 0xFFFF), vk_scf(vy & 0xFFFF), vk_scf(vx >> 16), vk_scf(vy >> 16), 0, 1};
        yflip = 0;
    } else
        vp = (VkViewport){vk_scf(tx - ax), vk_scf(ty - sy), vk_scf(2 * ax), vk_scf(2 * sy), 0, 1};
    {
        float zn, zf;
        memcpy(&zn, &r[0x302], 4);
        memcpy(&zf, &r[0x303], 4);
        if (vt[2] != 0.0f || vt[5] != 0.0f) { /* depth through the viewport transform, as the hardware does */
            zn = vt[5] - ((r[0x35F] & 1) ? 0.0f : vt[2]);
            zf = vt[5] + vt[2];
        }
        vp.minDepth = zn < 0 ? 0 : zn > 1 ? 1 : zn;
        vp.maxDepth = zf < 0 ? 0 : zf > 1 ? 1 : zf;
    }
    if (!(vp.width > 0) || !(vp.height != 0)) return; /* no area: nothing is drawn (and Vulkan takes none) */
    vkCmdSetViewportWithCount(cb, 1, &vp);
    VkRect2D sc = {{0, 0}, g_rp.ext};
    if (r[0x380] & 1) {
        u32 xs = r[0x381], ys = r[0x382];
        i32 x0 = (i32)vk_scf(xs & 0xFFFF), x1 = (i32)vk_scf(xs >> 16), y0 = (i32)vk_scf(ys & 0xFFFF),
            y1 = (i32)vk_scf(ys >> 16);
        if (x1 > (i32)g_rp.ext.width) x1 = (i32)g_rp.ext.width;
        if (y1 > (i32)g_rp.ext.height) y1 = (i32)g_rp.ext.height;
        sc = x1 > x0 && y1 > y0 ? (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}} : (VkRect2D){{0, 0}, {0, 0}};
    }
    vkCmdSetScissorWithCount(cb, 1, &sc);
    /* raster state. Vulkan decides facing in framebuffer space with y down, where GL's lower-left origin reverses
       the winding, so the guest's front face holds as is when y is flipped and swaps when it isn't */
    vkCmdSetCullMode(cb, !r[0x646]           ? VK_CULL_MODE_NONE
                         : r[0x648] == 0x404 ? VK_CULL_MODE_FRONT_BIT
                         : r[0x648] == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK
                                             : VK_CULL_MODE_BACK_BIT);
    int cw = r[0x647] == 0x900;
    if (!yflip) cw = !cw;
    vkCmdSetFrontFace(cb, cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, topo);
    vkCmdSetPrimitiveRestartEnable(cb, restart && indexed == 1);
    vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
    {
        float lw; /* smooth lines (0x55C) have a width of their own */
        memcpy(&lw, &r[r[0x55C] ? 0x4EC : 0x4ED], 4);
        vkCmdSetLineWidth(cb, g_vk.feat.wideLines && lw > 0 && lw < 64 ? lw : 1.0f);
    }
    /* depth bias: enabled per polygon mode (0x370 points, 0x371 lines, 0x372 filled), clamped by 0x61F. The units
       are passed as they are (Ryujinx halves them: a run decides) */
    vkCmdSetDepthBiasEnable(cb, r[k.polygon == VK_POLYGON_MODE_POINT  ? 0x370
                                  : k.polygon == VK_POLYGON_MODE_LINE ? 0x371
                                                                      : 0x372] != 0);
    {
        float fac, un, clamp;
        memcpy(&fac, &r[0x55B], 4);
        memcpy(&un, &r[0x56F], 4);
        memcpy(&clamp, &r[0x61F], 4);
        vkCmdSetDepthBias(cb, un, g_vk.feat.depthBiasClamp ? clamp : 0.0f, fac);
    }
    if (!(r[0x64F] & (1u << 11))) {
        /* the hardware clamps depth unless bit 11 of 0x64F (ViewVolumeClipControl) is set (Ryujinx), and pipelines
           are made without depth clamp. The first draw that asks for it is logged */
        LOG_LIMIT(1, "vulkan: ViewVolumeClipControl %x asks for depth clamp (not done)\n", r[0x64F]);
    }
    {
        float bc[4];
        memcpy(bc, &r[0x4C7], 16);
        vkCmdSetBlendConstants(cb, bc);
    }
    /* depth and stencil */
    vkCmdSetDepthTestEnable(cb, zt && r[0x4B3]);
    vkCmdSetDepthWriteEnable(cb, zt && r[0x4BA]);
    vkCmdSetDepthCompareOp(cb, cmp_op(r[0x4C3]));
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    vkCmdSetDepthBounds(cb, 0.0f, 1.0f);
    int stencil = zt && zt->f.stencil && r[0x4E0];
    vkCmdSetStencilTestEnable(cb, stencil);
    /* GL order in the registers: fail, depth fail, pass */
    int two = r[0x565] != 0;
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_op(r[0x4E1]), stencil_op(r[0x4E3]), stencil_op(r[0x4E2]),
                      cmp_op(r[0x4E4]));
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, r[0x4E5]);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, r[0x4E6]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, r[0x4E7]);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, stencil_op(r[two ? 0x566 : 0x4E1]),
                      stencil_op(r[two ? 0x568 : 0x4E3]), stencil_op(r[two ? 0x567 : 0x4E2]),
                      cmp_op(r[two ? 0x569 : 0x4E4]));
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, r[two ? 0x3D5 : 0x4E5]);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, r[two ? 0x3D7 : 0x4E6]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, r[two ? 0x3D6 : 0x4E7]);
    /* push constants: the depth mode (swr_vp.y) for the vertex stage, the alpha test (0x4BB on, 0x4C4 reference,
       0x4C5 function) for the fragment stage */
    float pc[8] = {0, (r[0x35F] & 1) ? 0.0f : 1.0f, 0, 0};
    {
        u32 fn = r[0x4C5];
        int f = fn >= 0x200 && fn <= 0x207 ? (int)(fn - 0x200) : fn >= 1 && fn <= 8 ? (int)fn - 1 : 7;
        pc[4] = (r[0x4BB] & 1) ? 1.0f : 0.0f;
        pc[5] = (float)f;
        memcpy(&pc[6], &r[0x4C4], 4);
    }
    vkCmdPushConstants(cb, g_vk_playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);

    if (indexed) {
        vkCmdBindIndexBuffer(cb, ibuf, ioff, itype);
        /* quads made from guest indices keep the base vertex, quads made from vertex numbers have it in them */
        vkCmdDrawIndexed(cb, icount, instances, 0, indexed == 2 && !quads_indexed ? 0 : (i32)base_vertex,
                         base_instance);
    } else
        vkCmdDraw(cb, count, instances, first, base_instance);
    targets_written();
    g_vk_drawn++;
}
