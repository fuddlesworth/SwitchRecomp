/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* guest texture and render target formats -> GL formats */
#include "gpu/gl/gl.h"

/* GLES: anything the hw can't sample gets decoded on the CPU */
int g_gles;
int g_has_astc, g_has_s3tc, g_has_rgtc, g_has_bptc;
static GlFmt F(GLenum i, GLenum f, GLenum t, int bpp) {
    GlFmt r = {i, f, t, (u8)bpp, 1, 1, 0, 0, 0, 0, 0};
    r.is_int = (f == GL_RGBA_INTEGER || f == GL_RG_INTEGER || f == GL_RED_INTEGER);
    return r;
}
static GlFmt FC(GLenum i, int bpp) {
    GlFmt r = {i, 0, 0, (u8)bpp, 4, 4, 1, 0, 0, 0, 0};
    return r;
}
static GlFmt FASTC(int bw, int bh) {
    GlFmt r = {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 16, (u8)bw, (u8)bh, 1, 0, 0, 1, 0};
    return r;
}
static GlFmt FD(GLenum i, GLenum f, GLenum t, int bpp) {
    GlFmt r = {i, f, t, (u8)bpp, 1, 1, 0, 1, 0, 0, 0};
    return r;
}
/* TIC: format, type of the first component, sRGB */
GlFmt tic_format(u32 w0, int srgb) {
    u32 f = w0 & 0x7F, ct = (w0 >> 7) & 7;
    int flt = ct == 7, sn = ct == 1, ui = ct == 4, si = ct == 3;
    GlFmt r;
    switch (f) {
        case 0x01:
            r = flt  ? F(GL_RGBA32F, GL_RGBA, GL_FLOAT, 16)
                : ui ? F(GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 16)
                     : F(GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, 16);
            break;
        case 0x02: r = F(GL_RGB32F, GL_RGB, GL_FLOAT, 12); break;
        case 0x03:
            r = flt  ? F(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8)
                : sn ? F(GL_RGBA16_SNORM, GL_RGBA, GL_SHORT, 8)
                : ui ? F(GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, 8)
                : si ? F(GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT, 8)
                     : F(GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 8);
            break;
        case 0x04:
            r = flt  ? F(GL_RG32F, GL_RG, GL_FLOAT, 8)
                : ui ? F(GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 8)
                     : F(GL_RG32I, GL_RG_INTEGER, GL_INT, 8);
            break;
        case 0x07:
        case 0x08:
            r = sn   ? F(GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, 4)
                : ui ? F(GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 4)
                : si ? F(GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE, 4)
                     : F(srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
            break;
        case 0x09:
            r = ui ? F(GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV, 4)
                   : F(GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4);
            break;
        case 0x0C:
            r = flt  ? F(GL_RG16F, GL_RG, GL_HALF_FLOAT, 4)
                : sn ? F(GL_RG16_SNORM, GL_RG, GL_SHORT, 4)
                : ui ? F(GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT, 4)
                : si ? F(GL_RG16I, GL_RG_INTEGER, GL_SHORT, 4)
                     : F(GL_RG16, GL_RG, GL_UNSIGNED_SHORT, 4);
            break;
        case 0x0F:
            r = flt  ? F(GL_R32F, GL_RED, GL_FLOAT, 4)
                : ui ? F(GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 4)
                     : F(GL_R32I, GL_RED_INTEGER, GL_INT, 4);
            break;
        case 0x12: r = F(GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2); break;
        case 0x13:
        case 0x14: r = F(GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2); break;
        case 0x15: r = F(GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, 2); break;
        case 0x16: r = F(GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2); break;
        case 0x18:
            r = sn   ? F(GL_RG8_SNORM, GL_RG, GL_BYTE, 2)
                : ui ? F(GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, 2)
                : si ? F(GL_RG8I, GL_RG_INTEGER, GL_BYTE, 2)
                     : F(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2);
            break;
        case 0x1B:
            r = flt  ? F(GL_R16F, GL_RED, GL_HALF_FLOAT, 2)
                : sn ? F(GL_R16_SNORM, GL_RED, GL_SHORT, 2)
                : ui ? F(GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 2)
                : si ? F(GL_R16I, GL_RED_INTEGER, GL_SHORT, 2)
                     : F(GL_R16, GL_RED, GL_UNSIGNED_SHORT, 2);
            break;
        case 0x1D:
        case 0x1C:
            r = sn   ? F(GL_R8_SNORM, GL_RED, GL_BYTE, 1)
                : ui ? F(GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 1)
                : si ? F(GL_R8I, GL_RED_INTEGER, GL_BYTE, 1)
                     : F(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1);
            break;
        case 0x20: r = F(GL_RGB9_E5, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, 4); break;
        case 0x21: r = F(GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, 4); break;
        case 0x24: r = FC(srgb ? 0x8C4D : 0x83F1, 8); break;
        case 0x25: r = FC(srgb ? 0x8C4E : 0x83F2, 16); break;
        case 0x26: r = FC(srgb ? 0x8C4F : 0x83F3, 16); break;
        case 0x27: r = FC(sn ? 0x8DBC : 0x8DBB, 8); break;
        case 0x28: r = FC(sn ? 0x8DBE : 0x8DBD, 16); break;
        case 0x17: r = FC(srgb ? 0x8E8D : 0x8E8C, 16); break;
        case 0x10: r = FC(0x8E8E, 16); break;
        case 0x11: r = FC(0x8E8F, 16); break;
        case 0x40: r = FASTC(4, 4); break;
        case 0x50: r = FASTC(5, 4); break;
        case 0x41: r = FASTC(5, 5); break;
        case 0x51: r = FASTC(6, 5); break;
        case 0x42: r = FASTC(6, 6); break;
        case 0x55: r = FASTC(8, 5); break;
        case 0x52: r = FASTC(8, 6); break;
        case 0x44: r = FASTC(8, 8); break;
        case 0x56: r = FASTC(10, 5); break;
        case 0x57: r = FASTC(10, 6); break;
        case 0x53: r = FASTC(10, 8); break;
        case 0x45: r = FASTC(10, 10); break;
        case 0x54: r = FASTC(12, 10); break;
        case 0x46: r = FASTC(12, 12); break;
        case 0x29:
        case 0x2A:
        case 0x2B: r = FD(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4); break;
        case 0x2F: r = FD(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4); break;
        case 0x30:
            r = FD(GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 8);
            break; /* ZF32_X24S8 */
        case 0x3A: r = FD(GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2); break;
        case 0x19: r = FD(GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 8); break;
        default:
            r = F(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
            r.bpp = 0;
            {
                static u32 said[4];
                int k;
                for (k = 0; k < 4 && said[k] != f + 1; k++);
                if (k == 4) {
                    for (k = 0; k < 4 && said[k]; k++);
                    if (k < 4) {
                        said[k] = f + 1;
                        WARN("gpu: unsupported texture format %x (ignored)\n", f);
                    }
                }
            }
            break;
    }
    if (r.astc && srgb) r.ifmt = GL_SRGB8_ALPHA8;
    if (g_gles) {
        int bc = f == 0x24                  ? 2
                 : f == 0x25                ? 3
                 : f == 0x26                ? 4
                 : f == 0x27                ? 5
                 : f == 0x28                ? 6
                 : f == 0x17                ? 7
                 : (f == 0x10 || f == 0x11) ? 8
                                            : 0;
        int native =
            (bc >= 2 && bc <= 4 && g_has_s3tc) || ((bc == 5 || bc == 6) && g_has_rgtc) || (bc >= 7 && g_has_bptc);
        if (bc && !native) {
            r.dec = (u8)bc;
            r.ifmt = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
            r.fmt = GL_RGBA;
            r.type = GL_UNSIGNED_BYTE;
        }
        /* GLES has no REV 16-bit packed formats, expand to RGBA8 */
        if (f == 0x12 || f == 0x13 || f == 0x14 || f == 0x15) {
            r.dec = f == 0x12 ? 10 : f == 0x15 ? 12 : 11;
            r.ifmt = GL_RGBA8;
            r.fmt = GL_RGBA;
            r.type = GL_UNSIGNED_BYTE;
        }
        /* native ASTC on Mali/Adreno, upload compressed with all mips */
        if (r.astc && g_has_astc) {
            static const u8 bwh[14][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                          {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
            for (int k = 0; k < 14; k++)
                if (bwh[k][0] == r.bw && bwh[k][1] == r.bh) {
                    r.ifmt = (srgb ? 0x93D0 : 0x93B0) + k;
                    r.astc = 0;
                    break;
                }
        }
    }
    /* desktop with hw ASTC (Intel Gen9-11 etc): upload compressed too */
    if (!g_gles && r.astc && g_has_astc && !getenv("SWR_ASTC_CPU")) {
        static const u8 bwh[14][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                      {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
        for (int k = 0; k < 14; k++)
            if (bwh[k][0] == r.bw && bwh[k][1] == r.bh) {
                r.ifmt = (srgb ? 0x93D0 : 0x93B0) + k;
                r.astc = 0;
                break;
            }
    }
    r.is_int = (r.fmt == GL_RGBA_INTEGER || r.fmt == GL_RG_INTEGER || r.fmt == GL_RED_INTEGER);
    return r;
}
/* SNORM color targets aren't required to be renderable in GL. If the driver gets them wrong (see
   gl_selftest) store them as half float; transfers still convert to/from the guest's snorm data */
int g_snorm_fb;
GlFmt rt_format(u32 f) {
    switch (f) {
        case 0xC0: return F(GL_RGBA32F, GL_RGBA, GL_FLOAT, 16);
        case 0xC2: return F(GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 16);
        case 0xC6: return F(GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 8);
        case 0xC7: return F(g_snorm_fb ? GL_RGBA16F : GL_RGBA16_SNORM, GL_RGBA, GL_SHORT, 8);
        case 0xCA:
        case 0xCE: return F(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8);
        case 0xCB: return F(GL_RG32F, GL_RG, GL_FLOAT, 8);
        case 0xCD: return F(GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 8);
        case 0xCF:
        case 0xD5: return F(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
        case 0xD0:
        case 0xD6: return F(GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
        case 0xD1: return F(GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4);
        case 0xD7: return F(g_snorm_fb ? GL_RGBA16F : GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, 4);
        case 0xD9: return F(GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 4);
        case 0xDA: return F(GL_RG16, GL_RG, GL_UNSIGNED_SHORT, 4);
        case 0xDB: return F(g_snorm_fb ? GL_RG16F : GL_RG16_SNORM, GL_RG, GL_SHORT, 4);
        case 0xDE: return F(GL_RG16F, GL_RG, GL_HALF_FLOAT, 4);
        case 0xE0: return F(GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, 4);
        case 0xE4: return F(GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 4);
        case 0xE5: return F(GL_R32F, GL_RED, GL_FLOAT, 4);
        case 0xE8: return F(GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2);
        case 0xE9: return F(GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2);
        case 0xEA: return F(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2);
        case 0xEB: return F(g_snorm_fb ? GL_RG16F : GL_RG8_SNORM, GL_RG, GL_BYTE, 2);
        case 0xEE: return F(GL_R16, GL_RED, GL_UNSIGNED_SHORT, 2);
        case 0xF1: return F(GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 2);
        case 0xF2: return F(GL_R16F, GL_RED, GL_HALF_FLOAT, 2);
        case 0xF3: return F(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1);
        case 0xF6: return F(GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 1);
        /* depth */
        case 0x0A: return FD(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4);
        case 0x13: return FD(GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2);
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x18: return FD(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4);
        case 0x19: return FD(GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 8);
    }
    WARN("gpu: unsupported render target format %x\n", f);
    return F(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4);
}
