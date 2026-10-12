/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* Guest formats -> Vulkan formats: render targets (the 3D class's RT_FORMAT and ZETA_FORMAT values) and textures
   (the texture header's format word). raw marks formats whose guest bytes are exactly the Vulkan format's, so the
   render target can be copied to and from guest memory as is. */
#include "platform/log.h"
#include "gpu/vk/vk.h"

/* whether optimal-tiling images of a format have these features, asked of the device once per format */
static int has_features(VkFormat f, VkFormatFeatureFlags need) {
    static struct {
        VkFormat f;
        VkFormatFeatureFlags have;
    } seen[64];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i].f == f) return (seen[i].have & need) == need;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    if (n < 64) {
        seen[n].f = f;
        seen[n++].have = p.optimalTilingFeatures;
    }
    return (p.optimalTilingFeatures & need) == need;
}

/* a format the tables don't have: 1 the first time it is seen (the first few formats), to say so once */
static int first_time(u32* said, u32 f) {
    for (int k = 0; k < 8; k++) {
        if (said[k] == f + 1) return 0;
        if (!said[k]) {
            said[k] = f + 1;
            return 1;
        }
    }
    return 0;
}

/* num: 0 float or normalized, 1 unsigned integer, 2 signed integer */
static VkFmt C(VkFormat f, int bpp, int num) {
    VkFmt r = {f, (u8)bpp, 1, 1, 0, 0, (u8)(num != 0), 1, (u8)(num == 2)};
    return r;
}
static VkFmt Cbgra(VkFormat f) { /* B, G, R, A in memory */
    VkFmt r = C(f, 4, 0);
    r.bgra = 1;
    return r;
}

