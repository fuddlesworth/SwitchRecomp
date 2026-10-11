/* Guest render target formats (the 3D class's RT_FORMAT and ZETA_FORMAT values, as gpu_gl.c's rt_format reads
   them) -> Vulkan formats. raw marks formats whose guest bytes are exactly the Vulkan format's, so the render
   target can be copied to and from guest memory as is. */
#include "vk.h"

static VkFmt C(VkFormat f, int bpp, int is_int) {
    VkFmt r = {f, (u8)bpp, 1, 1, 0, 0, (u8)is_int, 1};
    return r;
}

VkFmt vk_rt_format(u32 f) {
    switch (f) {
        case 0xC0: return C(VK_FORMAT_R32G32B32A32_SFLOAT, 16, 0);
        case 0xC2: return C(VK_FORMAT_R32G32B32A32_UINT, 16, 1);
        case 0xC6: return C(VK_FORMAT_R16G16B16A16_UNORM, 8, 0);
        case 0xC7: return C(VK_FORMAT_R16G16B16A16_SNORM, 8, 0);
        case 0xCA:
        case 0xCE: return C(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 0);
        case 0xCB: return C(VK_FORMAT_R32G32_SFLOAT, 8, 0);
        case 0xCD: return C(VK_FORMAT_R32G32_UINT, 8, 1);
        case 0xCF:
        case 0xD5: return C(VK_FORMAT_R8G8B8A8_UNORM, 4, 0);
        case 0xD0:
        case 0xD6: return C(VK_FORMAT_R8G8B8A8_SRGB, 4, 0);
        case 0xD1: return C(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 0);
        case 0xD7: return C(VK_FORMAT_R8G8B8A8_SNORM, 4, 0);
        case 0xD9: return C(VK_FORMAT_R8G8B8A8_UINT, 4, 1);
        case 0xDA: return C(VK_FORMAT_R16G16_UNORM, 4, 0);
        case 0xDB: return C(VK_FORMAT_R16G16_SNORM, 4, 0);
        case 0xDE: return C(VK_FORMAT_R16G16_SFLOAT, 4, 0);
        case 0xE0: return C(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 0);
        case 0xE4: return C(VK_FORMAT_R32_UINT, 4, 1);
        case 0xE5: return C(VK_FORMAT_R32_SFLOAT, 4, 0);
        case 0xE8: return C(VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 0);
        case 0xE9: { /* R5G5B5A1 with red in the low bits: no core Vulkan 1.3 format stores it that way */
            VkFmt r = C(VK_FORMAT_R8G8B8A8_UNORM, 4, 0);
            r.raw = 0;
            return r;
        }
        case 0xEA: return C(VK_FORMAT_R8G8_UNORM, 2, 0);
        case 0xEB: return C(VK_FORMAT_R8G8_SNORM, 2, 0);
        case 0xEE: return C(VK_FORMAT_R16_UNORM, 2, 0);
        case 0xF1: return C(VK_FORMAT_R16_UINT, 2, 1);
        case 0xF2: return C(VK_FORMAT_R16_SFLOAT, 2, 0);
        case 0xF3: return C(VK_FORMAT_R8_UNORM, 1, 0);
        case 0xF6: return C(VK_FORMAT_R8_UINT, 1, 1);
    }
    static u32 said[8];
    for (int k = 0; k < 8; k++) {
        if (said[k] == f + 1) break;
        if (!said[k]) {
            said[k] = f + 1;
            WARN("vulkan: unsupported render target format %x, drawn as RGBA8\n", f);
            break;
        }
    }
    VkFmt r = C(VK_FORMAT_R8G8B8A8_UNORM, 4, 0);
    r.raw = 0;
    return r;
}

static int depth_supported(VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
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
        case 0x18:
            /* D24S8 isn't supported everywhere (AMD), D32F S8 holds the same values */
            return depth_supported(VK_FORMAT_D24_UNORM_S8_UINT) ? D(VK_FORMAT_D24_UNORM_S8_UINT, 4, 1)
                                                                : D(VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1);
        case 0x19: return D(VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1);
    }
    WARN("vulkan: unsupported depth format %x, using D32F\n", f);
    return D(VK_FORMAT_D32_SFLOAT, 4, 0);
}
