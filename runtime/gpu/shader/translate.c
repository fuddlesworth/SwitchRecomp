/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* translating each instruction to GLSL, with the attribute and texture helpers */
#include "gpu/shader/translator.h"
/* ============================================================ attributes */
static char* attr_read(Ctx* c, u32 addr, int is_output) {
    u32 comp = (addr >> 2) & 3;
    if (addr >= 0x80 && addr < 0x280) {
        u32 i = (addr - 0x80) >> 4;
        if (is_output) {
            c->out_used[i] = 1;
            return T("floatBitsToUint(out_attr%u.%c)", i, "xyzw"[comp]);
        }
        c->in_used[i] = 1;
        if (c->stage == ST_VS && (c->si->attr_int_mask >> i) & 1) return T("uint(in_attr%u.%c)", i, "xyzw"[comp]);
        return T("floatBitsToUint(in_attr%u.%c)", i, "xyzw"[comp]);
    }
    if (addr >= 0x70 && addr < 0x80) {
        if (c->stage == ST_FS) {
            c->si->uses_position_in = 1;
            if (comp == 3) return "floatBitsToUint(1.0/gl_FragCoord.w)";
            if (comp < 2) return T("floatBitsToUint(gl_FragCoord.%c*snx_rs.x)", "xy"[comp]);
            return T("floatBitsToUint(gl_FragCoord.%c)", "xyzw"[comp]);
        }
        /* only vertex, fragment and compute programs get translated (no geometry or tessellation stage) */
        return c->stage == ST_VS ? T("floatBitsToUint(gl_Position.%c)", "xyzw"[comp]) : "0u";
    }
    switch (addr) {
        /* Vulkan's instance index counts from the draw's first instance, the guest's instance ID from 0 */
        case 0x2FC: return c->stage != ST_VS ? "0u" : "uint(gl_VertexIndex)";
        case 0x2F8:
            if (c->stage != ST_VS) return "0u";
            c->uses_draw_params = 1;
            return "uint(gl_InstanceIndex - gl_BaseInstanceARB)";
        case 0x3FC: return c->stage == ST_FS ? "(gl_FrontFacing? 0xFFFFFFFFu : 0u)" : "0u";
        case 0x60: return c->stage == ST_FS ? "uint(gl_PrimitiveID)" : "0u";
        case 0x64: return c->stage == ST_FS ? "uint(gl_Layer)" : "0u";
        case 0x68: return "0u";
        case 0x6C: return "floatBitsToUint(1.0)";
        case 0x2E0:
        case 0x2E4: return c->stage == ST_FS ? T("floatBitsToUint(gl_PointCoord.%c)", "xy"[(addr >> 2) & 1]) : "0u";
    }
    if (addr >= 0x2C0 && addr < 0x2E0) return "0u";
    return "0u";
}
static void attr_write(Ctx* c, u32 addr, const char* val) {
    u32 comp = (addr >> 2) & 3;
    if (addr >= 0x80 && addr < 0x280) {
        u32 i = (addr - 0x80) >> 4;
        c->out_used[i] = 1;
        emitl(c, "out_attr%u.%c = uintBitsToFloat(%s);", i, "xyzw"[comp], val);
        return;
    }
    if (addr >= 0x70 && addr < 0x80) {
        emitl(c, "gl_Position.%c = uintBitsToFloat(%s);", "xyzw"[comp], val);
        return;
    }
    switch (addr) {
        case 0x6C:
            c->si->writes_point_size = 1;
            emitl(c, "gl_PointSize = uintBitsToFloat(%s);", val);
            return;
        case 0x64:
            c->si->writes_layer = 1;
            emitl(c, "SNX_SET_LAYER(int(%s));", val);
            return;
        case 0x68: emitl(c, "/* viewport index */"); return;
    }
    if (addr >= 0x2C0 && addr < 0x2E0) {
        /* a distance the 3D engine doesn't enable doesn't clip, and Vulkan clips with every one written */
        u32 i = (addr - 0x2C0) >> 2;
        if (!((c->si->clip_enable >> i) & 1)) {
            emitl(c, "/* clip distance %u not enabled */", i);
            return;
        }
        emitl(c, "gl_ClipDistance[%u] = uintBitsToFloat(%s);", i, val);
        c->si->uses_clip_mask |= 1 << i;
        return;
    }
    emitl(c, "/* write to attribute %x ignored */", addr);
}

/* ============================================================ textures */
static void unsupported(Ctx* c, const char* name, u64 w);
static int tex_slot2(Ctx* c, u32 cbuf_off_words, int type, int array, int shadow, int bl, u32 bank);
static int tex_slot(Ctx* c, u32 cbuf_off_words, int type, int array, int shadow) {
    return tex_slot2(c, cbuf_off_words, type, array, shadow, 0, 0);
}
static int tex_slot2(Ctx* c, u32 cbuf_off_words, int type, int array, int shadow, int bl, u32 bank) {
    ShaderInfo* si = c->si;
    u8 b2 = bl ? c->bl2 : 0;
    for (int i = 0; i < si->ntex; i++) {
        ShTex* t = &si->tex[i];
        if (t->cbuf_off == cbuf_off_words && t->type == type && t->array == array && t->shadow == shadow &&
            t->bindless == bl && (!bl || t->bl_cbuf == bank) && t->bl2 == b2 &&
            (!b2 || (t->bl2_cbuf == c->bl2_bank && t->bl2_off == c->bl2_off)))
            return i;
    }
    if (si->ntex >= SH_MAXTEX) { /* no binding left: the access reads slot 0, so say so */
        unsupported(c, "texture (too many textures)", 0);
        return 0;
    }
    ShTex* t = &si->tex[si->ntex];
    memset(t, 0, sizeof *t);
    t->cbuf_off = cbuf_off_words;
    t->type = (u8)type;
    t->array = (u8)array;
    t->shadow = (u8)shadow;
    t->bindless = (u8)bl;
    t->bl_cbuf = (u8)bank;
    if (bl && bank < 18) {
        si->cbuf_mask |= 1u << bank;
        if (cbuf_off_words * 4 + 16 > si->cbuf_max[bank]) si->cbuf_max[bank] = ((cbuf_off_words * 4) | 15) + 1;
    }
    if (b2) {
        t->bl2 = 1;
        t->bl2_cbuf = (u8)c->bl2_bank;
        t->bl2_off = c->bl2_off;
        u32 b = c->bl2_bank;
        if (b < 18) {
            si->cbuf_mask |= 1u << b;
            if (c->bl2_off * 4 + 16 > si->cbuf_max[b]) si->cbuf_max[b] = ((c->bl2_off * 4) | 15) + 1;
        }
    }
    return si->ntex++;
}
/* bits 0-7 aren't a destination register for these (stores keep their data register there) */
static int no_rd(const Op* o) {
    const char* nm = o->name;
    return strstr(nm, "SETP") || !strncmp(nm, "ST", 2) || !strncmp(nm, "BR", 2) || !strcmp(nm, "EXIT") ||
           !strcmp(nm, "SSY") || !strcmp(nm, "SYNC") || !strcmp(nm, "PBK") || !strcmp(nm, "PCNT") ||
           !strcmp(nm, "CONT") || !strcmp(nm, "KIL") || !strcmp(nm, "NOP") || !strcmp(nm, "DEPBAR") ||
           !strcmp(nm, "BAR") || !strcmp(nm, "AST") || !strcmp(nm, "RED") || !strcmp(nm, "MEMBAR") ||
           !strcmp(nm, "CAL") || !strcmp(nm, "RET");
}
/* TEX.B / TXQ.B: handle comes in a register. Find the LDC / MOV from cbuf that produced it and treat it
   as a regular bound texture from that bank. */
/* backtrack rb before idx. handles show up as a plain LDC, a MOV from cbuf, reg copies, or OR/ADD of two
   LDCs (separate tex + sampler, Unity does this). returns up to 2 (bank, byte offset) reads */
static int track_src(Ctx* c, u32 idx, u32 rb, u32* bank, u32* off, int* n, int depth) {
    if (rb == 255 || depth > 6) return 0;
    for (u32 k = 1; k <= 1024 && k < idx; k++) {
        u32 j = idx - k;
        if (!IDX_OK(j)) continue;
        u64 w = c->w[j];
        const Op* o = decode(w);
        if (!o) continue;
        if (B(w, 0, 8) != rb) continue;
        if (no_rd(o)) continue;
        if (o->id == O_LDC) {
            u32 sz = B(w, 48, 3);
            if (sz != 4 && sz != 5) return 0;
            if (*n >= 2) return 0;
            /* indexed handle (array of textures): just use element 0 */
            bank[*n] = B(w, 36, 5);
            off[*n] = (u32)((i32)(B(w, 20, 16) << 16) >> 16);
            if (off[*n] & 3) return 0;
            (*n)++;
            return 1;
        }
        if (o->id == O_MOV_C) {
            if (*n >= 2) return 0;
            bank[*n] = cb_idx(w);
            off[*n] = cb_off(w);
            (*n)++;
            return 1;
        }
        if (o->id == O_MOV_R) return track_src(c, j, B(w, 20, 8), bank, off, n, depth + 1);
        if (o->id == O_LOP_R || o->id == O_LOP3_R || o->id == O_IADD_R) {
            u32 ra = B(w, 8, 8), rbb = B(w, 20, 8);
            if (ra == 255) return track_src(c, j, rbb, bank, off, n, depth + 1);
            if (rbb == 255) return track_src(c, j, ra, bank, off, n, depth + 1);
            return track_src(c, j, ra, bank, off, n, depth + 1) && track_src(c, j, rbb, bank, off, n, depth + 1);
        }
        if (o->id == O_LOP32I || o->id == O_LOP_I || o->id == O_IADD32I || o->id == O_IADD_I || o->id == O_LOP_C ||
            o->id == O_IADD_C)
            return track_src(c, j, B(w, 8, 8), bank, off, n, depth + 1);
        return 0;
    }
    return 0;
}
static int track_handle(Ctx* c, u32 idx, u32 rb, u32* bank, u32* off) {
    u32 bk[2] = {0, 0}, of[2] = {0, 0};
    int n = 0;
    c->bl2 = 0;
    if (!track_src(c, idx, rb, bk, of, &n, 0) || !n) return 0;
    *bank = bk[0];
    *off = of[0];
    if (n == 2) {
        c->bl2 = 1;
        c->bl2_bank = bk[1];
        c->bl2_off = of[1] / 4;
    }
    return 1;
}
/* the constant in r at idx, when it was moved there in the same block (MOV32I, MOV with an immediate, or a copy of
   such a register). GLSL's sampling functions only take constant offsets, and a shader's source had them as
   constants, which its compiler puts in a register for TEX/TXD/TLD4.PTP. 0 if it can't tell */
static int track_const(Ctx* c, u32 idx, u32 r, u32* v, int depth) {
    if (r == 255) {
        *v = 0;
        return 1;
    }
    if (depth > 6) return 0;
    for (u32 j = idx; j-- > 11;) {
        if ((c->leader[(j + 1) >> 3] >> ((j + 1) & 7)) & 1) return 0; /* another path can come in after j */
        if (!IDX_OK(j)) continue;
        u64 w = c->w[j];
        const Op* o = decode(w);
        if (!o || B(w, 0, 8) != r || no_rd(o)) continue;
        if (o->id == O_MOV32I) {
            *v = B(w, 20, 32);
            return 1;
        }
        if (o->id == O_MOV_I) { /* imm20 with its sign at bit 56 */
            *v = B(w, 20, 19) | (B(w, 56, 1) ? 0xFFF80000u : 0);
            return 1;
        }
        if (o->id == O_MOV_R) return track_const(c, j, B(w, 20, 8), v, depth + 1);
        return 0;
    }
    return 0;
}
/* the offsets packed in a register as an ivecN (n components of bits each, component k at shift + k*stride, signed),
   a constant expression when track_const knows the value; pad1d adds the 0 for a 1D texture read as 2D */
static const char* tex_offsets(Ctx* c, u32 idx, u32 r, int n, int shift, int stride, int bits, int pad1d,
                               int* is_const) {
    u32 v;
    char k[3][48];
    *is_const = track_const(c, idx, r, &v, 0);
    for (int i = 0; i < n; i++) {
        int lo = shift + i * stride;
        if (*is_const)
            snprintf(k[i], sizeof k[i], "%d", (i32)(v << (32 - lo - bits)) >> (32 - bits));
        else
            snprintf(k[i], sizeof k[i], "bitfieldExtract(int(%s),%d,%d)", R(c, r), lo, bits);
    }
    if (n == 1) return pad1d ? T("ivec2(%s, 0)", k[0]) : T("%s", k[0]);
    if (n == 2) return T("ivec2(%s, %s)", k[0], k[1]);
    return T("ivec3(%s, %s, %s)", k[0], k[1], k[2]);
}
/* the Dim (Ryujinx TexDim, as TEX bits 28-30 hold it) the program's other accesses use for the bound texture at
   word off, -1 when none does. TEXS/TLDS give theirs through their encoding */
static int tex_dim_of(Ctx* c, u32 off) {
    /* TEXS and TLDS encodings to TexDim: 1D 0, 2D 2, 2D array 3, 3D 4, cube 6 */
    static const i8 texs[16] = {0, 2, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 6, 6, -1, -1};
    static const i8 tlds[16] = {0, 0, 2, 2, 2, 2, 2, 4, 3, -1, -1, -1, 2, -1, -1, -1};
    for (u32 i = 11; i < c->nwords; i++) {
        if (!IDX_OK(i)) continue;
        u64 w = c->w[i];
        const Op* o = decode(w);
        if (!o || B(w, 36, 13) != off) continue;
        int id = o->id;
        if (id == O_TEX || id == O_TLD || id == O_TLD4 || id == O_TMML || id == O_TXD) return (int)B(w, 28, 3);
        if (id == O_TEXS && texs[B(w, 53, 4)] >= 0) return texs[B(w, 53, 4)];
        if (id == O_TLDS && tlds[B(w, 53, 4)] >= 0) return tlds[B(w, 53, 4)];
        if (id == O_TLD4S) return 2;
    }
    return -1;
}
static const char* IW(Ctx* c, int s, const char* smp) {
    if (s < 0 || s >= 32 || !((c->si->tex_int_mask >> s) & 1)) return smp;
    return T("uintBitsToFloat(uvec4(%s))", smp);
}
const char* samp_type(int type, int array, int shadow) {
    /* Vulkan can't view the 2D images 1D textures are stored in as 1D, so they are read as 2D */
    static const char* n[4] = {"1D", "2D", "3D", "Cube"};
    if (type == 0) type = 1;
    return T("sampler%s%s%s", n[type & 3], array ? "Array" : "", shadow ? "Shadow" : "");
}
/* store masked components of vec4 v into d, d+1, ... */
static void write_masked(Ctx* c, u32 d, u32 mask, const char* v) {
    u32 r = d;
    for (int k = 0; k < 4; k++)
        if (mask & (1u << k)) {
            if (r != 255) setrf(c, r, T("%s.%c", v, "xyzw"[k]));
            r = r == 255 ? 255 : r + 1;
        }
}

/* SULD/SUST coordinates for Dim 33-35 (Ryujinx SuDim: 0 1D, 2 1D array, 3 2D, 4 2D array, 5 3D) from ra on, with x
   given: for an image as declared, for a sampler (sampled) with 1D read as 2D like everywhere else */
static const char* su_coords(Ctx* c, u32 ty, u32 ra, const char* x, int sampled) {
    switch (ty) {
        case 0: return sampled ? T("ivec2(%s, 0)", x) : x;
        case 2: return sampled ? T("ivec3(%s, 0, int(%s))", x, R(c, ra + 1)) : T("ivec2(%s, int(%s))", x, R(c, ra + 1));
        case 3: return T("ivec2(%s, int(%s))", x, R(c, ra + 1));
        default: return T("ivec3(%s, int(%s), int(%s))", x, R(c, ra + 1), R(c, ra + 2));
    }
}

/* TEXS/TLDS/TLD4S write two reg pairs */
static void write_texs(Ctx* c, u64 w, const char* v, int fp16) {
    u32 da = B(w, 0, 8), db = B(w, 28, 8), sw = B(w, 50, 3);
    int comps[4];
    int n = 0;
    if (db == 255) {
        static const int tab[8][2] = {{0, -1}, {1, -1}, {2, -1}, {3, -1}, {0, 1}, {0, 3}, {1, 3}, {2, 3}};
        for (int k = 0; k < 2; k++)
            if (tab[sw][k] >= 0) comps[n++] = tab[sw][k];
    } else {
        static const int tab[5][4] = {{0, 1, 2, -1}, {0, 1, 3, -1}, {0, 2, 3, -1}, {1, 2, 3, -1}, {0, 1, 2, 3}};
        if (sw > 4) sw = 4;
        for (int k = 0; k < 4; k++)
            if (tab[sw][k] >= 0) comps[n++] = tab[sw][k];
    }
    if (fp16) {
        if (n > 0)
            setr(c, da,
                 n > 1 ? T("packHalf2x16(vec2(%s.%c,%s.%c))", v, "xyzw"[comps[0]], v, "xyzw"[comps[1]])
                       : T("packHalf2x16(vec2(%s.%c,0.0))", v, "xyzw"[comps[0]]));
        if (n > 2)
            setr(c, db,
                 n > 3 ? T("packHalf2x16(vec2(%s.%c,%s.%c))", v, "xyzw"[comps[2]], v, "xyzw"[comps[3]])
                       : T("packHalf2x16(vec2(%s.%c,0.0))", v, "xyzw"[comps[2]]));
        return;
    }
    for (int k = 0; k < n; k++) {
        u32 r = k < 2 ? da + k : db + (k - 2);
        if (k < 2 && da == 255) continue;
        if (k >= 2 && db == 255) continue;
        setrf(c, r, T("%s.%c", v, "xyzw"[comps[k]]));
    }
}