static VkFmt rt_format(u32 f) {
    switch (f) {
        case 0xC0:
        case 0xC3: return C(VK_FORMAT_R32G32B32A32_SFLOAT, 16, 0); /* the X formats: the fourth component is padding */
        case 0xC1:
        case 0xC4: return C(VK_FORMAT_R32G32B32A32_SINT, 16, 2);
        case 0xC2:
        case 0xC5: return C(VK_FORMAT_R32G32B32A32_UINT, 16, 1);
        case 0xC6: return C(VK_FORMAT_R16G16B16A16_UNORM, 8, 0);
        case 0xC7: return C(VK_FORMAT_R16G16B16A16_SNORM, 8, 0);
        case 0xC8: return C(VK_FORMAT_R16G16B16A16_SINT, 8, 2);
        case 0xC9: return C(VK_FORMAT_R16G16B16A16_UINT, 8, 1);
        case 0xCA:
        case 0xCE: return C(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 0);
        case 0xCB: return C(VK_FORMAT_R32G32_SFLOAT, 8, 0);
        case 0xCC: return C(VK_FORMAT_R32G32_SINT, 8, 2);
        case 0xCD: return C(VK_FORMAT_R32G32_UINT, 8, 1);
        case 0xCF:
        case 0xE6: return Cbgra(VK_FORMAT_B8G8R8A8_UNORM);
        case 0xD0:
        case 0xE7: return Cbgra(VK_FORMAT_B8G8R8A8_SRGB);
        case 0xD1: return C(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 0);
        case 0xD2: return C(VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, 1);
        case 0xD5:
        case 0xF9: return C(VK_FORMAT_R8G8B8A8_UNORM, 4, 0);
        case 0xD6:
        case 0xFA: return C(VK_FORMAT_R8G8B8A8_SRGB, 4, 0);
        case 0xD7: return C(VK_FORMAT_R8G8B8A8_SNORM, 4, 0);
        case 0xD8: return C(VK_FORMAT_R8G8B8A8_SINT, 4, 2);
        case 0xD9: return C(VK_FORMAT_R8G8B8A8_UINT, 4, 1);
        case 0xDA: return C(VK_FORMAT_R16G16_UNORM, 4, 0);
        case 0xDB: return C(VK_FORMAT_R16G16_SNORM, 4, 0);
        case 0xDC: return C(VK_FORMAT_R16G16_SINT, 4, 2);
        case 0xDD: return C(VK_FORMAT_R16G16_UINT, 4, 1);
        case 0xDE: return C(VK_FORMAT_R16G16_SFLOAT, 4, 0);
        case 0xDF: return Cbgra(VK_FORMAT_A2R10G10B10_UNORM_PACK32); /* B10G10R10A2: blue in the low bits */
        case 0xE0: return C(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 0);
        case 0xE3: return C(VK_FORMAT_R32_SINT, 4, 2);
        case 0xE4: return C(VK_FORMAT_R32_UINT, 4, 1);
        case 0xE5: return C(VK_FORMAT_R32_SFLOAT, 4, 0);
        case 0xE8: return C(VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 0);
        case 0xE9: /* B5G5R5A1: blue in the low bits, alpha in the top one (Ryujinx's table) */
        case 0xF8: return C(VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 0);
        case 0xEA: return C(VK_FORMAT_R8G8_UNORM, 2, 0);
        case 0xEB: return C(VK_FORMAT_R8G8_SNORM, 2, 0);
        case 0xEC: return C(VK_FORMAT_R8G8_SINT, 2, 2);
        case 0xED: return C(VK_FORMAT_R8G8_UINT, 2, 1);
        case 0xEE: return C(VK_FORMAT_R16_UNORM, 2, 0);
        case 0xEF: return C(VK_FORMAT_R16_SNORM, 2, 0);
        case 0xF0: return C(VK_FORMAT_R16_SINT, 2, 2);
        case 0xF1: return C(VK_FORMAT_R16_UINT, 2, 1);
        case 0xF2: return C(VK_FORMAT_R16_SFLOAT, 2, 0);
        case 0xF3: return C(VK_FORMAT_R8_UNORM, 1, 0);
        case 0xF4: return C(VK_FORMAT_R8_SNORM, 1, 0);
        case 0xF5: return C(VK_FORMAT_R8_SINT, 1, 2);
        case 0xF6: return C(VK_FORMAT_R8_UINT, 1, 1);
    }
    VkFmt r = {0};
    return r;
}
VkFmt vk_rt_format(u32 f) {
    static u32 said[8];
    VkFmt r = rt_format(f);
    if (!r.bpp) {
        if (first_time(said, f)) WARN("vulkan: unsupported render target format %x, drawn as RGBA8\n", f);
    } else if (!has_features(r.vk, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        /* some are optional in Vulkan (the snorm ones, 5551) */
        if (first_time(said, f))
            WARN("vulkan: the GPU can't render to format %d (target format %x), drawn as RGBA8\n", (int)r.vk, f);
    } else
        return r;
    r = C(VK_FORMAT_R8G8B8A8_UNORM, 4, 0);
    r.raw = 0;
    return r;
}

static VkFmt D(VkFormat f, int bpp, int stencil) {
    VkFmt r = {f, (u8)bpp, 1, 1, 1, (u8)stencil, 0, 0};
    return r;
}
VkFmt vk_zt_format(u32 f) {
    switch (f) {
        case 0x0A: return D(VK_FORMAT_D32_SFLOAT, 4, 0);
        case 0x13: return D(VK_FORMAT_D16_UNORM, 2, 0);
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17: /* S8: only the stencil is used */
        case 0x18:
            /* D24S8 isn't supported everywhere (AMD), D32F S8 holds the same values */
            return has_features(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                       ? D(VK_FORMAT_D24_UNORM_S8_UINT, 4, 1)
                       : D(VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1);
        case 0x19: return D(VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1);
    }
    static u32 said[8];
    if (first_time(said, f)) WARN("vulkan: unsupported depth format %x, using D32F\n", f);
    return D(VK_FORMAT_D32_SFLOAT, 4, 0);
}

/* textures: the component type of the first component (bits 7-9) picks float, snorm, unorm or integer */
static VkFmt T(VkFormat f, int bpp, int is_int, int sint) {
    VkFmt r = {f, (u8)bpp, 1, 1, 0, 0, (u8)is_int, 1, (u8)sint};
    return r;
}
static VkFmt TC(VkFormat f, int bpp) { /* block compressed, 4x4 */
    VkFmt r = {f, (u8)bpp, 4, 4, 0, 0, 0, 1};
    return r;
}
static VkFmt TA(int bw, int bh, int srgb) { /* ASTC, decoded on the CPU: few desktop GPUs sample it */
    VkFmt r = {srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 16, (u8)bw, (u8)bh, 0, 0, 0, 0};
    r.conv = VKC_ASTC;
    return r;
}
static VkFmt TV(VkFormat f, int bpp, int conv) { /* converted on upload */
    VkFmt r = {f, (u8)bpp, 1, 1, 0, 0, 0, 0};
    r.conv = (u8)conv;
    return r;
}
VkFmt vk_tic_format(u32 w0, int srgb) {
    u32 f = w0 & 0x7F, ct = (w0 >> 7) & 7;
    int flt = ct == 7, sn = ct == 1, ui = ct == 4, si = ct == 3;
#define PICK(F_, SN_, UI_, SI_, UN_, bpp_)                                                                             \
    return flt  ? T(F_, bpp_, 0, 0)                                                                                    \
           : sn ? T(SN_, bpp_, 0, 0)                                                                                   \
           : ui ? T(UI_, bpp_, 1, 0)                                                                                   \
           : si ? T(SI_, bpp_, 1, 1)                                                                                   \
                : T(UN_, bpp_, 0, 0)
    switch (f) {
        case 0x01:
            return flt  ? T(VK_FORMAT_R32G32B32A32_SFLOAT, 16, 0, 0)
                   : ui ? T(VK_FORMAT_R32G32B32A32_UINT, 16, 1, 0)
                        : T(VK_FORMAT_R32G32B32A32_SINT, 16, 1, 1);
        case 0x02: return TV(VK_FORMAT_R32G32B32A32_SFLOAT, 12, VKC_RGB32F);
        case 0x03:
            PICK(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
                 VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_UNORM, 8);
        case 0x04:
            return flt  ? T(VK_FORMAT_R32G32_SFLOAT, 8, 0, 0)
                   : ui ? T(VK_FORMAT_R32G32_UINT, 8, 1, 0)
                        : T(VK_FORMAT_R32G32_SINT, 8, 1, 1);
        case 0x07:
        case 0x08:
            if (!flt && !sn && !ui && !si) return T(srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 4, 0, 0);
            PICK(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
                 VK_FORMAT_R8G8B8A8_UNORM, 4);
        case 0x09:
            return ui ? T(VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, 1, 0) : T(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 0, 0);
        case 0x0C:
            PICK(VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
                 VK_FORMAT_R16G16_UNORM, 4);
        case 0x0F:
            return flt  ? T(VK_FORMAT_R32_SFLOAT, 4, 0, 0)
                   : ui ? T(VK_FORMAT_R32_UINT, 4, 1, 0)
                        : T(VK_FORMAT_R32_SINT, 4, 1, 1);
        case 0x12: return TV(VK_FORMAT_R8G8B8A8_UNORM, 2, VKC_RGBA4); /* R in the low bits */
        case 0x13: return TV(VK_FORMAT_R8G8B8A8_UNORM, 2, VKC_A5B5G5R1);
        case 0x14: return TV(VK_FORMAT_R8G8B8A8_UNORM, 2, VKC_RGB5A1);
        case 0x15: return T(VK_FORMAT_B5G6R5_UNORM_PACK16, 2, 0, 0); /* R in the low bits */
        case 0x18:
            PICK(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
                 VK_FORMAT_R8G8_UNORM, 2);
        case 0x1B:
            PICK(VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R16_UNORM,
                 2);
        case 0x1C:
        case 0x1D:
            PICK(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R8_UNORM, 1);
        case 0x20: return T(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, 0, 0);
        case 0x21: return T(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 0, 0);
        case 0x24: return TC(srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8);
        case 0x25: return TC(srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK, 16);
        case 0x26: return TC(srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK, 16);
        case 0x27: return TC(sn ? VK_FORMAT_BC4_SNORM_BLOCK : VK_FORMAT_BC4_UNORM_BLOCK, 8);
        case 0x28: return TC(sn ? VK_FORMAT_BC5_SNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK, 16);
        case 0x17: return TC(srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, 16);
        case 0x10: return TC(VK_FORMAT_BC6H_SFLOAT_BLOCK, 16);
        case 0x11: return TC(VK_FORMAT_BC6H_UFLOAT_BLOCK, 16);
        case 0x40: return TA(4, 4, srgb);
        case 0x50: return TA(5, 4, srgb);
        case 0x41: return TA(5, 5, srgb);
        case 0x51: return TA(6, 5, srgb);
        case 0x42: return TA(6, 6, srgb);
        case 0x55: return TA(8, 5, srgb);
        case 0x52: return TA(8, 6, srgb);
        case 0x44: return TA(8, 8, srgb);
        case 0x56: return TA(10, 5, srgb);
        case 0x57: return TA(10, 6, srgb);
        case 0x53: return TA(10, 8, srgb);
        case 0x45: return TA(10, 10, srgb);
        case 0x54: return TA(12, 10, srgb);
        case 0x46: return TA(12, 12, srgb);
        /* depth: sampled from render targets. From memory they aren't loaded yet (their packing differs from
           Vulkan's buffer layout for depth copies) */
        case 0x29:
        case 0x2A:
        case 0x2B: {
            VkFmt r = vk_zt_format(0x14);
            r.raw = 0;
            return r;
        }
        case 0x2F: return vk_zt_format(0x0A);
        case 0x30: return vk_zt_format(0x19); /* Zf32X24S8 (0x19 itself is EAC, not supported) */
        case 0x3A: return vk_zt_format(0x13);
    }
#undef PICK
    static u32 said[8];
    if (first_time(said, f)) WARN("vulkan: unsupported texture format %x (not drawn)\n", f);
    VkFmt r = {0};
    return r;
}

VkFormat vk_srgb_as(VkFormat f, int srgb) {
    static const VkFormat pairs[][2] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB},
                                        {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB},
                                        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
                                        {VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK},
                                        {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK},
                                        {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK}};
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        if (pairs[i][0] == f || pairs[i][1] == f) return pairs[i][srgb ? 1 : 0];
    return f;
}

void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf) {
    vf->f[0] = vk_srgb_as(ii->format, 0);
    vf->f[1] = vk_srgb_as(ii->format, 1);
    if (vf->f[0] == vf->f[1]) return; /* one format: no MUTABLE_FORMAT, which costs compression on some GPUs */
    vf->list = (VkImageFormatListCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                             .pNext = ii->pNext,
                                             .viewFormatCount = 2,
                                             .pViewFormats = vf->f};
    ii->pNext = &vf->list;
    ii->flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
}