/* ============================================================ per-instruction translation */
u32 bra_target(u64 w, u32 idx) {
    i32 off = (i32)(B(w, 20, 24) << 8) >> 8;
    u32 t = (u32)((i32)idx + 1 + off / 8);
    if (t >= 10 && (t - 10) % 4 == 0) t++; /* sched control word, skip */
    return t;
}
static void unsupported(Ctx* c, const char* name, u64 w) {
    c->si->unsupported++;
    if (!c->si->first_unsupported[0]) snprintf(c->si->first_unsupported, 64, "%s %016llx", name, (unsigned long long)w);
    emitl(c, "/* untranslated: %s */", name);
}

static void inst(Ctx* c, u32 idx, u64 w, const Op* op);

/* RED and ATOM: an atomic on global memory, at register ra plus off, of the 32-bit value in rv (size 0 u32, 1 s32,
   3 f32), returning the old value to dst unless it is RZ. Operations: add min max inc dec and or xor, exch, safeadd */
static void global_atomic(Ctx* c, u64 w, const char* name, u32 ra, u32 rv, i32 off, u32 sz, u32 op, u32 dst) {
    if (op == 10) op = 0; /* SAFEADD */
    if (!c->gtag[ra] || (sz != 0 && sz != 1 && sz != 3) || op > 8 || (sz == 3 && op != 0 && op != 8)) {
        unsupported(c, name, w);
        if (dst != 255) setr(c, dst, "0u");
        return;
    }
    int gi = c->gtag[ra] - 1;
    c->si->gbuf_wmask |= 1u << gi;
    emitl(c, "{ uint o_ = (%s + %uu - %s) >> 2u; uint v_ = %s; uint old_;", R(c, ra), (u32)off,
          CB(c, 0, c->si->gbuf_cbuf_off[gi]), R(c, rv));
    c->indent++;
    static const char* direct[9] = {"atomicAdd",     NULL, NULL, NULL, NULL, "atomicAnd", "atomicOr", "atomicXor",
                                    "atomicExchange"};
    if (sz != 3 ? direct[op] != NULL : op == 8)
        emitl(c, "old_ = %s(g%d.d[o_], v_);", direct[op], gi);
    else if (sz == 0 && (op == 1 || op == 2))
        emitl(c, "old_ = %s(g%d.d[o_], v_);", op == 1 ? "atomicMin" : "atomicMax", gi);
    else {
        /* signed min/max, inc, dec and f32 add: a compare-and-swap loop */
        const char* nv = sz == 3   ? "floatBitsToUint(uintBitsToFloat(old_) + uintBitsToFloat(v_))"
                         : op == 1 ? "uint(min(int(old_), int(v_)))"
                         : op == 2 ? "uint(max(int(old_), int(v_)))"
                         : op == 3 ? "(old_ >= v_ ? 0u : old_ + 1u)"
                                   : "((old_ == 0u || old_ > v_) ? v_ : old_ - 1u)";
        emitl(c, "old_ = g%d.d[o_];", gi);
        emitl(c, "for (;;) { uint p_ = atomicCompSwap(g%d.d[o_], old_, %s); if (p_ == old_) break; old_ = p_; }", gi,
              nv);
    }
    if (dst != 255) {
        emitl(c, "r%u = old_;", dst);
        c->used_reg[dst] = 1;
    }
    c->indent--;
    emitl(c, "}");
}

/* the condition code test of BRA/BRX/SYNC/BRK/CONT/EXIT/KIL/RET and CSETP (Ryujinx InstBra.Ccc, the Ccc enum and
   InstEmitConditionCode.GetCondition): N is cc_s, Z cc_z, C cc_c, V cc_o. NULL for the codes it doesn't model */
static const char* cc_test(Ctx* c, u32 ccc) {
    if (ccc == 15) return "true";
    if (ccc == 0) return "false";
    c->uses_cc = 1;
    switch (ccc) {
        case 1: return "((cc_s && !cc_z) != cc_o)";    /* LT */
        case 2: return "(!cc_s && cc_z)";              /* EQ */
        case 3: return "(cc_s != (cc_z || cc_o))";     /* LE */
        case 4: return "(!((cc_s != cc_o) || cc_z))";  /* GT */
        case 5: return "(!cc_z)";                      /* NE */
        case 6: return "(cc_s == cc_o)";               /* GE */
        case 7: return "(!(cc_s && cc_z))";            /* NUM */
        case 8: return "(cc_s && cc_z)";               /* NAN */
        case 9: return "(cc_s != cc_o)";               /* LTU */
        case 10: return "cc_z";                        /* EQU */
        case 11: return "((cc_s != cc_o) || cc_z)";    /* LEU */
        case 12: return "((!cc_s) != (cc_o || cc_z))"; /* GTU */
        case 13: return "(cc_s || !cc_z)";             /* NEU */
        case 14: return "(((!cc_s) || cc_z) != cc_o)"; /* GEU */
        case 16: return "(!cc_o)";                     /* OFF */
        case 17: return "(!cc_c)";                     /* LO */
        case 18: return "(!cc_s)";                     /* SFF */
        case 19: return "(cc_z || !cc_c)";             /* LS */
        case 20: return "(cc_c && !cc_z)";             /* HI */
        case 21: return "cc_s";                        /* SFT */
        case 22: return "cc_c";                        /* HS */
        case 23: return "cc_o";                        /* OFT */
        case 30: return "(cc_s || cc_z)";              /* RLE */
        case 31: return "(!(cc_s || cc_z))";           /* RGT */
    }
    return NULL; /* CSM_* / FCSM_*: states of the hardware's own, not flags */
}
static int has_ccc(int id) {
    return id == O_BRA || id == O_BRX || id == O_SYNC || id == O_BRK || id == O_CONT || id == O_EXIT || id == O_KIL ||
           id == O_RET;
}
/* a flow instruction that always runs: predicate PT and condition code T */
int flow_always(u64 w) { return B(w, 16, 3) == 7 && !B(w, 19, 1) && B(w, 0, 5) == 15; }
/* when a flow instruction runs: its predicate and its condition code */
const char* flow_cond(Ctx* c, u64 w) {
    u32 pg = B(w, 16, 3), pn = B(w, 19, 1);
    const char* p = pg == 7 ? (pn ? "false" : "true") : PN(c, pg, pn);
    const char* t = cc_test(c, B(w, 0, 5));
    if (!t) { /* as in Ryujinx, an EXIT on a condition it can't test doesn't run, the others do */
        const Op* op = decode(w);
        unsupported(c, "condition code", w);
        t = op && op->id == O_EXIT ? "false" : "true";
    }
    if (!strcmp(t, "true")) return p;
    if (!strcmp(p, "true")) return t;
    return T("(%s && %s)", p, t);
}

/* d = e, and with WriteCC the Z and N flags of the result (Ryujinx SetZnFlags, SetFPZnFlags for a float one): a
   branch or CSETP on a condition code reads them. Through a temporary, since d can be RZ (FSET RZ.CC) */
static void setr_cc(Ctx* c, u32 d, const char* e, int cc, int fp) {
    if (!cc) {
        setr(c, d, e);
        return;
    }
    c->uses_cc = 1;
    emitl(c, "{ uint v_ = %s;", e);
    c->indent++;
    setr(c, d, "v_");
    emitl(c, fp ? "cc_z = uintBitsToFloat(v_) == 0.0; cc_s = uintBitsToFloat(v_) < 0.0;"
                : "cc_z = v_ == 0u; cc_s = int(v_) < 0;");
    c->indent--;
    emitl(c, "}");
}
static void setrf_cc(Ctx* c, u32 d, const char* e, int cc) { setr_cc(c, d, T("floatBitsToUint(%s)", e), cc, 1); }

/* IADD/IADD32I: AvgMode (Ryujinx AvgMode) 1 negates B, 2 negates A, 3 adds one. .X adds the carry in, .CC sets the
   flags (Ryujinx SetIaddFlags: C the carry out, V the signed overflow, and after .X Z only stays set if the earlier
   words were zero too). Under .CC or .X a negated operand goes through the adder as ~B + 1, so C comes out as the
   subtraction's no-borrow (also for B = 0, which 0u-B would lose), and under .X the carry in takes the place of the
   +1: a multiword subtraction (IADD.CC on the low words, IADD.X on the high ones) needs exactly that */
static void emit_iadd(Ctx* c, u32 d, const char* A, const char* Bv, u32 avg, int x, int cc) {
    if (!x && !cc) {
        if (avg == 2) A = T("(0u-%s)", A);
        if (avg == 1) Bv = T("(0u-%s)", Bv);
        setr(c, d, T("(%s + %s%s)", A, Bv, avg == 3 ? " + 1u" : ""));
        return;
    }
    if (avg == 2) A = T("(~%s)", A);
    if (avg == 1) Bv = T("(~%s)", Bv);
    const char* cin = x ? (avg == 3 ? "(cc_c?2u:1u)" : "(cc_c?1u:0u)") : avg ? "1u" : "0u";
    c->uses_cc = 1;
    emitl(c, "{ uint a_ = %s, b_ = %s, c1_, c2_ = 0u; uint s_ = uaddCarry(a_, b_, c1_);", A, Bv);
    c->indent++;
    if (strcmp(cin, "0u")) emitl(c, "s_ = uaddCarry(s_, %s, c2_);", cin);
    if (cc)
        emitl(
            c,
            "cc_c = (c1_|c2_)!=0u; cc_o = ((s_ ^ a_) & ~(a_ ^ b_)) >= 0x80000000u; cc_z = s_==0u%s; cc_s = int(s_)<0;",
            x ? " && cc_z" : "");
    setr(c, d, "s_");
    c->indent--;
    emitl(c, "}");
}

/* local memory per thread in words: the SPH's ShaderLocalMemoryLowSize + HighSize (common words 1 and 2, bits 0-23,
   as Ryujinx ShaderHeader reads them) rounded up to a power of two. Compute programs come without an SPH (zeros), so
   they, and a header that gives no size, get 1024 words */
u32 lmem_words(Ctx* c) {
    u32 bytes = (c->si->sph[1] & 0xFFFFFF) + (c->si->sph[2] & 0xFFFFFF), n = 1;
    if (c->stage == ST_CS || !bytes) return 1024;
    while (n * 4 < bytes && n < 65536) n <<= 1;
    return n;
}

/* opens a block in which h_ is the high word of a * b, each operand signed or not (Ryujinx EmitImad: a signed
   multiply when both are, else the unsigned one corrected for the signed operand); the caller closes it */
static void open_mulhi(Ctx* c, const char* a, const char* b, int sa, int sb) {
    emitl(c, "{ uint a_ = %s, b_ = %s, h_, l_;", a, b);
    c->indent++;
    if (sa && sb)
        emitl(c, "{ int hs_, ls_; imulExtended(int(a_), int(b_), hs_, ls_); h_ = uint(hs_); }");
    else {
        emitl(c, "umulExtended(a_, b_, h_, l_);");
        if (sa) emitl(c, "h_ += b_ * uint(int(a_) >> 31);");
        if (sb) emitl(c, "h_ += a_ * uint(int(b_) >> 31);");
    }
}

/* ISETP/ISET: IComp 49-51, Signed 48. With .X (bit 43) they compare the high words of a multiword value and the low
   words' subtraction left its result in the flags (Ryujinx GetIntComparisonExtended) */
static const char* icmp_xe(Ctx* c, u64 w, const char* a, const char* b) {
    u32 op = B(w, 49, 3);
    int sgn = B(w, 48, 1);
    if (!B(w, 43, 1) || op == 0 || op == 7) return icmp_expr(op, a, b, sgn);
    c->uses_cc = 1;
    const char *lt = icmp_expr(1, a, b, sgn), *gt = icmp_expr(4, a, b, sgn), *eq = icmp_expr(2, a, b, 0);
    switch (op) {
        case 1: return T("(%s || (%s && !cc_c))", lt, eq);
        case 2: return T("(%s && cc_z)", eq);
        case 3: return T("(%s || (%s && (cc_z || !cc_c)))", lt, eq);
        case 4: return T("(%s || (%s && !cc_z && cc_c))", gt, eq);
        case 5: return T("(%s || !cc_z)", icmp_expr(5, a, b, 0));
        default: return T("(%s || (%s && cc_c))", gt, eq);
    }
}

static void emit_guarded(Ctx* c, u32 idx, u64 w, const Op* op) {
    u32 pg = B(w, 16, 3), pn = B(w, 19, 1);
    int id = op->id;
    /* SSY/PBK/PCNT/PRET/CAL ignore the predicate field (reads as P0) */
    if (id == O_SSY || id == O_PBK || id == O_PCNT || id == O_PRET || id == O_CAL || id == O_PEXIT) {
        inst(c, idx, w, op);
        return;
    }
    if (pg == 7 && pn) {
        emitl(c, "/* never: %s */", op->name);
        return;
    }
    if (has_ccc(id) && B(w, 0, 5) != 15) { /* also depends on a condition code */
        emitl(c, "if(%s) {", flow_cond(c, w));
        c->indent++;
        inst(c, idx, w, op);
        c->indent--;
        emitl(c, "}");
        return;
    }
    if (pg == 7) {
        inst(c, idx, w, op);
        return;
    }
    emitl(c, "if(%s) {", PN(c, pg, pn));
    c->indent++;
    inst(c, idx, w, op);
    c->indent--;
    emitl(c, "}");
}

/* emit + label bookkeeping: any rN write kills its label, only unpredicated instrs set one */
void emit_tracked(Ctx* c, u32 idx, u64 w, const Op* op) {
    size_t s0 = c->out.n;
    c->tag_r = -1;
    c->tag_v = 0;
    emit_guarded(c, idx, w, op);
    for (size_t k = s0; k + 1 < c->out.n; k++) {
        const char* p = c->out.p + k;
        if (p[0] == 'r' && p[1] >= '0' && p[1] <= '9' &&
            (k == s0 || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'))) {
            u32 r = 0;
            const char* q = p + 1;
            while (*q >= '0' && *q <= '9') {
                r = r * 10 + (u32)(*q - '0');
                q++;
            }
            if (q[0] == ' ' && q[1] == '=' && q[2] != '=' && r < 256) c->wtag[r] = 0;
        }
    }
    if (c->tag_r >= 0 && c->tag_r < 255 && !c->nest && B(w, 16, 3) == 7 && !B(w, 19, 1))
        c->wtag[c->tag_r] = (u8)c->tag_v;
    c->tag_r = -1;
}
void exit_epilogue(Ctx* c) {
    if (c->stage == ST_FS) {
        u32 target = c->si->sph[18], flags = c->si->sph[19];
        u32 reg = 0;
        for (int rt = 0; rt < 8; rt++) {
            u32 m = (target >> (rt * 4)) & 15;
            if (!m) continue;
            for (int k = 0; k < 4; k++) {
                if (m & (1u << k)) {
                    if ((c->si->rt_int_mask >> rt) & 1)
                        emitl(c, "frag_color%d.%c = %s;", rt, "xyzw"[k],
                              ((c->si->rt_sint_mask >> rt) & 1) ? T("int(%s)", R(c, reg)) : R(c, reg));
                    else
                        emitl(c, "frag_color%d.%c = %s;", rt, "xyzw"[k], RF(c, reg));
                }
                reg++;
            }
        }
        /* after the colours: the sample mask in the next register, depth in the one after it whether or not the
           mask is written (Ryujinx TranslatorContext.GetDepthRegister: two registers after the last colour) */
        if (flags & 1) emitl(c, "gl_SampleMask[0] = int(%s);", R(c, reg));
        if (flags & 2) emitl(c, "gl_FragDepth = %s;", RF(c, reg + 1));
    }
    emitl(c, "return;");
}

static void inst(Ctx* c, u32 idx, u64 w, const Op* op) {
    u32 d = B(w, 0, 8), a = B(w, 8, 8);
    int id = op->id;
    switch (id) {
        /* ---------------- float */
        case O_FADD_R:
        case O_FADD_C:
        case O_FADD_I: {
            char* A = fneg_abs(RF(c, a), B(w, 48, 1), B(w, 46, 1));
            char* Bv = fneg_abs(opBf(c, w, id - O_FADD_R), B(w, 45, 1), B(w, 49, 1));
            setrf_cc(c, d, fsat(T("(%s + %s)", A, Bv), B(w, 50, 1)), B(w, 47, 1));
            break;
        }
        case O_FADD32I: { /* WriteCC 52 on the 32-bit immediate forms (Ryujinx InstFadd32i, Fmul32i, Ffma32i) */
            char* A = fneg_abs(RF(c, a), B(w, 56, 1), B(w, 54, 1));
            char* Bv = fneg_abs(imm32f(w), B(w, 53, 1), B(w, 57, 1));
            setrf_cc(c, d, T("(%s + %s)", A, Bv), B(w, 52, 1));
            break;
        }
        case O_FMUL_R:
        case O_FMUL_C:
        case O_FMUL_I: {
            char* Bv = opBf(c, w, id - O_FMUL_R);
            if (B(w, 48, 1)) Bv = T("(-%s)", Bv);
            char* e = B(w, 44, 2) == 2 ? T("fmz(%s, %s)", RF(c, a), Bv) : T("(%s * %s)", RF(c, a), Bv);
            u32 sc = B(w, 41, 3); /* 0*x=0 only matters for .FMZ */
            static const char* scl[8] = {"", "*0.5", "*0.25", "*0.125", "*8.0", "*4.0", "*2.0", ""};
            if (sc) e = T("(%s%s)", e, scl[sc]);
            setrf_cc(c, d, fsat(e, B(w, 50, 1)), B(w, 47, 1));
            break;
        }
        case O_FMUL32I:
            setrf_cc(
                c, d,
                fsat(B(w, 53, 2) == 2 ? T("fmz(%s, %s)", RF(c, a), imm32f(w)) : T("(%s * %s)", RF(c, a), imm32f(w)),
                     B(w, 55, 1)),
                B(w, 52, 1));
            break;
        case O_FFMA_R:
        case O_FFMA_RC:
        case O_FFMA_CR:
        case O_FFMA_I: {
            char *Bv, *Cv;
            if (id == O_FFMA_R) {
                Bv = RF(c, B(w, 20, 8));
                Cv = RF(c, B(w, 39, 8));
            } else if (id == O_FFMA_RC) {
                Bv = RF(c, B(w, 39, 8));
                Cv = CBF(c, cb_idx(w), cb_off(w));
            } else if (id == O_FFMA_CR) {
                Bv = CBF(c, cb_idx(w), cb_off(w));
                Cv = RF(c, B(w, 39, 8));
            } else {
                Bv = imm20f(w);
                Cv = RF(c, B(w, 39, 8));
            }
            if (B(w, 48, 1)) Bv = T("(-%s)", Bv);
            if (B(w, 49, 1)) Cv = T("(-%s)", Cv);
            setrf_cc(c, d,
                     fsat(B(w, 53, 2) == 2 ? T("ffmz(%s, %s, %s)", RF(c, a), Bv, Cv)
                                           : T("fma(%s, %s, %s)", RF(c, a), Bv, Cv),
                          B(w, 50, 1)),
                     B(w, 47, 1));
            break;
        }
        case O_FFMA32I: {
            char* A = RF(c, a);
            if (B(w, 56, 1)) A = T("(-%s)", A);
            char* Cv = RF(c, d);
            if (B(w, 57, 1)) Cv = T("(-%s)", Cv);
            setrf_cc(c, d,
                     fsat(B(w, 53, 2) == 2 ? T("ffmz(%s, %s, %s)", A, imm32f(w), Cv)
                                           : T("fma(%s, %s, %s)", A, imm32f(w), Cv),
                          B(w, 55, 1)),
                     B(w, 52, 1));
            break;
        }
        case O_FMNMX_R:
        case O_FMNMX_C:
        case O_FMNMX_I: {
            char* A = fneg_abs(RF(c, a), B(w, 48, 1), B(w, 46, 1));
            char* Bv = fneg_abs(opBf(c, w, id - O_FMNMX_R), B(w, 45, 1), B(w, 49, 1));
            /* with one NaN operand the result is the other one (GLSL's min/max leave it undefined) */
            emitl(c, "{ float a_ = %s, b_ = %s;", A, Bv);
            c->indent++;
            setrf_cc(c, d,
                     T("(isnan(a_) ? b_ : isnan(b_) ? a_ : %s ? min(a_, b_) : max(a_, b_))",
                       PN(c, B(w, 39, 3), B(w, 42, 1))),
                     B(w, 47, 1));
            c->indent--;
            emitl(c, "}");
            break;
        }
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I: {
            char* A = fneg_abs(RF(c, a), B(w, 43, 1), B(w, 7, 1));
            char* Bv = fneg_abs(opBf(c, w, id - O_FSETP_R), B(w, 6, 1), B(w, 44, 1));
            const char* t = fcmp_expr(B(w, 48, 4), A, Bv);
            const char* pc = PN(c, B(w, 39, 3), B(w, 42, 1));
            setp2(c, B(w, 3, 3), B(w, 0, 3), B(w, 45, 2), t, pc);
            break;
        }
        case O_FSET_R:
        case O_FSET_C:
        case O_FSET_I: {
            char* A = fneg_abs(RF(c, a), B(w, 43, 1), B(w, 54, 1));
            char* Bv = fneg_abs(opBf(c, w, id - O_FSET_R), B(w, 53, 1), B(w, 44, 1));
            const char* t = bop(B(w, 45, 2), fcmp_expr(B(w, 48, 4), A, Bv), PN(c, B(w, 39, 3), B(w, 42, 1)));
            setr_cc(c, d, T("(%s ? %s : 0u)", t, B(w, 52, 1) ? "floatBitsToUint(1.0)" : "0xFFFFFFFFu"), B(w, 47, 1),
                    B(w, 52, 1));
            break;
        }
        case O_FCMP_R:
        case O_FCMP_RC:
        case O_FCMP_CR:
        case O_FCMP_I: {
            char *Bv, *Cv;
            if (id == O_FCMP_R) {
                Bv = R(c, B(w, 20, 8));
                Cv = RF(c, B(w, 39, 8));
            } else if (id == O_FCMP_RC) {
                Bv = R(c, B(w, 39, 8));
                Cv = CBF(c, cb_idx(w), cb_off(w));
            } else if (id == O_FCMP_CR) {
                Bv = CB(c, cb_idx(w), cb_off(w));
                Cv = RF(c, B(w, 39, 8));
            } else {
                Bv = T("floatBitsToUint(%s)", imm20f(w));
                Cv = RF(c, B(w, 39, 8));
            }
            setr(c, d, T("(%s ? %s : %s)", fcmp_expr(B(w, 48, 4), Cv, "0.0"), R(c, a), Bv));
            break;
        }
        case O_MUFU: {
            char* A = fneg_abs(RF(c, a), B(w, 48, 1), B(w, 46, 1));
            const char* e;
            switch (B(w, 20, 4)) {
                case 0: e = T("cos(%s)", A); break;
                case 1: e = T("sin(%s)", A); break;
                case 2: e = T("exp2(%s)", A); break;
                case 3: e = T("log2(%s)", A); break;
                case 4: e = T("(1.0/%s)", A); break;
                case 5: e = T("inversesqrt(%s)", A); break;
                case 8: e = T("sqrt(%s)", A); break;
                default: e = A;
            }
            if (B(w, 20, 4) == 4 && !B(w, 48, 1) && !B(w, 46, 1) && !B(w, 50, 1) && a != 255 && c->wtag[a] == 1) {
                c->tag_r = (int)d;
                c->tag_v = 2;
            }
            setrf(c, d, fsat(e, B(w, 50, 1)));
            break;
        }
        case O_RRO_R:
        case O_RRO_C:
        case O_RRO_I: setrf(c, d, fneg_abs(opBf(c, w, id - O_RRO_R), B(w, 45, 1), B(w, 49, 1))); break;
        case O_F2F_R:
        case O_F2F_C:
        case O_F2F_I: {
            u32 ds = B(w, 8, 2), ss = B(w, 10, 2);
            char* src;
            if (ss == 1) { /* F16 */
                src = T("unpackHalf2x16(%s).%c", opB(c, w, id - O_F2F_R), B(w, 41, 1) ? 'y' : 'x');
            } else
                src = opBf(c, w, id - O_F2F_R);
            src = fneg_abs(src, B(w, 45, 1), B(w, 49, 1));
            /* rounding to an integer only with bit 42 (Ryujinx InstF2fR.RoundMode, IntegerRound 4-7); without it bits
               39-40 are the float rounding mode, which a same-size conversion doesn't use */
            if (ds == ss && ss == 2 && B(w, 42, 1)) {
                static const char* fn[4] = {"roundEven", "floor", "ceil", "trunc"};
                src = T("%s(%s)", fn[B(w, 39, 2)], src);
            }
            if (ds == 1)
                setr(c, d, T("packHalf2x16(vec2(%s,0.0))", fsat(src, B(w, 50, 1))));
            else
                setrf(c, d, fsat(src, B(w, 50, 1)));
            break;
        }
        case O_F2I_R:
        case O_F2I_C:
        case O_F2I_I: {
            char* src = opBf(c, w, id - O_F2I_R);
            if (B(w, 10, 2) == 1) src = T("unpackHalf2x16(%s).%c", opB(c, w, id - O_F2I_R), B(w, 41, 1) ? 'y' : 'x');
            src = fneg_abs(src, B(w, 45, 1), B(w, 49, 1)); /* Ryujinx InstF2iR: NegB 45, AbsB 49 */
            static const char* fn[4] = {"roundEven", "floor", "ceil", "trunc"};
            src = T("%s(%s)", fn[B(w, 39, 2)], src);
            /* saturating, NaN to 0, and a 16-bit destination (IDstFmt bits 8-9 = 1) clamped to its range */
            int sg = B(w, 12, 1), small = B(w, 8, 2) == 1;
            const char* v = sg ? "(isnan(f_) ? 0 : f_ >= 2147483648.0 ? 0x7FFFFFFF : int(max(f_, -2147483648.0)))"
                               : "(isnan(f_) ? 0u : f_ >= 4294967296.0 ? 0xFFFFFFFFu : uint(max(f_, 0.0)))";
            if (small) v = sg ? T("clamp(%s, -32768, 32767)", v) : T("min(%s, 65535u)", v);
            emitl(c, "{ float f_ = %s;", src);
            c->indent++;
            setr(c, d, sg ? T("uint(%s)", v) : v);
            c->indent--;
            emitl(c, "}");
            break;
        }
        case O_I2F_R:
        case O_I2F_C:
        case O_I2F_I: {
            char* s = opB(c, w, id - O_I2F_R);
            u32 fmt = B(w, 10, 2), sel = B(w, 41, 2);
            int sg = B(w, 13, 1);
            const char* v;
            if (fmt == 0)
                v = sg ? T("float(bitfieldExtract(int(%s),%u,8))", s, sel * 8)
                       : T("float(bitfieldExtract(%s,%u,8))", s, sel * 8);
            else if (fmt == 1)
                v = sg ? T("float(bitfieldExtract(int(%s),%u,16))", s, (sel & 2) * 8)
                       : T("float(bitfieldExtract(%s,%u,16))", s, (sel & 2) * 8);
            else
                v = sg ? T("float(int(%s))", s) : T("float(%s)", s);
            v = fneg_abs(v, B(w, 45, 1), B(w, 49, 1));
            if (B(w, 8, 2) == 1)
                setr(c, d, T("packHalf2x16(vec2(%s,0.0))", v));
            else
                setrf(c, d, v);
            break;
        }
        case O_I2I_R:
        case O_I2I_C:
        case O_I2I_I: {
            char* s = opB(c, w, id - O_I2I_R);
            u32 sf = B(w, 10, 2), df = B(w, 8, 2), sel = B(w, 41, 2);
            int ss = B(w, 13, 1), ds = B(w, 12, 1);
            const char* v = sf == 0   ? (ss ? T("bitfieldExtract(int(%s),%u,8)", s, sel * 8)
                                            : T("int(bitfieldExtract(%s,%u,8))", s, sel * 8))
                            : sf == 1 ? (ss ? T("bitfieldExtract(int(%s),%u,16)", s, (sel & 2) * 8)
                                            : T("int(bitfieldExtract(%s,%u,16))", s, (sel & 2) * 8))
                                      : T("int(%s)", s);
            if (B(w, 49, 1)) v = T("abs(%s)", v);
            if (B(w, 45, 1)) v = T("(-%s)", v);
            if (B(w, 50, 1)) {
                if (df == 0)
                    v = ds ? T("clamp(%s,-128,127)", v) : T("clamp(%s,0,255)", v);
                else if (df == 1)
                    v = ds ? T("clamp(%s,-32768,32767)", v) : T("clamp(%s,0,65535)", v);
            } else {
                if (df == 0)
                    v = ds ? T("bitfieldExtract(%s,0,8)", v) : T("(%s & 255)", v);
                else if (df == 1)
                    v = ds ? T("bitfieldExtract(%s,0,16)", v) : T("(%s & 65535)", v);
            }
            setr_cc(c, d, T("uint(%s)", v), B(w, 47, 1), 0);
            break;
        }
        /* ---------------- moves */
        case O_MOV_R:
        case O_MOV_C:
        case O_MOV_I:
            if (d != 255) c->gtag[d] = id == O_MOV_C ? gbuf_tag(c, w) : id == O_MOV_R ? c->gtag[B(w, 20, 8)] : 0;
            setr(c, d, opB(c, w, id - O_MOV_R));
            break;
        case O_MOV32I: setr(c, d, imm32(w)); break;
        case O_S2R: {
            u32 sr = B(w, 20, 8);
            const char* v = "0u";
            switch (sr) {
                case 0x00: v = "0u"; break;
                case 0x12: v = "floatBitsToUint(1.0)"; break;
                case 0x20:
                    v = c->stage == ST_CS
                            ? "(gl_LocalInvocationID.x | (gl_LocalInvocationID.y<<16) | (gl_LocalInvocationID.z<<26))"
                            : "0u";
                    break;
                case 0x21: v = c->stage == ST_CS ? "gl_LocalInvocationID.x" : "0u"; break;
                case 0x22: v = c->stage == ST_CS ? "gl_LocalInvocationID.y" : "0u"; break;
                case 0x23: v = c->stage == ST_CS ? "gl_LocalInvocationID.z" : "0u"; break;
                case 0x25: v = c->stage == ST_CS ? "gl_WorkGroupID.x" : "0u"; break;
                case 0x26: v = c->stage == ST_CS ? "gl_WorkGroupID.y" : "0u"; break;
                case 0x27: v = c->stage == ST_CS ? "gl_WorkGroupID.z" : "0u"; break;
                case 0x1D: v = c->stage == ST_FS ? "0u" : "0x00ff0000u"; break;
                case 0x38: v = "1u"; break;
                case 0x39: v = "0u"; break;
                case 0x3A: v = "1u"; break;
                case 0x3B: v = "0xFFFFFFFEu"; break;
                case 0x3C: v = "0xFFFFFFFFu"; break;
                case 0x50:
                case 0x52: v = "uint(0)"; break;
                default: v = "0u";
            }
            setr(c, d, v);
            break;
        }
        case O_CS2R:
            setr(c, d, "0u");
            if (d != 255) setr(c, d + 1, "0u");
            break;
        /* ---------------- integer */
        case O_IADD_R:
        case O_IADD_C:
        case O_IADD_I: {
            if (d != 255) {
                u8 tg = 0;
                if (id == O_IADD_C) tg = gbuf_tag(c, w);
                if (!tg) tg = c->gtag[a];
                if (!tg && id == O_IADD_R) tg = c->gtag[B(w, 20, 8)];
                c->gtag[d] = tg;
            }
            /* AvgMode 48-49, X 43, CC 47 (Ryujinx InstIaddR) */
            emit_iadd(c, d, R(c, a), opB(c, w, id - O_IADD_R), B(w, 48, 2), B(w, 43, 1), B(w, 47, 1));
            break;
        }
        case O_IADD32I: /* AvgMode 55-56, X 53, CC 52 (Ryujinx InstIadd32i) */
            emit_iadd(c, d, R(c, a), imm32(w), B(w, 55, 2), B(w, 53, 1), B(w, 52, 1));
            break;
        case O_IADD3_R:
        case O_IADD3_C:
        case O_IADD3_I: {
            char* A = R(c, a);
            char* Bv = id == O_IADD3_R ? R(c, B(w, 20, 8)) : id == O_IADD3_C ? CB(c, cb_idx(w), cb_off(w)) : imm20i(w);
            char* Cv = R(c, B(w, 39, 8));
            if (id == O_IADD3_R) {
                static const char* hs[4] = {"%s", "(%s & 0xFFFFu)", "(%s >> 16)", "%s"};
                A = T(hs[B(w, 35, 2)], A);
                Bv = T(hs[B(w, 33, 2)], Bv);
                Cv = T(hs[B(w, 31, 2)], Cv);
            }
            if (B(w, 51, 1)) A = T("(0u-%s)", A);
            if (B(w, 50, 1)) Bv = T("(0u-%s)", Bv);
            if (B(w, 49, 1)) Cv = T("(0u-%s)", Cv);
            u32 sh = id == O_IADD3_R ? B(w, 37, 2) : 0;
            int x = B(w, 48, 1), cc = B(w, 47, 1); /* Ryujinx InstIadd3R: X 48, WriteCC 47 */
            if (cc || x) {
                /* .X adds the carry in; .CC: Z and N of the sum, C if any of its adds carried (A + B's own carry
                   only counts when no shift drops it) */
                c->uses_cc = 1;
                emitl(c, "{ uint c0_ = 0u, c1_, c2_ = 0u; uint s_ = %s;",
                      sh == 1 || sh == 2 ? T("(%s + %s)", A, Bv) : T("uaddCarry(%s, %s, c0_)", A, Bv));
                c->indent++;
                if (sh == 1) emitl(c, "s_ >>= 16;");
                if (sh == 2) emitl(c, "s_ <<= 16;");
                emitl(c, "s_ = uaddCarry(s_, %s, c1_);", Cv);
                if (x) emitl(c, "s_ = uaddCarry(s_, cc_c?1u:0u, c2_);");
                if (cc) emitl(c, "cc_c = (c0_|c1_|c2_)!=0u; cc_z = s_==0u%s; cc_s = int(s_)<0;", x ? " && cc_z" : "");
                setr(c, d, "s_");
                c->indent--;
                emitl(c, "}");
                break;
            }
            const char* ab = T("(%s + %s)", A, Bv);
            if (sh == 1)
                ab = T("(%s >> 16)", ab);
            else if (sh == 2)
                ab = T("(%s << 16)", ab);
            setr(c, d, T("(%s + %s)", ab, Cv));
            break;
        }
        case O_ISCADD_R:
        case O_ISCADD_C:
        case O_ISCADD_I: {
            /* AvgMode 48-49 (Ryujinx InstIscaddR): 1 negates B, 2 negates A, 3 adds one */
            /* a scaled index added to a global buffer's base keeps pointing into that buffer */
            if (d != 255)
                c->gtag[d] = B(w, 48, 2) == 1   ? 0
                             : id == O_ISCADD_C ? gbuf_tag(c, w)
                             : id == O_ISCADD_R ? c->gtag[B(w, 20, 8)]
                                                : 0;
            char* A = R(c, a);
            char* Bv = opB(c, w, id - O_ISCADD_R);
            u32 avg = B(w, 48, 2);
            if (B(w, 47, 1)) { /* WriteCC: the flags of the add, as IADD's (Ryujinx EmitIscadd) */
                emit_iadd(c, d, T("(%s << %uu)", A, B(w, 39, 5)), Bv, avg, 0, 1);
                break;
            }
            if (avg == 2) A = T("(0u-%s)", A);
            if (avg == 1) Bv = T("(0u-%s)", Bv);
            setr(c, d, T("((%s << %uu) + %s%s)", A, B(w, 39, 5), Bv, avg == 3 ? " + 1u" : ""));
            break;
        }
        case O_ISCADD32I: /* Imm5 53-57, WriteCC 52 (Ryujinx InstIscadd32i) */
            if (B(w, 52, 1))
                emit_iadd(c, d, T("(%s << %uu)", R(c, a), B(w, 53, 5)), imm32(w), 0, 0, 1);
            else
                setr(c, d, T("((%s << %uu) + %s)", R(c, a), B(w, 53, 5), imm32(w)));
            break;
        case O_IMNMX_R:
        case O_IMNMX_C:
        case O_IMNMX_I: {
            char* A = R(c, a);
            char* Bv = opB(c, w, id - O_IMNMX_R);
            const char* pr = PN(c, B(w, 39, 3), B(w, 42, 1));
            if (B(w, 48, 1))
                setr_cc(c, d, T("uint(%s ? min(int(%s),int(%s)) : max(int(%s),int(%s)))", pr, A, Bv, A, Bv),
                        B(w, 47, 1), 0);
            else
                setr_cc(c, d, T("(%s ? min(%s,%s) : max(%s,%s))", pr, A, Bv, A, Bv), B(w, 47, 1), 0);
            break;
        }
        case O_ISETP_R:
        case O_ISETP_C:
        case O_ISETP_I: {
            const char* t = icmp_xe(c, w, R(c, a), opB(c, w, id - O_ISETP_R));
            const char* pc = PN(c, B(w, 39, 3), B(w, 42, 1));
            setp2(c, B(w, 3, 3), B(w, 0, 3), B(w, 45, 2), t, pc);
            break;
        }
        case O_ISET_R:
        case O_ISET_C:
        case O_ISET_I: {
            const char* t =
                bop(B(w, 45, 2), icmp_xe(c, w, R(c, a), opB(c, w, id - O_ISET_R)), PN(c, B(w, 39, 3), B(w, 42, 1)));
            setr_cc(c, d, T("(%s ? %s : 0u)", t, B(w, 44, 1) ? "floatBitsToUint(1.0)" : "0xFFFFFFFFu"), B(w, 47, 1),
                    B(w, 44, 1));
            break;
        }
        case O_ICMP_R:
        case O_ICMP_RC:
        case O_ICMP_CR:
        case O_ICMP_I: {
            char *Bv, *Cv;
            if (id == O_ICMP_R) {
                Bv = R(c, B(w, 20, 8));
                Cv = R(c, B(w, 39, 8));
            } else if (id == O_ICMP_RC) {
                Bv = R(c, B(w, 39, 8));
                Cv = CB(c, cb_idx(w), cb_off(w));
            } else if (id == O_ICMP_CR) {
                Bv = CB(c, cb_idx(w), cb_off(w));
                Cv = R(c, B(w, 39, 8));
            } else {
                Bv = imm20i(w);
                Cv = R(c, B(w, 39, 8));
            }
            setr(c, d, T("(%s ? %s : %s)", icmp_expr(B(w, 49, 3), Cv, "0u", B(w, 48, 1)), R(c, a), Bv));
            break;
        }
        case O_LOP_R:
        case O_LOP_C:
        case O_LOP_I:
        case O_LOP32I: {
            char* A = R(c, a);
            char* Bv = id == O_LOP32I ? imm32(w) : opB(c, w, id - O_LOP_R);
            u32 opn = id == O_LOP32I ? B(w, 53, 2) : B(w, 41, 2);
            int ia = id == O_LOP32I ? B(w, 55, 1) : B(w, 39, 1);
            int ib = id == O_LOP32I ? B(w, 56, 1) : B(w, 40, 1);
            if (ia) A = T("(~%s)", A);
            if (ib) Bv = T("(~%s)", Bv);
            const char* e = opn == 0   ? T("(%s & %s)", A, Bv)
                            : opn == 1 ? T("(%s | %s)", A, Bv)
                            : opn == 2 ? T("(%s ^ %s)", A, Bv)
                                       : Bv;
            if (id != O_LOP32I && B(w, 48, 3) != 7) {
                u32 po = B(w, 44, 2);
                emitl(c, "{ uint t_ = %s;", e);
                c->indent++;
                setr(c, d, "t_");
                setp(c, B(w, 48, 3), po == 0 ? "false" : po == 1 ? "true" : po == 2 ? "(t_ == 0u)" : "(t_ != 0u)");
                c->indent--;
                emitl(c, "}");
            } else
                setr(c, d, e);
            if ((id == O_LOP32I ? B(w, 52, 1) : B(w, 47, 1))) {
                c->uses_cc = 1;
                emitl(c, "cc_z = (%s) == 0u; cc_s = int(%s) < 0;", R(c, d), R(c, d));
            }
            break;
        }
        case O_LOP3_R:
        case O_LOP3_C:
        case O_LOP3_I: {
            u32 lut = id == O_LOP3_R ? B(w, 28, 8) : B(w, 48, 8);
            char* A = R(c, a);
            char* Bv = id == O_LOP3_R ? R(c, B(w, 20, 8)) : id == O_LOP3_C ? CB(c, cb_idx(w), cb_off(w)) : imm20i(w);
            char* Cv = R(c, B(w, 39, 8));
            Sb e = {0};
            sb_put(&e, "(0u");
            for (int i = 0; i < 8; i++)
                if (lut & (1u << i))
                    sb_f(&e, " | (%s%s & %s%s & %s%s)", (i & 4) ? "" : "~", A, (i & 2) ? "" : "~", Bv,
                         (i & 1) ? "" : "~", Cv);
            sb_put(&e, ")");
            if (id == O_LOP3_R && B(w, 48, 3) != 7) {
                u32 po = B(w, 36, 2);
                emitl(c, "{ uint t_ = %s;", e.p);
                c->indent++;
                setr(c, d, "t_");
                setp(c, B(w, 48, 3), po == 0 ? "false" : po == 1 ? "true" : po == 2 ? "(t_ == 0u)" : "(t_ != 0u)");
                c->indent--;
                emitl(c, "}");
            } else
                setr(c, d, e.p);
            free(e.p);
            break;
        }
        case O_SHL_R:
        case O_SHL_C:
        case O_SHL_I: {
            char* Bv = opB(c, w, id - O_SHL_R);
            if (B(w, 39, 1))
                setr(c, d, T("(%s << (%s & 31u))", R(c, a), Bv));
            else
                setr(c, d, T("((%s) >= 32u ? 0u : (%s << (%s)))", Bv, R(c, a), Bv));
            break;
        }
        case O_SHR_R:
        case O_SHR_C:
        case O_SHR_I: {
            char* Bv = opB(c, w, id - O_SHR_R);
            char* A = R(c, a);
            if (B(w, 40, 1)) A = T("bitfieldReverse(%s)", A);
            const char* sh = B(w, 39, 1) ? T("(%s & 31u)", Bv) : T("min(%s,31u)", Bv);
            if (B(w, 48, 1))
                setr(c, d, T("uint(int(%s) >> int(%s))", A, sh));
            else
                setr(c, d, B(w, 39, 1) ? T("(%s >> %s)", A, sh) : T("((%s) >= 32u ? 0u : (%s >> (%s)))", Bv, A, Bv));
            break;
        }
        case O_SHF_LR:
        case O_SHF_LI:
        case O_SHF_RR:
        case O_SHF_RI: {
            char* lo = R(c, a);
            char* sh = (id == O_SHF_LR || id == O_SHF_RR) ? R(c, B(w, 20, 8)) : T("%uu", B(w, 20, 6));
            char* hi = R(c, B(w, 39, 8));
            int left = id == O_SHF_LR || id == O_SHF_LI;
            /* Ryujinx InstShf* and EmitShf: MaxShift 37-38 (2 U64, 3 S64) makes it a 64-bit shift of hi:lo, M (bit
               50) wraps the amount to the shift's width, without M an amount past it gives 0. (XModeShf 48-49 has
               no reference semantics, Ryujinx leaves it too) */
            u32 maxs = B(w, 37, 2);
            int lng = maxs >= 2, s64 = maxs == 3;
            emitl(c, "{ uint s_ = %s%s, lo_ = %s, hi_ = %s;", sh, B(w, 50, 1) ? (lng ? " & 63u" : " & 31u") : "", lo,
                  hi);
            c->indent++;
            const char* e;
            if (left)
                e = T(
                    "(s_ >= %uu ? 0u : s_ >= 32u ? (lo_ << (s_-32u)) : s_ == 0u ? hi_ : ((hi_ << s_) | (lo_ >> (32u-s_))))",
                    lng ? 64 : 32);
            else
                e = T("(s_ >= %uu ? 0u : s_ >= 32u ? %s : s_ == 0u ? lo_ : ((lo_ >> s_) | (hi_ << (32u-s_))))",
                      lng ? 64 : 32, s64 ? "uint(int(hi_) >> int(s_-32u))" : "(hi_ >> (s_-32u))");
            setr_cc(c, d, e, B(w, 47, 1), 0); /* WriteCC 47: Z and N of the result */
            c->indent--;
            emitl(c, "}");
            break;
        }
        case O_BFE_R:
        case O_BFE_C:
        case O_BFE_I: {
            char* Bv = opB(c, w, id - O_BFE_R);
            char* A = R(c, a);
            if (B(w, 40, 1)) A = T("bitfieldReverse(%s)", A);
            const char *off = T("int(%s & 255u)", Bv), *cnt = T("int((%s >> 8) & 255u)", Bv);
            if (B(w, 48, 1))
                setr(c, d,
                     T("uint((%s)==0 ? 0 : bitfieldExtract(int(%s), min(%s,31), min(%s,32-min(%s,31))))", cnt, A, off,
                       cnt, off));
            else
                setr(c, d,
                     T("((%s)==0 ? 0u : bitfieldExtract(%s, min(%s,31), min(%s,32-min(%s,31))))", cnt, A, off, cnt,
                       off));
            break;
        }
        case O_BFI_R:
        case O_BFI_RC:
        case O_BFI_CR:
        case O_BFI_I: {
            char *Bv, *Cv;
            if (id == O_BFI_R) {
                Bv = R(c, B(w, 20, 8));
                Cv = R(c, B(w, 39, 8));
            } else if (id == O_BFI_RC) {
                Bv = R(c, B(w, 39, 8));
                Cv = CB(c, cb_idx(w), cb_off(w));
            } else if (id == O_BFI_CR) {
                Bv = CB(c, cb_idx(w), cb_off(w));
                Cv = R(c, B(w, 39, 8));
            } else {
                Bv = imm20i(w);
                Cv = R(c, B(w, 39, 8));
            }
            const char *off = T("int(%s & 255u)", Bv), *cnt = T("int((%s >> 8) & 255u)", Bv);
            setr(c, d, T("bitfieldInsert(%s, %s, min(%s,31), min(%s,32-min(%s,31)))", Cv, R(c, a), off, cnt, off));
            break;
        }
        case O_SEL_R:
        case O_SEL_C:
        case O_SEL_I:
            setr(c, d, T("(%s ? %s : %s)", PN(c, B(w, 39, 3), B(w, 42, 1)), R(c, a), opB(c, w, id - O_SEL_R)));
            break;
        case O_POPC_R:
        case O_POPC_C:
        case O_POPC_I: {
            char* Bv = opB(c, w, id - O_POPC_R);
            if (B(w, 40, 1)) Bv = T("(~%s)", Bv);
            setr(c, d, T("uint(bitCount(%s))", Bv));
            break;
        }
        case O_FLO_R:
        case O_FLO_C:
        case O_FLO_I: {
            char* Bv = opB(c, w, id - O_FLO_R);
            if (B(w, 40, 1)) Bv = T("(~%s)", Bv);
            const char* m = B(w, 48, 1) ? T("findMSB(int(%s))", Bv) : T("findMSB(%s)", Bv);
            if (B(w, 41, 1))
                setr(c, d, T("(%s < 0 ? 0xFFFFFFFFu : uint(31 - %s))", m, m));
            else
                setr(c, d, T("uint(%s)", m));
            break;
        }
        case O_PRMT_R:
        case O_PRMT_RC:
        case O_PRMT_CR:
        case O_PRMT_I: {
            char *Bv, *Cv;
            if (id == O_PRMT_R) {
                Bv = R(c, B(w, 20, 8));
                Cv = R(c, B(w, 39, 8));
            } else if (id == O_PRMT_RC) {
                Bv = R(c, B(w, 39, 8));
                Cv = CB(c, cb_idx(w), cb_off(w));
            } else if (id == O_PRMT_CR) {
                Bv = CB(c, cb_idx(w), cb_off(w));
                Cv = R(c, B(w, 39, 8));
            } else {
                Bv = imm20i(w);
                Cv = R(c, B(w, 39, 8));
            }
            emitl(c, "{ uint s_ = %s; uint a_ = %s, c_ = %s; uint o_ = 0u;", Bv, R(c, a), Cv);
            c->indent++;
            /* PMode 48-51 (Ryujinx InstPrmtR, enum PMode: F4E B4E RC8 ECL ECR RC16) picks the bytes from the
               selector's low 2 bits instead, as PTX's prmt modes do: the table holds each mode's byte indices, d.b0
               in the low nibble */
            static const u16 pm[6][4] = {{0x3210, 0x4321, 0x5432, 0x6543}, {0x5670, 0x6701, 0x7012, 0x0123},
                                         {0x0000, 0x1111, 0x2222, 0x3333}, {0x3210, 0x3211, 0x3222, 0x3333},
                                         {0x0000, 0x1110, 0x2210, 0x3210}, {0x1010, 0x3232, 0x1010, 0x3232}};
            u32 pmode = B(w, 48, 4);
            if (pmode >= 1 && pmode <= 6) {
                const u16* t = pm[pmode - 1];
                emitl(c, "s_ = uint[4](0x%Xu, 0x%Xu, 0x%Xu, 0x%Xu)[s_ & 3u];", t[0], t[1], t[2], t[3]);
            } else if (pmode)
                unsupported(c, "PRMT mode", w);
            emitl(
                c,
                "for(int i=0;i<4;i++){ uint sel=(s_>>(i*4))&15u; uint src=(sel&4u)!=0u? c_ : a_; uint by=(src>>((sel&3u)*8u))&255u; if((sel&8u)!=0u) by=(by&128u)!=0u?255u:0u; o_ |= by<<(i*8); }");
            setr(c, d, "o_");
            c->indent--;
            emitl(c, "}");
            break;
        }
        case O_XMAD_R:
        case O_XMAD_RC:
        case O_XMAD_CR:
        case O_XMAD_I: {
            /* fields per Ryujinx InstXmadR/I/C/Rc: HiloA 53, ASigned 48, BSigned 49, WriteCC 47; R and I: XmadCop
               50-52, HiloB 35 (R), Psl 36, Mrg 37, X 38; C and RC: XmadCop 50-51, HiloB 52, X 54, and only C has Psl 55
               and Mrg 56 */
            char *Bv, *Cv;
            u32 mode, hb;
            int psl = 0, mrg = 0, x;
            if (id == O_XMAD_R) {
                Bv = R(c, B(w, 20, 8));
                Cv = R(c, B(w, 39, 8));
                mode = B(w, 50, 3);
                hb = B(w, 35, 1);
                psl = B(w, 36, 1);
                mrg = B(w, 37, 1);
                x = B(w, 38, 1);
            } else if (id == O_XMAD_RC) {
                Bv = R(c, B(w, 39, 8));
                Cv = CB(c, cb_idx(w), cb_off(w));
                mode = B(w, 50, 2);
                hb = B(w, 52, 1);
                x = B(w, 54, 1);
            } else if (id == O_XMAD_CR) {
                Bv = CB(c, cb_idx(w), cb_off(w));
                Cv = R(c, B(w, 39, 8));
                mode = B(w, 50, 2);
                hb = B(w, 52, 1);
                psl = B(w, 55, 1);
                mrg = B(w, 56, 1);
                x = B(w, 54, 1);
            } else {
                Bv = T("%uu", B(w, 20, 16));
                Cv = R(c, B(w, 39, 8));
                mode = B(w, 50, 3);
                hb = 0;
                psl = B(w, 36, 1);
                mrg = B(w, 37, 1);
                x = B(w, 38, 1);
            }
            int sa = B(w, 48, 1), sb = B(w, 49, 1), ha = B(w, 53, 1);
            const char* A = sa ? T("uint(bitfieldExtract(int(%s),%d,16))", R(c, a), ha ? 16 : 0)
                               : T("bitfieldExtract(%s,%d,16)", R(c, a), ha ? 16 : 0);
            const char* Bb = sb ? T("uint(bitfieldExtract(int(%s),%d,16))", Bv, hb ? 16 : 0)
                                : T("bitfieldExtract(%s,%d,16)", Bv, hb ? 16 : 0);
            /* XmadCop: 1 CLO, 2 CHI, 3 CSFU (C less the sign adjustments of the 16-bit operands, Ryujinx EmitXmad),
               4 CBCC */
            const char* C = mode == 1   ? T("(%s & 0xFFFFu)", Cv)
                            : mode == 2 ? T("(%s >> 16)", Cv)
                            : mode == 3 ? T("(%s - (((%s >> 31) << 16) + ((%s >> 31) << 16)))", Cv, A, Bb)
                            : mode == 4 ? T("((%s << 16) + %s)", Bv, Cv)
                                        : Cv;
            const char* prod = T("(%s * %s)", A, Bb);
            if (psl) prod = T("(%s << 16)", prod);
            if (B(w, 47, 1) || x) {
                /* .X adds the carry in, .CC sets the flags like IADD (Ryujinx EmitXmad, SetIaddFlags) */
                c->uses_cc = 1;
                emitl(c, "{ uint a_ = %s, b_ = %s, c1_, c2_ = 0u; uint s_ = uaddCarry(a_, b_, c1_);", prod, C);
                c->indent++;
                if (x) emitl(c, "s_ = uaddCarry(s_, cc_c?1u:0u, c2_);");
                if (B(w, 47, 1))
                    emitl(
                        c,
                        "cc_c = (c1_|c2_)!=0u; cc_o = ((s_ ^ a_) & ~(a_ ^ b_)) >= 0x80000000u; cc_z = s_==0u%s; cc_s = int(s_)<0;",
                        x ? " && cc_z" : "");
                setr(c, d, mrg ? T("((s_ & 0xFFFFu) | (%s << 16))", Bv) : "s_");
                c->indent--;
                emitl(c, "}");
                break;
            }
            const char* e = T("(%s + %s)", prod, C);
            if (mrg) e = T("((%s & 0xFFFFu) | (%s << 16))", e, Bv);
            setr(c, d, e);
            break;
        }
        case O_IMUL_R:
        case O_IMUL_C:
        case O_IMUL_I:
        case O_IMUL32I: {
            /* Ryujinx InstImul*: Hilo 39, ASigned 40, BSigned 41; IMUL32I Hilo 53, ASigned 54, BSigned 55 */
            char* Bv = id == O_IMUL32I ? imm32(w) : opB(c, w, id - O_IMUL_R);
            int hi = id == O_IMUL32I ? B(w, 53, 1) : B(w, 39, 1);
            if (hi) {
                open_mulhi(c, R(c, a), Bv, id == O_IMUL32I ? B(w, 54, 1) : B(w, 40, 1),
                           id == O_IMUL32I ? B(w, 55, 1) : B(w, 41, 1));
                setr(c, d, "h_");
                c->indent--;
                emitl(c, "}");
            } else
                setr(c, d, T("(%s * %s)", R(c, a), Bv));
            break;
        }
        case O_IMAD_R:
        case O_IMAD_RC:
        case O_IMAD_CR:
        case O_IMAD_I:
        case O_IMAD32I: {
            char *Bv, *Cv;
            if (id == O_IMAD_R) {
                Bv = R(c, B(w, 20, 8));
                Cv = R(c, B(w, 39, 8));
            } else if (id == O_IMAD_RC) {
                Bv = R(c, B(w, 39, 8));
                Cv = CB(c, cb_idx(w), cb_off(w));
            } else if (id == O_IMAD_CR) {
                Bv = CB(c, cb_idx(w), cb_off(w));
                Cv = R(c, B(w, 39, 8));
            } else if (id == O_IMAD_I) {
                Bv = imm20i(w);
                Cv = R(c, B(w, 39, 8));
            } else {
                Bv = imm32(w);
                Cv = R(c, d);
            }
            /* Ryujinx InstImad*: Hilo 54, ASigned 48, BSigned 53, AvgMode 51-52, X 49; IMAD32I: Hilo 53, ASigned 54,
               BSigned 57, AvgMode 55-56. AvgMode (Ryujinx EmitImad) 2 negates B, 1 negates C, 3 adds one. IMAD.HI is
               hi32(a*b) + c: this is how div-by-constant gets compiled */
            int i32i = id == O_IMAD32I;
            int hi = i32i ? B(w, 53, 1) : B(w, 54, 1), x = i32i ? 0 : B(w, 49, 1);
            u32 avg = i32i ? B(w, 55, 2) : B(w, 51, 2);
            if (avg == 2) Bv = T("(0u-%s)", Bv);
            if (avg == 1) Cv = T("(0u-%s)", Cv);
            const char* add = T("%s%s%s", Cv, avg == 3 ? " + 1u" : "", x ? " + (cc_c?1u:0u)" : "");
            if (x) c->uses_cc = 1;
            if (hi) {
                open_mulhi(c, R(c, a), Bv, i32i ? B(w, 54, 1) : B(w, 48, 1), i32i ? B(w, 57, 1) : B(w, 53, 1));
                setr(c, d, T("h_ + %s", add));
                c->indent--;
                emitl(c, "}");
                break;
            }
            setr(c, d, T("(%s * %s + %s)", R(c, a), Bv, add));
            break;
        }
        case O_LEA_LR:
        case O_LEA_LC:
        case O_LEA_LI:
        case O_LEA_HR:
        case O_LEA_HC: {
            char* Bv = id == O_LEA_LR || id == O_LEA_HR ? R(c, B(w, 20, 8))
                       : id == O_LEA_LI                 ? imm20i(w)
                                                        : CB(c, cb_idx(w), cb_off(w));
            /* Ryujinx InstLea*: the LO forms take ImmU5 39-43 and NegA 45; LEA.HI R ImmU5 28-32 and NegA 37, LEA.HI C
               ImmU5 51-55 and NegA 56 (its 28-32 are the cbuf offset) */
            int hiform = id == O_LEA_HR || id == O_LEA_HC;
            u32 sh = !hiform ? B(w, 39, 5) : id == O_LEA_HR ? B(w, 28, 5) : B(w, 51, 5);
            int nega = !hiform ? B(w, 45, 1) : id == O_LEA_HR ? B(w, 37, 1) : B(w, 56, 1);
            char* A = R(c, a);
            if (hiform) {
                /* the high word of (C:A) << sh, plus B (Ryujinx EmitLeaHi); NegA negates the 64-bit value, so the +1
                   carries into the high word only when the low word is 0 */
                char* hi = R(c, B(w, 39, 8));
                const char* h = sh ? T("((%s >> %uu) | (%s << %uu))", A, 32 - sh, hi, sh) : hi;
                if (nega) h = T("(~%s + ((%s << %uu) == 0u ? 1u : 0u))", h, A, sh);
                setr(c, d, T("(%s + %s)", h, Bv));
            } else {
                if (nega) A = T("(0u-%s)", A);
                setr(c, d, T("((%s << %uu) + %s)", A, sh, Bv));
            }
            if (B(w, 48, 3) != 7 && (id == O_LEA_LR || id == O_LEA_LI || id == O_LEA_LC)) setp(c, B(w, 48, 3), "false");
            break;
        }
        /* ---------------- predicates */
        case O_PSETP: {
            const char* ab = bop(B(w, 24, 2), PN(c, B(w, 12, 3), B(w, 15, 1)), PN(c, B(w, 29, 3), B(w, 32, 1)));
            const char* cc = PN(c, B(w, 39, 3), B(w, 42, 1));
            setp2(c, B(w, 3, 3), B(w, 0, 3), B(w, 45, 2), ab, cc);
            break;
        }
        case O_PSET: {
            const char* ab = bop(B(w, 24, 2), PN(c, B(w, 12, 3), B(w, 15, 1)), PN(c, B(w, 29, 3), B(w, 32, 1)));
            const char* t = bop(B(w, 45, 2), ab, PN(c, B(w, 39, 3), B(w, 42, 1)));
            setr(c, d, T("(%s ? %s : 0u)", t, B(w, 44, 1) ? "floatBitsToUint(1.0)" : "0xFFFFFFFFu"));
            break;
        }
        case O_P2R_R:
        case O_P2R_C:
        case O_P2R_I: {
            /* ByteSel 41-42; Ccpr (bit 40) moves the flags Z N C V instead of P0-P6 (Ryujinx InstP2r*, EmitP2r, which
               also keeps the mask to a byte) */
            char* M = T("(%s & 0xFFu)", opB(c, w, id - O_P2R_R));
            u32 sh = B(w, 41, 2) * 8;
            int ccpr = B(w, 40, 1);
            static const char* fl[4] = {"cc_z", "cc_s", "cc_c", "cc_o"};
            Sb e = {0};
            sb_put(&e, "(0u");
            for (int i = 0; i < (ccpr ? 4 : 7); i++) sb_f(&e, " | (%s ? %du : 0u)", ccpr ? fl[i] : P(c, i), 1 << i);
            sb_put(&e, ")");
            if (ccpr) c->uses_cc = 1;
            setr(c, d, T("((%s & ~(%s << %uu)) | ((%s & %s) << %uu))", R(c, a), M, sh, e.p, M, sh));
            free(e.p);
            break;
        }
        case O_R2P_R:
        case O_R2P_C:
        case O_R2P_I: {
            /* ByteSel 41-42, Ccpr 40 as for P2R (Ryujinx EmitR2p) */
            char* M = opB(c, w, id - O_R2P_R);
            u32 sh = B(w, 41, 2) * 8;
            int ccpr = B(w, 40, 1);
            static const char* fl[4] = {"cc_z", "cc_s", "cc_c", "cc_o"};
            emitl(c, "{ uint v_ = %s >> %uu; uint m_ = %s;", R(c, a), sh, M);
            c->indent++;
            if (ccpr) c->uses_cc = 1;
            for (int i = 0; i < (ccpr ? 4 : 7); i++)
                emitl(c, "if((m_ & %du) != 0u) %s = (v_ & %du) != 0u;", 1 << i, ccpr ? fl[i] : P(c, i), 1 << i);
            c->indent--;
            emitl(c, "}");
            break;
        }
        case O_CSETP:  /* condition on CC: Ccc 8-12 (Ryujinx InstCsetp) */
        case O_CSET: { /* the same into a register, BVal 44 picks 1.0 over all ones (Ryujinx InstCset) */
            const char* t = cc_test(c, B(w, 8, 5));
            if (!t) {
                unsupported(c, id == O_CSET ? "CSET condition" : "CSETP condition", w);
                t = "false";
            }
            const char* pc = PN(c, B(w, 39, 3), B(w, 42, 1));
            if (id == O_CSET) {
                setr(
                    c, d,
                    T("(%s ? %s : 0u)", bop(B(w, 45, 2), t, pc), B(w, 44, 1) ? "floatBitsToUint(1.0)" : "0xFFFFFFFFu"));
                break;
            }
            setp(c, B(w, 3, 3), bop(B(w, 45, 2), t, pc));
            setp(c, B(w, 0, 3), bop(B(w, 45, 2), T("(!%s)", t), pc));
            break;
        }
        /* ---------------- attribs + memory */
        case O_IPA: {
            u32 addr = B(w, 30, 8) * 4;
            const char* v = attr_read(c, addr, 0);
            if (B(w, 38, 1)) { /* Idx (Ryujinx InstIpa): the attribute indexed by a register */
                unsupported(c, "indexed IPA", w);
                v = "0u";
            }
            /* hw semantics: perspective generics come out pre-multiplied by w (1/gl_FragCoord.w). PASS keeps that,
       MULTIPLY multiplies by the given reg */
            else if (c->stage == ST_FS && addr >= 0x80 && addr < 0x280) {
                u32 gi = (addr - 0x80) >> 4, cp = (addr >> 2) & 3;
                u32 im = (c->si->sph[6 + gi / 4] >> ((gi % 4) * 8 + cp * 2)) & 3;
                u32 mode = B(w, 54, 2);
                /* IPA.MULTIPLY by RCP(IPA pos.w) cancels out, just read the varying */
                if (im == 2 && mode == 1 && c->wtag[B(w, 20, 8)] == 2 && B(w, 20, 8) != 255) {
                } else {
                    if (im == 2) v = T("floatBitsToUint(uintBitsToFloat(%s)*(1.0/gl_FragCoord.w))", v);
                    if (mode == 1)
                        v = T("floatBitsToUint(uintBitsToFloat(%s)*uintBitsToFloat(%s))", v, R(c, B(w, 20, 8)));
                }
            } else if (c->stage == ST_FS && addr == 0x7C && B(w, 54, 2) == 0 && !B(w, 51, 1)) {
                c->tag_r = (int)d;
                c->tag_v = 1;
            }
            if (B(w, 51, 1)) v = T("floatBitsToUint(clamp(uintBitsToFloat(%s),0.0,1.0))", v);
            setr(c, d, v);
            break;
        }
        case O_ALD: {
            u32 addr = B(w, 20, 10), n = B(w, 47, 2) + 1;
            int is_out = B(w, 32, 1);
            if (B(w, 8, 8) != 255) { /* indexed attribs: only generic-by-register */
                for (u32 k = 0; k < n; k++) setr(c, d + k, "0u");
                unsupported(c, "indirect ALD", w);
                break;
            }
            for (u32 k = 0; k < n; k++) setr(c, d + k, attr_read(c, addr + k * 4, is_out));
            break;
        }
        case O_AST: {
            u32 addr = B(w, 20, 10), n = B(w, 47, 2) + 1;
            u32 ra = B(w, 8, 8);
            if (ra != 255) { /* runtime-indexed output */
                emitl(c, "{ uint aa_ = %s + %uu;", R(c, ra), addr);
                c->indent++;
                for (u32 k = 0; k < n; k++) {
                    const char* v = R(c, d + k);
                    emitl(c, "switch(aa_ + %uu){", k * 4);
                    c->indent++;
                    for (u32 q = 0; q < 4; q++)
                        emitl(c, "case %uu: gl_Position.%c = uintBitsToFloat(%s); break;", 0x70 + q * 4, "xyzw"[q], v);
                    /* the generics the program header says it writes: OmapGenericVector, 4 bits per generic from
                       bit 432 of the SPH (checked against every VertexB program Catherine ships) */
                    for (u32 i = 0; i < 32; i++) {
                        u32 bit = 432 + i * 4;
                        if (!((c->si->sph[bit >> 5] >> (bit & 31)) & 15)) continue;
                        c->out_used[i] = 1;
                        for (u32 q = 0; q < 4; q++)
                            emitl(c, "case %uu: out_attr%u.%c = uintBitsToFloat(%s); break;", 0x80 + i * 16 + q * 4, i,
                                  "xyzw"[q], v);
                    }
                    emitl(c, "default: break;");
                    c->indent--;
                    emitl(c, "}");
                }
                c->indent--;
                emitl(c, "}");
                break;
            }
            for (u32 k = 0; k < n; k++) attr_write(c, addr + k * 4, R(c, d + k));
            break;
        }
        case O_LDC: {
            u32 idx = B(w, 36, 5);
            i32 off = (i32)(B(w, 20, 16) << 16) >> 16;
            u32 sz = B(w, 48, 3);
            u32 ar = a;
            c->si->cbuf_mask |= 1u << idx;
            if (idx < 18) { /* a register base can reach the whole buffer, a constant address only what it reads */
                u32 end = sz == 5 ? 8 : 4;
                if (ar != 255 || off < 0 || (u32)off + end > 65536)
                    c->si->cbuf_max[idx] = 65536;
                else if (((((u32)off + end - 1) | 15) + 1) > c->si->cbuf_max[idx])
                    c->si->cbuf_max[idx] = (((u32)off + end - 1) | 15) + 1;
            }
            const char* addr = ar == 255 ? T("%uu", (u32)off) : T("(%s + %uu)", R(c, ar), (u32)off);
            /* compute the address first, dst can alias the address reg */
            emitl(c, "{ uint ca_ = %s;", addr);
            c->indent++;
            addr = "ca_";
            const char* word = T("floatBitsToUint(cb%u.d[(%s)>>4][((%s)>>2)&3u])", idx, addr, addr);
            if (sz == 4)
                setr(c, d, word);
            else if (sz == 5) {
                setr(c, d, word);
                setr(c, d + 1, T("floatBitsToUint(cb%u.d[((%s)+4u)>>4][(((%s)+4u)>>2)&3u])", idx, addr, addr));
            } else if (sz == 0)
                setr(c, d, T("bitfieldExtract(%s, int(((%s)&3u)*8u), 8)", word, addr));
            else if (sz == 1)
                setr(c, d, T("uint(bitfieldExtract(int(%s), int(((%s)&3u)*8u), 8))", word, addr));
            else if (sz == 2)
                setr(c, d, T("bitfieldExtract(%s, int(((%s)&2u)*8u), 16)", word, addr));
            else if (sz == 3)
                setr(c, d, T("uint(bitfieldExtract(int(%s), int(((%s)&2u)*8u), 16))", word, addr));
            else
                setr(c, d, word);
            c->indent--;
            emitl(c, "}");
            break;
        }
        case O_LDS:
        case O_STS:
        case O_LDL:
        case O_STL: {
            int shared = id == O_LDS || id == O_STS;
            int load = id == O_LDS || id == O_LDL;
            i32 off = (i32)(B(w, 20, 24) << 8) >> 8;
            u32 sz = B(w, 48, 3);
            const char* mem = shared ? "smem" : "lmem";
            if (shared)
                c->uses_shared = 1;
            else
                c->uses_local = 1;
            const char* addr = a == 255 ? T("%uu", (u32)off) : T("(%s + %uu)", R(c, a), (u32)off);
            /* LsSize (Ryujinx InstLds/Ldl/Sts/Stl 48-50): 0 U8, 1 S8, 2 U16, 3 S16, 4 B32, 5 B64, 6 B128, 7 UB128 */
            u32 n = sz == 5 ? 2 : sz >= 6 ? 4 : 1;
            emitl(c, "{ uint la_ = %s;", addr);
            c->indent++;
            addr = "la_";
            const char* bo = sz < 2 ? "int((la_&3u)*8u)" : "int((la_&2u)*8u)"; /* the byte or halfword in its word */
            for (u32 k = 0; k < n; k++) {
                const char* ix = T("(((%s)>>2)+%uu) & %uu", addr, k, shared ? 0xFFFF : lmem_words(c) - 1);
                if (load) {
                    if (sz < 4)
                        setr(c, d + k,
                             (sz & 1) ? T("uint(bitfieldExtract(int(%s[%s]), %s, %d))", mem, ix, bo, sz < 2 ? 8 : 16)
                                      : T("bitfieldExtract(%s[%s], %s, %d)", mem, ix, bo, sz < 2 ? 8 : 16));
                    else
                        setr(c, d + k, T("%s[%s]", mem, ix));
                } else if (sz < 4) { /* only the byte or halfword: shared memory with atomics, its neighbours can
                                        be other threads' */
                    const char* m = sz < 2 ? "0xFFu" : "0xFFFFu";
                    if (shared)
                        emitl(
                            c,
                            "{ uint s_ = uint(%s); atomicAnd(smem[%s], ~(%s<<s_)); atomicOr(smem[%s], (%s & %s)<<s_); }",
                            bo, ix, m, ix, R(c, d), m);
                    else
                        emitl(c, "lmem[%s] = bitfieldInsert(lmem[%s], %s, %s, %d);", ix, ix, R(c, d), bo,
                              sz < 2 ? 8 : 16);
                } else
                    emitl(c, "%s[%s] = %s;", mem, ix, R(c, d + k));
            }
            c->indent--;
            emitl(c, "}");
            break;
        }
        case O_LDG:
            if (c->gtag[a]) {
                /* ssbo load, addr = c0[off] + offset */
                int gi = c->gtag[a] - 1;
                u32 goff = c->si->gbuf_cbuf_off[gi];
                i32 imm = (i32)(B(w, 20, 24) << 8) >> 8;
                u32 sz = B(w, 48, 3);
                emitl(c, "{ uint o_ = %s + %uu - %s;", R(c, a), (u32)imm, CB(c, 0, goff));
                c->indent++;
                if (sz <= 3) {
                    const char* v = T("((g%d.d[o_>>2u] >> ((o_&3u)*8u)) & %s)", gi, sz <= 1 ? "0xFFu" : "0xFFFFu");
                    if (sz == 1)
                        v = T("uint(int(%s<<24u)>>24)", v);
                    else if (sz == 3)
                        v = T("uint(int(%s<<16u)>>16)", v);
                    setr(c, d, v);
                } else {
                    u32 n = sz == 5 ? 2 : sz >= 6 ? 4 : 1;
                    for (u32 k = 0; k < n && d != 255; k++) setr(c, d + k, T("g%d.d[(o_>>2u)+%uu]", gi, k));
                }
                c->indent--;
                emitl(c, "}");
                break;
            }
            /* unknown base -> unsupported */
        case O_STG:
            if (id == O_STG && c->gtag[a]) {
                /* ssbo store: 32/64/128 direct, 8/16 via atomics so neighbours survive */
                int gi = c->gtag[a] - 1;
                u32 goff = c->si->gbuf_cbuf_off[gi];
                i32 imm = (i32)(B(w, 20, 24) << 8) >> 8;
                u32 sz = B(w, 48, 3);
                c->si->gbuf_wmask |= 1u << gi;
                emitl(c, "{ uint o_ = %s + %uu - %s;", R(c, a), (u32)imm, CB(c, 0, goff));
                c->indent++;
                if (sz <= 3) {
                    const char* m = sz <= 1 ? "0xFFu" : "0xFFFFu";
                    emitl(
                        c,
                        "uint s_=(o_&3u)*8u; atomicAnd(g%d.d[o_>>2u], ~(%s<<s_)); atomicOr(g%d.d[o_>>2u], (%s & %s)<<s_);",
                        gi, m, gi, R(c, d), m);
                } else {
                    u32 n = sz == 5 ? 2 : sz >= 6 ? 4 : 1;
                    /* RZ as the data of a wide store stores zeros in every word */
                    for (u32 k = 0; k < n; k++)
                        emitl(c, "g%d.d[(o_>>2u)+%uu] = %s;", gi, k, R(c, d == 255 ? 255 : d + k));
                }
                c->indent--;
                emitl(c, "}");
                break;
            }
            {
                unsupported(c, id == O_LDG ? "LDG" : "STG", w);
                if (id == O_LDG) {
                    u32 sz = B(w, 48, 3), n = sz == 5 ? 2 : sz >= 6 ? 4 : 1;
                    for (u32 k = 0; k < n; k++) setr(c, d + k, "0u");
                }
                break;
            }
        case O_RED: /* no result: value in bits 0-7, unsigned offset, size at 20, operation at 23 */
            global_atomic(c, w, "RED", B(w, 8, 8), B(w, 0, 8), (i32)B(w, 28, 20), B(w, 20, 3), B(w, 23, 3), 255);
            break;
        case O_ATOM: /* result in bits 0-7, value in 20-27, signed offset, size at 49, operation at 52 */
            global_atomic(c, w, "ATOM", a, B(w, 20, 8), (i32)(B(w, 28, 20) << 12) >> 12, B(w, 49, 3), B(w, 52, 4), d);
            break;
        case O_VMNMX: {
            /* VMNMX: min or max (bit 56) of A and B, each a byte, half or word picked and sign- or zero-extended
               (A select bits 36-38 with its sign at 48, B at 28-30 and 49; B is a register when bit 50 is set,
               else a 16-bit immediate at 20), saturated (55, signed result format at 54), then merged with,
               added to, or min/maxed with C (op at 51) */
            u32 asel = B(w, 36, 3) | (B(w, 48, 1) << 3), bsel = B(w, 28, 3) | (B(w, 49, 1) << 3);
            int as = asel >> 3, bs = bsel >> 3, mx = (int)B(w, 56, 1), sat = (int)B(w, 55, 1), dfmt = (int)B(w, 54, 1);
            u32 vop = B(w, 51, 3);
            const char *va, *vb;
#define VSEL(x, sel)                                                                                                   \
    ((sel & 7) < 4                                                                                                     \
         ? T((sel & 8) ? "uint(bitfieldExtract(int(%s), %u, 8))" : "bitfieldExtract(%s, %u, 8)", x, (sel & 7) * 8)     \
     : (sel & 7) < 6 ? T((sel & 8) ? "uint(bitfieldExtract(int(%s), %u, 16))" : "bitfieldExtract(%s, %u, 16)", x,      \
                         ((sel & 7) - 4) * 16)                                                                         \
                     : T("%s", x))
            va = VSEL(R(c, a), asel);
            if (B(w, 50, 1))
                vb = VSEL(R(c, B(w, 20, 8)), bsel);
            else {
                u32 imm = B(w, 20, 16);
                if (bs) imm = (u32)((i32)(imm << 16) >> 16);
                vb = T("%uu", imm);
            }
#undef VSEL
            /* a < b across signedness: a signed negative A is below any unsigned B, an unsigned B above INT_MAX
               above any signed A */
            const char* lt = as == bs ? T(as ? "(int(a_) < int(b_))" : "(a_ < b_)")
                             : as     ? "(int(a_) < 0 || a_ < b_)"
                                      : "(int(b_) >= 0 && a_ < b_)";
            int rs = as == bs ? as : !mx; /* the result's signedness */
            emitl(c, "{ uint a_ = %s; uint b_ = %s; uint r_ = %s ? %s : %s;", va, vb, lt, mx ? "b_" : "a_",
                  mx ? "a_" : "b_");
            c->indent++;
            if (sat && dfmt && !rs) emitl(c, "r_ = min(r_, 0x7FFFFFFFu);");
            if (sat && !dfmt && rs) emitl(c, "r_ = uint(max(int(r_), 0));");
            const char* C = R(c, B(w, 39, 8));
            switch (vop) {
                case 0: emitl(c, "r_ = bitfieldInsert(%s, r_, 16, 16);", C); break;
                case 1: emitl(c, "r_ = bitfieldInsert(%s, r_, 0, 16);", C); break;
                case 2: emitl(c, "r_ = bitfieldInsert(%s, r_, 0, 8);", C); break;
                case 3: emitl(c, "r_ = bitfieldInsert(%s, r_, 16, 8);", C); break;
                case 4: emitl(c, "r_ = r_ + %s;", C); break;
                case 5: emitl(c, dfmt ? "r_ = uint(min(int(r_), int(%s)));" : "r_ = min(r_, %s);", C); break;
                case 6: emitl(c, dfmt ? "r_ = uint(max(int(r_), int(%s)));" : "r_ = max(r_, %s);", C); break;
                default: break;
            }
            if (d != 255) {
                emitl(c, "r%u = r_;", d);
                c->used_reg[d] = 1;
            }
            c->indent--;
            emitl(c, "}");
            break;
        }
        /* ---------------- textures */
        case O_TEXS: {
            u32 enc = B(w, 53, 4), ra = a, rb = B(w, 20, 8);
            u32 off = B(w, 36, 13);
            int type = 1, array = 0, shadow = 0;
            const char* crd = "vec2(0.0)";
            const char* lod = NULL;
            const char* dref = NULL;
            const char* arr = NULL;
            switch (enc) {
                case 0:
                    type = 0;
                    crd = RF(c, ra);
                    lod = "0.0";
                    break;
                case 1: crd = T("vec2(%s,%s)", RF(c, ra), RF(c, rb)); break;
                case 2:
                    crd = T("vec2(%s,%s)", RF(c, ra), RF(c, rb));
                    lod = "0.0";
                    break;
                case 3:
                    crd = T("vec2(%s,%s)", RF(c, ra), RF(c, ra + 1));
                    lod = RF(c, rb);
                    break;
                case 4:
                    shadow = 1;
                    crd = T("vec2(%s,%s)", RF(c, ra), RF(c, ra + 1));
                    dref = RF(c, rb);
                    break;
                case 5:
                    shadow = 1;
                    crd = T("vec2(%s,%s)", RF(c, ra), RF(c, ra + 1));
                    lod = RF(c, rb);
                    dref = RF(c, rb + 1);
                    break;
                case 6:
                    shadow = 1;
                    crd = T("vec2(%s,%s)", RF(c, ra), RF(c, ra + 1));
                    dref = RF(c, rb);
                    lod = "0.0";
                    break;
                case 7:
                    array = 1;
                    arr = R(c, ra);
                    crd = T("vec2(%s,%s)", RF(c, ra + 1), RF(c, rb));
                    break;
                case 8:
                    array = 1;
                    arr = R(c, ra);
                    crd = T("vec2(%s,%s)", RF(c, ra + 1), RF(c, rb));
                    lod = "0.0";
                    break;
                case 9:
                    array = 1;
                    shadow = 1;
                    arr = R(c, ra);
                    crd = T("vec2(%s,%s)", RF(c, ra + 1), RF(c, rb));
                    dref = RF(c, rb + 1);
                    lod = "0.0";
                    break;
                case 10:
                    type = 2;
                    crd = T("vec3(%s,%s,%s)", RF(c, ra), RF(c, ra + 1), RF(c, rb));
                    break;
                case 11:
                    type = 2;
                    crd = T("vec3(%s,%s,%s)", RF(c, ra), RF(c, ra + 1), RF(c, rb));
                    lod = "0.0";
                    break;
                case 12:
                    type = 3;
                    crd = T("vec3(%s,%s,%s)", RF(c, ra), RF(c, ra + 1), RF(c, rb));
                    break;
                case 13:
                    type = 3;
                    crd = T("vec3(%s,%s,%s)", RF(c, ra), RF(c, ra + 1), RF(c, rb));
                    lod = RF(c, rb + 1);
                    break; /* cube + LL: z in rb, lod in rb+1 (matches what NVN's compiler emits) */
            }
            int s = tex_slot(c, off, type, array, shadow);
            if (type == 0) crd = T("vec2(%s, 0.5)", crd);
            if (array) crd = T("vec3(%s, float(%s))", crd, arr);
            const char* smp;
            if (shadow) {
                const char* cc = array ? T("vec4(%s,%s)", crd, dref) : T("vec3(%s,%s)", crd, dref);
                smp = lod ? T("vec4(textureLod(tex%d, %s, %s))", s, array ? T("%s", cc) : cc, lod)
                          : T("vec4(texture(tex%d, %s))", s, cc);
                if (lod && !array) smp = T("vec4(textureLod(tex%d, vec3(%s,%s), %s))", s, crd, dref, lod);
                if (array) { /* an array's depth compare at a level needs GL_EXT_texture_shadow_lod */
                    smp = lod ? T("vec4(textureLod(tex%d, vec4(%s,%s), %s))", s, crd, dref, lod)
                              : T("vec4(texture(tex%d, vec4(%s,%s)))", s, crd, dref);
                    if (lod) c->uses_shadow_lod = 1;
                }
            } else
                smp = lod ? T("textureLod(tex%d, %s, %s)", s, crd, lod) : T("texture(tex%d, %s)", s, crd);
            emitl(c, "{ vec4 t_ = %s;", IW(c, s, smp));
            c->indent++;
            write_texs(c, w, "t_", !B(w, 59, 1));
            c->indent--;
            emitl(c, "}"); /* bit59: fp32 */
            break;
        }
        case O_SULD:
        case O_SUST: {
            /* compute images. SUST -> imageStore, SULD -> texelFetch on the same texture.
       .P = formatted (mask 20..23), .D = raw bytes (size 20..22) converted using the format class
       uniforms snx_ic / snx_tc the runtime sets at dispatch */
            u32 isd = B(w, 52, 1), bound = B(w, 51, 1), ty = B(w, 33, 3), ra = a, rdd = d;
            if (c->stage != ST_CS || ty == 1 || ty > 5) {
                unsupported(c, id == O_SUST ? "SUST" : "SULD", w);
                if (id == O_SULD && rdd != 255) { setr(c, rdd, "0u"); }
                break;
            }
            u32 off = 0, bank = 0;
            int bl = 0;
            if (bound)
                off = B(w, 36, 13);
            else {
                u32 bo;
                if (!track_handle(c, idx, B(w, 39, 8), &bank, &bo)) {
                    unsupported(c, id == O_SUST ? "SUST.B (untracked handle)" : "SULD.B (untracked handle)", w);
                    if (id == O_SULD) setr(c, rdd, "0u");
                    break;
                }
                off = bo / 4;
                bl = 1;
                if (id == O_SUST && c->bl2) { /* the image list has no place for the second read of a split handle */
                    unsupported(c, "SUST.B (split handle)", w);
                    break;
                }
            }
            int ba = isd && B(w, 23, 1); /* x is in bytes, divide by texel size */
            /* Size 20-22 (Ryujinx InstSuldD/SustD, SuSize): 5 B64 two words, 6 B128 and 7 UB128 four */
            u32 sz = B(w, 20, 3), nw = isd ? (sz == 5 ? 2 : sz >= 6 ? 4 : 1) : 0;
            const char* crd = su_coords(c, ty, ra, T("int(%s)", R(c, ra)), 0);
            if (id == O_SUST) {
                ShaderInfo* si = c->si;
                int k = 0;
                for (; k < si->nimg; k++)
                    if (si->img[k].cbuf_off == off && si->img[k].type == ty && si->img[k].bindless == bl &&
                        (!bl || si->img[k].bl_cbuf == bank))
                        break;
                if (k == si->nimg) {
                    if (si->nimg >= 8) {
                        unsupported(c, "SUST (too many images)", w);
                        break;
                    }
                    si->img[k].cbuf_off = off;
                    si->img[k].type = (u8)ty;
                    si->img[k].bindless = (u8)bl;
                    si->img[k].bl_cbuf = (u8)bank;
                    si->nimg++;
                    if (bl && bank < 18) {
                        si->cbuf_mask |= 1u << bank;
                        if (off * 4 + 16 > si->cbuf_max[bank]) si->cbuf_max[bank] = ((off * 4) | 15) + 1;
                    }
                }
                if (ba) {
                    c->uses_img_raw = 1;
                    crd = su_coords(c, ty, ra, T("int(%s) / snx_ib[%d]", R(c, ra), k), 0);
                }
                if (!isd) {
                    u32 m = B(w, 20, 4), r = rdd;
                    const char* cc[4] = {"0.0", "0.0", "0.0", "1.0"};
                    for (int q = 0; q < 4; q++)
                        if (m & (1u << q)) {
                            cc[q] = r == 255 ? "0.0" : T("uintBitsToFloat(%s)", R(c, r));
                            if (r != 255) r++;
                        }
                    emitl(c, "imageStore(img%d, %s, vec4(%s, %s, %s, %s));", k, crd, cc[0], cc[1], cc[2], cc[3]);
                } else {
                    c->uses_img_raw = 1;
                    emitl(c, "imageStore(img%d, %s, snx_r2v(uvec4(%s, %s, %s, %s), %uu, %uu, snx_ic[%d]));", k, crd,
                          R(c, rdd), nw > 1 ? R(c, rdd + 1) : "0u", nw > 2 ? R(c, rdd + 2) : "0u",
                          nw > 2 ? R(c, rdd + 3) : "0u", nw, sz, k);
                }
            } else {
                int tt = ty == 5 ? 2 : (ty == 0 || ty == 2) ? 0 : 1, arr = ty == 2 || ty == 4;
                int ts = tex_slot2(c, off, tt, arr, 0, bl, bank);
                if (ba) c->uses_img_raw = 1;
                /* read through a sampler, where 1D is a 2D texture */
                crd = su_coords(c, ty, ra, ba ? T("int(%s) / snx_tb[%d]", R(c, ra), ts) : T("int(%s)", R(c, ra)), 1);
                const char* f = T("texelFetch(tex%d, %s, 0)", ts, crd);
                if (!isd) {
                    emitl(c, "{ vec4 t_ = %s;", f);
                    c->indent++;
                    write_masked(c, rdd, B(w, 20, 4), "t_");
                    c->indent--;
                    emitl(c, "}");
                } else {
                    c->uses_img_raw = 1;
                    emitl(c, "{ uvec4 t_ = snx_v2r(%s, %uu, snx_tc[%d]);", f, sz, ts);
                    c->indent++;
                    for (u32 q = 0; q < nw; q++)
                        if (rdd != 255) setr(c, rdd + q, T("t_.%c", "xyzw"[q]));
                    c->indent--;
                    emitl(c, "}");
                }
            }
            break;
        }
        case O_TLDS: {
            u32 enc = B(w, 53, 4), ra = a, rb = B(w, 20, 8);
            u32 off = B(w, 36, 13);
            int type = 1, array = 0;
            const char* crd;
            const char* lod = "0";
            const char* ofs = NULL;
            switch (enc) {
                case 0:
                    type = 0;
                    crd = T("int(%s)", R(c, ra));
                    break;
                case 1:
                    type = 0;
                    crd = T("int(%s)", R(c, ra));
                    lod = T("int(%s)", R(c, rb));
                    break;
                case 2: crd = T("ivec2(%s,%s)", R(c, ra), R(c, rb)); break;
                case 4:
                    crd = T("ivec2(%s,%s)", R(c, ra), R(c, ra + 1));
                    ofs = R(c, rb);
                    break;
                case 5:
                    crd = T("ivec2(%s,%s)", R(c, ra), R(c, ra + 1));
                    lod = T("int(%s)", R(c, rb));
                    break;
                case 6: crd = T("ivec2(%s,%s)", R(c, ra), R(c, ra + 1)); break;
                case 7:
                    type = 2;
                    crd = T("ivec3(%s,%s,%s)", R(c, ra), R(c, ra + 1), R(c, rb));
                    break;
                case 8:
                    array = 1;
                    crd = T("ivec3(%s,%s,%s)", R(c, ra + 1), R(c, rb), R(c, ra));
                    break;
                case 12:
                    crd = T("ivec2(%s,%s)", R(c, ra), R(c, ra + 1));
                    lod = T("int(%s)", R(c, rb));
                    ofs = R(c, rb + 1);
                    break;
                default: crd = T("ivec2(%s,%s)", R(c, ra), R(c, rb)); break;
            }
            int s = tex_slot(c, off, type, array, 0);
            if (type == 0) crd = T("ivec2(%s, 0)", crd);
            const char* smp =
                ofs ? T("texelFetch(tex%d, %s + ivec2(bitfieldExtract(int(%s),0,4),bitfieldExtract(int(%s),4,4)), %s)",
                        s, crd, ofs, ofs, lod)
                    : T("texelFetch(tex%d, %s, %s)", s, crd, lod);
            emitl(c, "{ vec4 t_ = %s;", IW(c, s, smp));
            c->indent++;
            write_texs(c, w, "t_", !B(w, 59, 1));
            c->indent--;
            emitl(c, "}"); /* bit59: fp32 */
            break;
        }
        case O_TLD4S: {
            u32 ra = a, rb = B(w, 20, 8);
            u32 off = B(w, 36, 13);
            int dc = B(w, 50, 1), ao = B(w, 51, 1);
            u32 comp = B(w, 52, 2);
            const char* crd = T("vec2(%s,%s)", RF(c, ra), RF(c, rb));
            const char* ofs = NULL;
            const char* dref = NULL;
            if (ao || dc) {
                crd = T("vec2(%s,%s)", RF(c, ra), RF(c, ra + 1));
                if (ao) {
                    ofs = R(c, rb);
                    if (dc) dref = RF(c, rb + 1);
                } else
                    dref = RF(c, rb);
            }
            int s = tex_slot(c, off, 1, 0, dc);
            const char* smp;
            if (dc)
                smp =
                    ofs ? T("textureGatherOffset(tex%d, %s, %s, ivec2(bitfieldExtract(int(%s),0,6),bitfieldExtract(int(%s),8,6)))",
                            s, crd, dref, ofs, ofs)
                        : T("textureGather(tex%d, %s, %s)", s, crd, dref);
            else
                smp =
                    ofs ? T("textureGatherOffset(tex%d, %s, ivec2(bitfieldExtract(int(%s),0,6),bitfieldExtract(int(%s),8,6)), %u)",
                            s, crd, ofs, ofs, comp)
                        : T("textureGather(tex%d, %s, %u)", s, crd, comp);
            emitl(c, "{ vec4 t_ = %s;", IW(c, s, smp));
            c->indent++;
            {
                u32 da = d, db = B(w, 28, 8);
                if (B(w, 55, 1)) {
                    setr(c, da, "packHalf2x16(t_.xy)");
                    setr(c, db, "packHalf2x16(t_.zw)");
                } else {
                    setrf(c, da, "t_.x");
                    if (da != 255) setrf(c, da + 1, "t_.y");
                    if (db != 255) {
                        setrf(c, db, "t_.z");
                        setrf(c, db + 1, "t_.w");
                    }
                }
            }
            c->indent--;
            emitl(c, "}");
            break;
        }
        case O_TEX:
        case O_TEX_B:
        case O_TLD4:
        case O_TLD:
        case O_TMML:
        case O_TXD: {
            int eid = id == O_TEX_B ? O_TEX : id, bl = id == O_TEX_B;
            u32 blbank = 0, bloff = 0;
            u32 ty = B(w, 28, 3), mask = B(w, 31, 4), off = B(w, 36, 13);
            u32 cr = a, mr = B(w, 20, 8);
            if (bl) {
                if (!track_handle(c, idx, mr, &blbank, &bloff)) {
                    unsupported(c, "TEX.B (untracked handle)", w);
                    write_masked(c, d, mask ? mask : 15, "vec4(0.0)");
                    break;
                }
                off = bloff / 4;
                mr++;
            }
            int type = (int)(ty >> 1), array = ty & 1;
            if (type > 3) type = 3;
            int dc = 0, aoffi = 0, ptp = 0, ms = 0;
            u32 blod = 0;
            if (eid == O_TEX) {
                /* Ryujinx InstTex: Dc 50, Aoffi 54, Lod 55-57; InstTexB: Dc 50, Aoffib 36, Lodb 37-39 */
                dc = B(w, 50, 1);
                aoffi = bl ? B(w, 36, 1) : B(w, 54, 1);
                blod = bl ? B(w, 37, 3) : B(w, 55, 3);
            } else if (eid == O_TLD4) {
                /* Ryujinx InstTld4: Dc 50, Toff 54-55 (1 AOFFI, 2 PTP), TexComp 56-57 */
                dc = B(w, 50, 1);
                aoffi = B(w, 54, 2) == 1;
                ptp = B(w, 54, 2) == 2;
            } else if (eid == O_TLD) {
                /* Ryujinx InstTld: Lod 55, Toff 35, Ms 50 */
                blod = B(w, 55, 1) ? 3 : 1;
                aoffi = B(w, 35, 1);
                ms = B(w, 50, 1);
            } else if (eid == O_TXD)
                aoffi = B(w, 35, 1); /* Ryujinx InstTxd: Toff 35 */
            int dim = type == 0 ? 1 : type == 1 ? 2 : 3, ncrd = dim;
            const char* arr = NULL;
            u32 r = cr;
            if (array && eid != O_TXD) { /* TXD has the array index after the coordinates */
                arr = R(c, r);
                r++;
            }
            const char* crd;
            if (eid == O_TLD) {
                crd = dim == 1   ? T("int(%s)", R(c, r))
                      : dim == 2 ? T("ivec2(%s,%s)", R(c, r), R(c, r + 1))
                                 : T("ivec3(%s,%s,%s)", R(c, r), R(c, r + 1), R(c, r + 2));
            } else
                crd = dim == 1   ? RF(c, r)
                      : dim == 2 ? T("vec2(%s,%s)", RF(c, r), RF(c, r + 1))
                                 : T("vec3(%s,%s,%s)", RF(c, r), RF(c, r + 1), RF(c, r + 2));
            u32 m = mr;
            const char* lod = NULL;
            const char* bias = NULL;
            const char* ofs = NULL;
            const char* dref = NULL;
            int ofs_const = 1;
            if (eid == O_TXD) {
                /* after the coordinates a register with the array index (bits 0-15) and the offsets (4 bits each
                   from bit 16); the derivatives from rb, dPdx then dPdy (Ryujinx EmitTxd) */
                u32 pk = r + dim;
                if (array) arr = T("(%s & 0xFFFFu)", R(c, pk));
                if (aoffi) ofs = tex_offsets(c, idx, pk, ncrd, 16, 4, 4, type == 0, &ofs_const);
            } else {
                /* rb on: the LOD or bias, the offsets, the depth reference, the sample (Ryujinx EmitTex, EmitTld4) */
                if (blod == 2 || blod == 6) {
                    bias = RF(c, m);
                    m++;
                } else if (blod == 3 || blod == 7) {
                    lod = eid == O_TLD ? T("int(%s)", R(c, m)) : RF(c, m);
                    m++;
                } else if (blod == 1) {
                    lod = eid == O_TLD ? "0" : "0.0";
                }
                if (aoffi) {
                    /* TEX/TLD: 4 bits per coordinate, TLD4: 6 bits in each byte */
                    ofs = eid == O_TLD4 ? tex_offsets(c, idx, m, ncrd, 0, 8, 6, type == 0, &ofs_const)
                                        : tex_offsets(c, idx, m, ncrd, 0, 4, 4, type == 0, &ofs_const);
                    m++;
                } else if (ptp) { /* four texels' offsets in two registers: textureGatherOffsets takes constants */
                    int k0, k1;
                    const char* o0 = tex_offsets(c, idx, m, 2, 0, 8, 6, 0, &k0);
                    const char* o1 = tex_offsets(c, idx, m, 2, 16, 8, 6, 0, &k0);
                    const char* o2 = tex_offsets(c, idx, m + 1, 2, 0, 8, 6, 0, &k1);
                    const char* o3 = tex_offsets(c, idx, m + 1, 2, 16, 8, 6, 0, &k1);
                    if (k0 && k1 && type == 1)
                        ofs = T("ivec2[4](%s, %s, %s, %s)", o0, o1, o2, o3);
                    else
                        unsupported(c, "TLD4.PTP (offsets not constant)", w);
                    m += 2;
                }
                if (dc) {
                    dref = RF(c, m);
                    m++;
                }
                if (ms) /* no multisampled texture types here: sample 0's texel */
                    unsupported(c, "TLD.MS", w);
            }
            int s = tex_slot2(c, off, type, array, dc, bl, blbank);
            if (type == 0) {
                crd = eid == O_TLD ? T("ivec2(%s, 0)", crd) : T("vec2(%s, 0.5)", crd);
                dim = 2;
            }
            const char* crd0 = crd; /* textureQueryLod takes no array index */
            if (array && eid != O_TLD) crd = T("vec%d(%s, float(%s))", dim + 1, crd, arr);
            if (array && eid == O_TLD) crd = T("ivec%d(%s, int(%s))", dim + 1, crd, arr);
            /* the sampling functions take constant offsets only, and none on cube maps */
            if (ofs && eid != O_TLD && ((eid != O_TLD4 && !ofs_const) || type == 3)) {
                unsupported(c, type == 3 ? "texture offset on a cube map" : "texture offset not constant", w);
                ofs = NULL;
            }
            const char* smp;
            if (eid == O_TMML) {
                /* x the LOD the hardware computes, y the level it reads, 8.8 fixed point; z and w 0 (Ryujinx
                   EmitTmml: GLSL's order is the other way round) */
                emitl(c, "{ vec2 q_ = textureQueryLod(tex%d, %s);", s, crd0);
                c->indent++;
                static const char* tv[4] = {"uint(int(q_.y * 256.0))", "uint(int(q_.x * 256.0))", "0u", "0u"};
                u32 rr = d;
                for (int k = 0; k < 4; k++)
                    if ((mask ? mask : 15) & (1u << k)) setr(c, rr++, tv[k]);
                c->indent--;
                emitl(c, "}");
                break;
            } else if (eid == O_TLD) {
                /* integer coordinates: the offset is just added */
                if (ofs && array)
                    crd = T("(%s + ivec%d(%s, 0))", crd, dim + 1, ofs);
                else if (ofs)
                    crd = T("(%s + %s)", crd, ofs);
                smp = T("texelFetch(tex%d, %s, %s)", s, crd, lod ? lod : "0");
            } else if (eid == O_TLD4) {
                u32 comp = B(w, 56, 2);
                const char* fn = !ofs ? "textureGather" : ptp ? "textureGatherOffsets" : "textureGatherOffset";
                const char* o = ofs ? T(", %s", ofs) : "";
                smp =
                    dc ? T("%s(tex%d, %s, %s%s)", fn, s, crd, dref, o) : T("%s(tex%d, %s%s, %u)", fn, s, crd, o, comp);
            } else if (eid == O_TXD) {
                u32 rb = mr, nd = ncrd;
                const char *dx, *dy;
                if (nd == 1) {
                    dx = T("vec2(%s, 0.0)", RF(c, rb));
                    dy = T("vec2(%s, 0.0)", RF(c, rb + 1));
                } else if (nd == 2) {
                    dx = T("vec2(%s,%s)", RF(c, rb), RF(c, rb + 1));
                    dy = T("vec2(%s,%s)", RF(c, rb + 2), RF(c, rb + 3));
                } else {
                    dx = T("vec3(%s,%s,%s)", RF(c, rb), RF(c, rb + 1), RF(c, rb + 2));
                    dy = T("vec3(%s,%s,%s)", RF(c, rb + 3), RF(c, rb + 4), RF(c, rb + 5));
                }
                smp = ofs ? T("textureGradOffset(tex%d, %s, %s, %s, %s)", s, crd, dx, dy, ofs)
                          : T("textureGrad(tex%d, %s, %s, %s)", s, crd, dx, dy);
            } else {
                /* a cube array's depth reference goes separately, the others' after the coordinates. With an LOD,
                   a bias or an offset, the array and cube forms need GL_EXT_texture_shadow_lod */
                int sep = dc && type == 3 && array;
                const char* cc = dc && !sep ? T("vec%d(%s, %s)", dim + array + 1, crd, dref) : crd;
                const char* cmp = sep ? T(", %s", dref) : "";
                const char* o = ofs ? T(", %s", ofs) : "";
                const char* fo = ofs ? "Offset" : "";
                if (dc && (array || type == 3) && (lod || bias || ofs)) c->uses_shadow_lod = 1;
                if (lod)
                    smp = T("textureLod%s(tex%d, %s%s, %s%s)", fo, s, cc, cmp, lod, o);
                else if (bias)
                    smp = T("texture%s(tex%d, %s%s%s, %s)", fo, s, cc, cmp, o, bias);
                else
                    smp = T("texture%s(tex%d, %s%s%s)", fo, s, cc, cmp, o);
                if (dc) smp = T("vec4(%s)", smp);
            }
            emitl(c, "{ vec4 t_ = %s;", IW(c, s, smp));
            c->indent++;
            write_masked(c, d, mask ? mask : 15, "t_");
            c->indent--;
            emitl(c, "}");
            break;
        }
        case O_TXQ:
        case O_TXQ_B: {
            u32 q = B(w, 22, 6), mask = B(w, 31, 4), off = B(w, 36, 13);
            int bl = id == O_TXQ_B;
            u32 blbank = 0, bloff = 0;
            if (bl) {
                if (!track_handle(c, idx, a, &blbank, &bloff)) {
                    unsupported(c, "TXQ.B (untracked handle)", w);
                    write_masked(c, d, mask, "vec4(0.0)");
                    break;
                }
                off = bloff / 4;
                a++;
            }
            /* TXQ has no Dim of its own: the texture's type is what the program's other accesses to it use (2D when
               none does, or for a bindless handle) */
            int dm = bl ? -1 : tex_dim_of(c, off);
            int type = dm < 0 ? 1 : dm >> 1 > 3 ? 3 : dm >> 1, array = dm < 0 ? 0 : dm & 1;
            int s = tex_slot2(c, off, type, array, 0, bl, blbank);
            if (q == 1) {
                /* x width, y height, z depth or layers, w levels */
                int n3 = array || type == 2;
                emitl(c,
                      "{ ivec%d sz_ = textureSize(tex%d, int(%s)); uvec4 t_ = uvec4(uint(sz_.x), uint(sz_.y), %s, %s);",
                      n3 ? 3 : 2, s, R(c, a), n3 ? "uint(sz_.z)" : "1u", T("uint(textureQueryLevels(tex%d))", s));
                c->indent++;
                u32 r = d;
                for (int k = 0; k < 4; k++)
                    if (mask & (1u << k)) {
                        setr(c, r, T("t_.%c", "xyzw"[k]));
                        r++;
                    }
                c->indent--;
                emitl(c, "}");
            } else {
                u32 r = d;
                for (int k = 0; k < 4; k++)
                    if (mask & (1u << k)) {
                        setr(c, r, "0u");
                        r++;
                    }
            }
            break;
        }
        /* ---------------- control flow */
        case O_BRX: {
            i32 off = (i32)(B(w, 20, 24) << 8) >> 8;
            emitl(c,
                  "{ uint t_ = uint(%d + (int(%s) + (%d))/8); if(t_>=10u && ((t_-10u)&3u)==0u) t_++; pc = t_; break; }",
                  (int)idx + 1, R(c, a), off);
            break;
        }
        case O_BRA:
            emitl(c, "pc = %uu; break;", bra_target(w, idx));
            break; /* its condition is emit_guarded's */
        /* SSY/PBK/PCNT push a token and their target on the one CRS stack; SYNC/BRK/CONT pop entries down to the
           nearest token of their kind and jump to it, so a BRK or CONT out of an SSY drops the SSY entry. Resolved
           statically: push = nothing, pop = fixed jump. A pop that finds no entry of its kind falls through */
        case O_SSY:
        case O_PBK:
        case O_PCNT:
            if (!c->stk_static) {
                c->uses_crs = 1;
                emitl(c, "if(crs_sp < 32) crs[crs_sp++] = %uu;", bra_target(w, idx) | (u32)CRS_KIND(id) << 16);
            }
            break;
        case O_SYNC:
        case O_BRK:
        case O_CONT:
            if (c->stk_static)
                emitl(c, "pc = %uu; break;", c->pop_tgt[idx]);
            else {
                c->uses_crs = 1;
                emitl(
                    c,
                    "{ int s_ = crs_sp; while(s_ > 0 && (crs[s_-1] >> 16) != %uu) s_--; if(s_ > 0) { crs_sp = s_-1; pc = crs[crs_sp] & 0xFFFFu; break; } }",
                    (u32)CRS_KIND(id));
            }
            break;
        case O_EXIT: exit_epilogue(c); break;
        /* PRET/RET without CAL: RET goes to the PRET target = end of program (early exit in compute) */
        case O_PRET: break;
        case O_RET: exit_epilogue(c); break;
        case O_KIL: emitl(c, "discard;"); break;
        case O_NOP:
        case O_VOTE_VTG:
        case O_DEPBAR:
        case O_MEMBAR:
        case O_CCTL:
        case O_CCTLL:
        case O_IDE: break;
        case O_BAR:
            if (c->stage == ST_CS) emitl(c, "barrier();");
            break;
        case O_VOTE: {
            const char* pr = PN(c, B(w, 39, 3), B(w, 42, 1));
            u32 vop = B(w, 48, 2);
            setp(c, B(w, 45, 3), vop == 2 ? "true" : pr);
            setr(c, d, T("(%s ? 1u : 0u)", pr));
            break;
        }
        case O_SHFL: {
            /* a warp of one: the thread is lane 0 (S2R LaneId reads 0), so the lane it reads from is only there,
               and the predicate set, when that's lane 0; the value is its own either way. Ryujinx InstShfl: b is
               the 5-bit immediate at 20 with BFixShfl (bit 28), DestPred 48-50 */
            const char* b = B(w, 28, 1) ? T("%uu", B(w, 20, 5)) : R(c, B(w, 20, 8));
            setp(c, B(w, 48, 3), T("((%s & 31u) == 0u)", b));
            setr(c, d, R(c, a));
            break;
        }
        case O_FSWZADD: {
            /* derivatives the nouveau way: SHFL.BFLY + FSWZADD.
       0x99 = QUADOP(SUB,SUBR,SUB,SUBR) -> dFdx(Rb), 0xA5 = QUADOP(SUB,SUB,SUBR,SUBR) -> dFdy(Rb) */
            u32 q = B(w, 28, 8);
            u32 rb = B(w, 20, 8);
            if (c->stage == ST_FS && q == 0x99)
                setrf(c, d, T("dFdx(%s)", RF(c, rb)));
            else if (c->stage == ST_FS && q == 0xA5)
                setrf(c, d, T("dFdy(%s)", RF(c, rb)));
            else if (c->stage == ST_FS && q == 0x66)
                setrf(c, d, T("(-dFdx(%s))", RF(c, rb)));
            else if (c->stage == ST_FS && q == 0x5A)
                setrf(c, d, T("(-dFdy(%s))", RF(c, rb)));
            else
                setrf(c, d, T("(%s + %s)", RF(c, a), RF(c, rb)));
            break;
        }
        case O_OUT_R:
        case O_OUT_C:
        case O_OUT_I: {
            /* a geometry program's, and those aren't translated */
            unsupported(c, "OUT", w);
            setr(c, d, R(c, a));
            break;
        }
        case O_HADD2_R:
        case O_HMUL2_R:
        case O_HFMA2_R:
        case O_HADD2_C:
        case O_HMUL2_C:
        case O_HADD2_I:
        case O_HMUL2_I:
        case O_HADD2_32I:
        case O_HMUL2_32I:
        case O_HFMA2_32I:
        case O_HFMA2_I:
        case O_HFMA2_RC:
        case O_HFMA2_CR: {
            /* packed f16 (same approach as yuzu): swizzle, neg/abs, sat, merge */
            int isadd = id == O_HADD2_R || id == O_HADD2_C || id == O_HADD2_I || id == O_HADD2_32I;
            int isfma = id == O_HFMA2_R || id == O_HFMA2_32I || id == O_HFMA2_I || id == O_HFMA2_RC || id == O_HFMA2_CR;
            int is32 = id == O_HADD2_32I || id == O_HMUL2_32I || id == O_HFMA2_32I;
            u32 merge = is32 ? 0 : B(w, 49, 2), swa = B(w, 47, 2);
            int nega = 0, absa = 0, negb = 0, absb = 0, negc = 0, sat = 0;
            u32 swb = 0, swc = 0;
            const char* sb = NULL;
            const char* sc = NULL;
            int bimm = 0;
            if (isadd) {
                nega = B(w, 43, 1);
                absa = B(w, 44, 1);
                if (id == O_HADD2_R) {
                    negb = B(w, 31, 1);
                    absb = B(w, 30, 1);
                    swb = B(w, 28, 2);
                    sb = R(c, B(w, 20, 8));
                    sat = B(w, 32, 1);
                } else if (id == O_HADD2_C) {
                    negb = B(w, 56, 1);
                    absb = B(w, 54, 1);
                    swb = 1;
                    sb = CB(c, cb_idx(w), cb_off(w));
                    sat = B(w, 52, 1);
                } else if (id == O_HADD2_I) {
                    bimm = 1;
                    sat = B(w, 52, 1);
                } else {
                    nega = B(w, 56, 1);
                    absa = 0;
                    swa = B(w, 53, 2);
                    bimm = 2;
                    sat = B(w, 52, 1);
                }
            } else if (!isfma) {
                absa = B(w, 44, 1);
                if (id == O_HMUL2_R) {
                    negb = B(w, 31, 1);
                    absb = B(w, 30, 1);
                    swb = B(w, 28, 2);
                    sb = R(c, B(w, 20, 8));
                    sat = B(w, 32, 1);
                } else if (id == O_HMUL2_C) {
                    nega = B(w, 43, 1);
                    absb = B(w, 54, 1);
                    swb = 1;
                    sb = CB(c, cb_idx(w), cb_off(w));
                    sat = B(w, 52, 1);
                } else if (id == O_HMUL2_I) {
                    nega = B(w, 43, 1);
                    bimm = 1;
                    sat = B(w, 52, 1);
                } else {
                    absa = 0;
                    swa = B(w, 53, 2);
                    bimm = 2;
                    sat = B(w, 52, 1);
                }
            } else {
                if (id == O_HFMA2_R) {
                    negb = B(w, 31, 1);
                    negc = B(w, 30, 1);
                    swb = B(w, 28, 2);
                    swc = B(w, 35, 2);
                    sb = R(c, B(w, 20, 8));
                    sc = R(c, B(w, 39, 8));
                    sat = B(w, 32, 1);
                } else if (id == O_HFMA2_RC) {
                    negb = B(w, 56, 1);
                    negc = B(w, 51, 1);
                    swb = B(w, 53, 2);
                    swc = 1;
                    sb = R(c, B(w, 39, 8));
                    sc = CB(c, cb_idx(w), cb_off(w));
                    sat = B(w, 52, 1);
                } else if (id == O_HFMA2_CR) {
                    negb = B(w, 56, 1);
                    negc = B(w, 51, 1);
                    swb = 1;
                    swc = B(w, 53, 2);
                    sb = CB(c, cb_idx(w), cb_off(w));
                    sc = R(c, B(w, 39, 8));
                    sat = B(w, 52, 1);
                } else if (id == O_HFMA2_I) {
                    negc = B(w, 51, 1);
                    swc = B(w, 53, 2);
                    sc = R(c, B(w, 39, 8));
                    bimm = 1;
                    sat = B(w, 52, 1);
                } else {
                    negc = B(w, 52, 1);
                    swa = B(w, 53, 2);
                    swc = 0;
                    sc = R(c, d);
                    bimm = 2;
                }
            }
            if (bimm == 1) {
                u32 lo = ((B(w, 20, 9) << 6) | (B(w, 29, 1) << 15)), hi = ((B(w, 30, 9) << 6) | (B(w, 56, 1) << 15));
                sb = T("0x%Xu", lo | (hi << 16));
                swb = 0;
            } else if (bimm == 2) {
                sb = imm32(w);
                swb = 0;
            }
            static const char* swz[4] = {"unpackHalf2x16(%s)", "vec2(uintBitsToFloat(%s))", "unpackHalf2x16(%s).xx",
                                         "unpackHalf2x16(%s).yy"};
            emitl(c, "{");
            emitl(c, "vec2 ha=%s;", T(swz[swa & 3], R(c, a)));
            if (absa) emitl(c, "ha=abs(ha);");
            if (nega) emitl(c, "ha=-ha;");
            emitl(c, "vec2 hb=%s;", T(swz[swb & 3], sb));
            if (absb) emitl(c, "hb=abs(hb);");
            if (negb) emitl(c, "hb=-hb;");
            if (isfma) {
                emitl(c, "vec2 hc=%s;", T(swz[swc & 3], sc));
                if (negc) emitl(c, "hc=-hc;");
            }
            emitl(c, "vec2 hr=%s;", isadd ? "ha+hb" : isfma ? "ha*hb+hc" : "ha*hb");
            if (sat) emitl(c, "hr=clamp(hr,0.0,1.0);");
            if (merge == 1)
                setr(c, d, "floatBitsToUint(hr.x)");
            else if (merge == 2)
                setr(c, d, T("((%s & 0xFFFF0000u) | (packHalf2x16(hr) & 0xFFFFu))", R(c, d)));
            else if (merge == 3)
                setr(c, d, T("((%s & 0xFFFFu) | (packHalf2x16(hr) & 0xFFFF0000u))", R(c, d)));
            else
                setr(c, d, "packHalf2x16(hr)");
            emitl(c, "}");
            break;
        }
        case O_HSETP2_R:
        case O_HSETP2_C:
        case O_HSETP2_I: {
            /* packed f16 compare; h_and combines both halves */
            static const char* swz[4] = {"unpackHalf2x16(%s)", "vec2(uintBitsToFloat(%s))", "unpackHalf2x16(%s).xx",
                                         "unpackHalf2x16(%s).yy"};
            u32 cmp, hand, swb;
            int negb, absb;
            const char* sb;
            if (id == O_HSETP2_R) {
                cmp = B(w, 35, 4);
                hand = B(w, 49, 1);
                negb = B(w, 31, 1);
                absb = B(w, 30, 1);
                swb = B(w, 28, 2);
                sb = R(c, B(w, 20, 8));
            } else if (id == O_HSETP2_C) {
                cmp = B(w, 49, 4);
                hand = B(w, 53, 1);
                negb = B(w, 56, 1);
                absb = B(w, 54, 1);
                swb = 1;
                sb = CB(c, cb_idx(w), cb_off(w));
            } else {
                cmp = B(w, 49, 4);
                hand = B(w, 53, 1);
                negb = 0;
                absb = 0;
                swb = 0;
                u32 lo = ((B(w, 20, 9) << 6) | (B(w, 29, 1) << 15)), hi = ((B(w, 30, 9) << 6) | (B(w, 56, 1) << 15));
                sb = T("0x%Xu", lo | (hi << 16));
            }
            const char* pc = PN(c, B(w, 39, 3), B(w, 42, 1));
            u32 pa = B(w, 3, 3), pb = B(w, 0, 3), bo = B(w, 45, 2);
            emitl(c, "{");
            emitl(c, "vec2 ha=%s;", T(swz[B(w, 47, 2)], R(c, a)));
            if (B(w, 44, 1)) emitl(c, "ha=abs(ha);");
            if (B(w, 43, 1)) emitl(c, "ha=-ha;");
            emitl(c, "vec2 hb=%s;", T(swz[swb & 3], sb));
            if (absb) emitl(c, "hb=abs(hb);");
            if (negb) emitl(c, "hb=-hb;");
            const char* cx = fcmp_expr(cmp, "ha.x", "hb.x");
            const char* cy = fcmp_expr(cmp, "ha.y", "hb.y");
            if (hand)
                setp2(c, pa, pb, bo, T("(%s && %s)", cx, cy), pc);
            else {
                setp(c, pa, bop(bo, cx, pc));
                setp(c, pb, bop(bo, cy, pc));
            }
            emitl(c, "}");
            break;
        }
        default: {
            unsupported(c, op->name, w);
            /* zero what it would have written, so what reads it later sees no stale value: the destination at bits
               0-7 for the ones that have one there (stores, RED and SURED keep their data register there) */
            u32 n = 0;
            switch (id) {
                case O_LD: n = B(w, 53, 3) == 5 ? 2 : B(w, 53, 3) >= 6 ? 4 : 1; break; /* Ryujinx InstLd LsSize */
                case O_DADD_R:
                case O_DADD_C:
                case O_DADD_I:
                case O_DMUL_R:
                case O_DMUL_C:
                case O_DMUL_I:
                case O_DFMA_R:
                case O_DFMA_RC:
                case O_DFMA_CR:
                case O_DFMA_I:
                case O_DMNMX_R:
                case O_DMNMX_C:
                case O_DMNMX_I: n = 2; break;
                case O_TXA: n = (u32)__builtin_popcount(B(w, 31, 4)); break;
                case O_ATOM:
                case O_ATOMS:
                case O_ATOM_CAS:
                case O_ATOMS_CAS:
                case O_SUATOM:
                case O_SUATOM_CAS:
                case O_DSET_R:
                case O_DSET_C:
                case O_DSET_I:
                case O_HSET2_R:
                case O_HSET2_C:
                case O_HSET2_I:
                case O_VMNMX:
                case O_VMAD:
                case O_VADD:
                case O_ISBERD:
                case O_AL2P:
                case O_PIXLD:
                case O_B2R:
                case O_LEPC:
                case O_GETCRSPTR: n = 1; break;
            }
            for (u32 k = 0; k < n; k++) setr(c, d + k, "0u");
            break;
        }
    }
}
