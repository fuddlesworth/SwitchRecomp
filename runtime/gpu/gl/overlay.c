/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* touch controls overlay (Android, or the GLES backend on PC).
   drawn right before present. each finger sticks to one control; you can slide between buttons. */
#include "core/rt.h"
#include "gpu/gl/glfn.h"
#include "gpu/gl/overlay_font.h"
#include <math.h>

extern int g_gles;
int g_overlay_visible = 1;
static float g_opacity = 0.5f;

enum { C_BTN, C_RECT, C_STICK };
typedef struct {
    int kind;
    float ax, ay;
    int from_right, from_bottom;
    float r;
    float w, h;
    u64 bits;
    const char* label;
    int stick;
} Ctl;
/* units: 1u = screen height / 100 */
static const Ctl ctls[] = {
    /* sticks */
    {C_STICK, 21, 27, 0, 1, 15, 0, 0, 0, "", 0},
    {C_STICK, 46, 17, 1, 1, 12, 0, 0, 0, "", 1},
    /* ABXY diamond */
    {C_BTN, 10, 43, 1, 1, 6.5f, 0, 0, 1ull << 0, "A", -1},
    {C_BTN, 21, 32, 1, 1, 6.5f, 0, 0, 1ull << 1, "B", -1},
    {C_BTN, 21, 54, 1, 1, 6.5f, 0, 0, 1ull << 2, "X", -1},
    {C_BTN, 32, 43, 1, 1, 6.5f, 0, 0, 1ull << 3, "Y", -1},
    /* dpad */
    {C_BTN, 21, 71, 0, 1, 5.0f, 0, 0, 1ull << 13, "^", -1},
    {C_BTN, 21, 51, 0, 1, 5.0f, 0, 0, 1ull << 15, "v", -1},
    {C_BTN, 11, 61, 0, 1, 5.0f, 0, 0, 1ull << 12, "<", -1},
    {C_BTN, 31, 61, 0, 1, 5.0f, 0, 0, 1ull << 14, ">", -1},
    /* triggers */
    {C_RECT, 2, 3, 0, 0, 0, 18, 10, 1ull << 8, "ZL", -1},
    {C_RECT, 22, 3, 0, 0, 0, 18, 10, 1ull << 6, "L", -1},
    {C_RECT, 20, 3, 1, 0, 0, 18, 10, 1ull << 9, "ZR", -1},
    {C_RECT, 40, 3, 1, 0, 0, 18, 10, 1ull << 7, "R", -1},
    /* +/- go top center, placed separately */
    {C_BTN, -9, 7, 0, 0, 4.5f, 0, 0, 1ull << 11, "-", -1},
    {C_BTN, 9, 7, 0, 0, 4.5f, 0, 0, 1ull << 10, "+", -1},
    /* stick clicks */
    {C_BTN, 40, 12, 0, 1, 4.5f, 0, 0, 1ull << 4, "L3", -1},
    {C_BTN, 64, 30, 1, 1, 4.5f, 0, 0, 1ull << 5, "R3", -1},
};
#define NCTL ((int)(sizeof ctls / sizeof ctls[0]))
static int g_W = 1280, g_H = 720;
static void ctl_pos(const Ctl* c, float* x, float* y) {
    float u = g_H / 100.f;
    if (c == &ctls[14] || c == &ctls[15]) {
        *x = g_W * 0.5f + c->ax * u;
        *y = c->ay * u;
        return;
    }
    float cx = c->from_right ? g_W - c->ax * u : c->ax * u;
    float cy = c->from_bottom ? g_H - c->ay * u : c->ay * u;
    if (c->kind == C_RECT) {
        if (c->from_right) cx = g_W - c->ax * u - c->w * u;
    }
    *x = cx;
    *y = cy;
}

/* ---------------- input */
typedef struct {
    int active;
    int id;
    int ctl;
    float x, y, sx, sy;
} Ptr;
static Ptr ptrs[10];
static float stick_v[2][2];
static u64 g_ov_buttons;
static PMutex g_ovlock;
static int g_ovinit;
static int hit(float x, float y, float scale) {
    float u = g_H / 100.f;
    for (int i = 0; i < NCTL; i++) {
        const Ctl* c = &ctls[i];
        float cx, cy;
        ctl_pos(c, &cx, &cy);
        if (c->kind == C_RECT) {
            if (x >= cx - u && x <= cx + (c->w + 1) * u && y >= cy - u && y <= cy + (c->h + 1) * u) return i;
        } else {
            float r = c->r * u * (c->kind == C_STICK ? 1.25f : scale);
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return i;
        }
    }
    return -1;
}
static void recompute(void) {
    u64 b = 0;
    float sv[2][2] = {{0, 0}, {0, 0}};
    for (int k = 0; k < 10; k++) {
        Ptr* p = &ptrs[k];
        if (!p->active || p->ctl < 0) continue;
        const Ctl* c = &ctls[p->ctl];
        if (c->kind == C_STICK) {
            float cx, cy;
            ctl_pos(c, &cx, &cy);
            float r = c->r * (g_H / 100.f);
            float dx = (p->x - cx) / r, dy = (p->y - cy) / r;
            float m = sqrtf(dx * dx + dy * dy);
            if (m > 1) {
                dx /= m;
                dy /= m;
            }
            sv[c->stick][0] = dx;
            sv[c->stick][1] = dy;
        } else {
            int h = hit(p->x, p->y, 1.35f);
            if (h >= 0 && ctls[h].kind != C_STICK) b |= ctls[h].bits;
        }
    }
    g_ov_buttons = b;
    memcpy(stick_v, sv, sizeof sv);
}
/* action: 0 down, 1 move, 2 up, 3 cancel */
void overlay_touch(int id, int action, float x, float y) {
    if (!g_ovinit) {
        pm_init(&g_ovlock);
        g_ovinit = 1;
    }
    pm_lock(&g_ovlock);
    if (action == 3) {
        memset(ptrs, 0, sizeof ptrs);
    } else {
        int k;
        for (k = 0; k < 10; k++)
            if (ptrs[k].active && ptrs[k].id == id) break;
        if (action == 0) {
            g_overlay_visible = 1;
            if (k == 10)
                for (k = 0; k < 10; k++)
                    if (!ptrs[k].active) break;
            if (k < 10) {
                ptrs[k].active = 1;
                ptrs[k].id = id;
                ptrs[k].x = ptrs[k].sx = x;
                ptrs[k].y = ptrs[k].sy = y;
                ptrs[k].ctl = hit(x, y, 1.35f);
            }
        } else if (k < 10) {
            if (action == 1) {
                ptrs[k].x = x;
                ptrs[k].y = y;
            } else
                ptrs[k].active = 0;
        }
    }
    recompute();
    pm_unlock(&g_ovlock);
}
/* ORed into the real pad state */
void overlay_state(u64* buttons, i32* lx, i32* ly, i32* rx, i32* ry) {
    if (!g_ovinit) {
        pm_init(&g_ovlock);
        g_ovinit = 1;
    }
    pm_lock(&g_ovlock);
    *buttons |= g_ov_buttons;
    if (stick_v[0][0] || stick_v[0][1]) {
        *lx = (i32)(stick_v[0][0] * 32767);
        *ly = (i32)(-stick_v[0][1] * 32767);
    }
    if (stick_v[1][0] || stick_v[1][1]) {
        *rx = (i32)(stick_v[1][0] * 32767);
        *ry = (i32)(-stick_v[1][1] * 32767);
    }
    pm_unlock(&g_ovlock);
}
void overlay_set_opacity(float o) { g_opacity = o; }

/* ---------------- one-off message ("romfs missing" and such) */
static char g_msg[1024];
void overlay_message(const char* m) { strncpy(g_msg, m ? m : "", sizeof g_msg - 1); }

/* ---------------- drawing */
static GLuint prog, ftex, vao;
static GLint u_rect, u_col, u_kind, u_uv, u_px;
static const char* VS_BODY =
    "uniform vec4 rect; out vec2 lp; void main(){ vec2 c=vec2(float(gl_VertexID&1),float(gl_VertexID>>1)); lp=c*2.0-1.0; gl_Position=vec4(mix(rect.xy,rect.zw,c),0.0,1.0); }";
static const char* FS_BODY =
    "in vec2 lp; out vec4 o; uniform vec4 col; uniform int kind; uniform vec4 uv; uniform vec4 px; uniform sampler2D ft;\n"
    "void main(){ float a=1.0; if(kind==0){ float d=length(lp); float aa=px.x*2.0; a=1.0-smoothstep(1.0-aa,1.0,d); float ring=smoothstep(0.80-aa,0.80,d); a*=mix(0.55,1.0,ring); }\n"
    " else if(kind==1){ vec2 q=abs(lp)-vec2(1.0)+px.xy*6.0; float d=length(max(q,0.0))-px.x*6.0; a=1.0-smoothstep(-px.x*2.0,0.0,d); }\n"
    " else if(kind==2){ a=texture(ft, mix(uv.xy,uv.zw,lp*0.5+0.5)).r; }\n"
    " o=vec4(col.rgb, col.a*a); }";
static GLuint cmp(GLenum t, const char* s) {
    GLuint h = glCreateShader(t);
    glShaderSource(h, 1, &s, NULL);
    glCompileShader(h);
    GLint ok = 0;
    glGetShaderiv(h, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char lg[1024];
        glGetShaderInfoLog(h, 1024, NULL, lg);
        WARN("overlay: %s\n", lg);
    }
    return h;
}
static int ov_init(void) {
    if (prog) return 1;
    const char* hdr = g_gles ? "#version 300 es\nprecision highp float;\n" : "#version 330 core\n";
    char vs[1024], fs[2048];
    snprintf(vs, sizeof vs, "%s%s", hdr, VS_BODY);
    snprintf(fs, sizeof fs, "%s%s", hdr, FS_BODY);
    prog = glCreateProgram();
    glAttachShader(prog, cmp(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, cmp(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        WARN("overlay: link failed\n");
        return 0;
    }
    u_rect = glGetUniformLocation(prog, "rect");
    u_col = glGetUniformLocation(prog, "col");
    u_kind = glGetUniformLocation(prog, "kind");
    u_uv = glGetUniformLocation(prog, "uv");
    u_px = glGetUniformLocation(prog, "px");
    glGenTextures(1, &ftex);
    glBindTexture(GL_TEXTURE_2D, ftex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, OVF_W, OVF_H);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, OVF_W, OVF_H, GL_RED, GL_UNSIGNED_BYTE, ovf_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    {
        static const GLint sw[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw);
    }
    glGenVertexArrays(1, &vao);
    return 1;
}
static void quad(float x0, float y0, float x1, float y1, int kind, float r, float g, float b, float a) {
    float rc[4] = {x0 / g_W * 2 - 1, 1 - y1 / g_H * 2, x1 / g_W * 2 - 1, 1 - y0 / g_H * 2};
    glUniform4fv(u_rect, 1, rc);
    float c[4] = {r, g, b, a};
    glUniform4fv(u_col, 1, c);
    glUniform1i(u_kind, kind);
    float px[4] = {2.0f / (x1 - x0 > 1 ? x1 - x0 : 1), 2.0f / (y1 - y0 > 1 ? y1 - y0 : 1), 0, 0};
    glUniform4fv(u_px, 1, px); /* no glUniform2fv loaded, just use vec4 */
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void text(float x, float y, float size, const char* s, float r, float g, float b, float a, int center) {
    float cw = size * 0.5f, n = (float)strlen(s);
    if (center) x -= cw * n * 0.5f;
    for (; *s; s++, x += cw) {
        int ch = (u8)*s;
        if (ch < 32 || ch > 127) ch = '?';
        int i = ch - 32;
        float u0 = (float)((i % 16) * OVF_CW) / OVF_W, v0 = (float)((i / 16) * OVF_CH) / OVF_H;
        float uvr[4] = {u0, v0 + (float)OVF_CH / OVF_H, u0 + (float)OVF_CW / OVF_W, v0};
        glUniform4fv(u_uv, 1, uvr);
        quad(x, y - size * 0.5f, x + cw, y + size * 0.5f, 2, r, g, b, a);
    }
}
void overlay_draw(int w, int h) {
    if (!w || !h) return;
    g_W = w;
    g_H = h;
    if (!ov_init()) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparatei(0, GL_FUNC_ADD, GL_FUNC_ADD);
    glColorMaski(0, 1, 1, 1, 1);
    glUseProgram(prog);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ftex);
    glBindSampler(0, 0);
    glUniform1i(glGetUniformLocation(prog, "ft"), 0);
    float u = h / 100.f, op = g_opacity;
    if (g_overlay_visible) {
        u64 pressed = 0;
        float sv[2][2];
        if (g_ovinit) pm_lock(&g_ovlock);
        pressed = g_ov_buttons;
        memcpy(sv, stick_v, sizeof sv);
        if (g_ovinit) pm_unlock(&g_ovlock);
        for (int i = 0; i < NCTL; i++) {
            const Ctl* c = &ctls[i];
            float cx, cy;
            ctl_pos(c, &cx, &cy);
            int on = (pressed & c->bits) != 0;
            float br = on ? 1.0f : 0.85f, al = op * (on ? 1.3f : 1.0f);
            if (c->kind == C_STICK) {
                float r = c->r * u;
                quad(cx - r, cy - r, cx + r, cy + r, 0, 0.15f, 0.15f, 0.18f, op * 0.8f);
                float kr = r * 0.45f, kx = cx + sv[c->stick][0] * r * 0.6f, ky = cy + sv[c->stick][1] * r * 0.6f;
                quad(kx - kr, ky - kr, kx + kr, ky + kr, 0, 0.85f, 0.85f, 0.9f, op * 1.2f);
            } else if (c->kind == C_RECT) {
                quad(cx, cy, cx + c->w * u, cy + c->h * u, 1, 0.15f * br, 0.15f * br, 0.18f * br + (on ? 0.3f : 0),
                     al * 0.9f);
                text(cx + c->w * u * 0.5f, cy + c->h * u * 0.5f, c->h * u * 0.6f, c->label, 1, 1, 1, al * 1.4f, 1);
            } else {
                float r = c->r * u;
                quad(cx - r, cy - r, cx + r, cy + r, 0, on ? 0.9f : 0.15f, on ? 0.9f : 0.15f, on ? 0.95f : 0.18f,
                     al * 0.9f);
                text(cx, cy, r * 1.1f, c->label, on ? 0.1f : 1, on ? 0.1f : 1, on ? 0.1f : 1, al * 1.4f, 1);
            }
        }
    }
    if (g_msg[0]) { /* centered, multi-line */
        char tmp[1024];
        strncpy(tmp, g_msg, sizeof tmp - 1);
        tmp[sizeof tmp - 1] = 0;
        int nl = 1;
        for (char* q = tmp; *q; q++)
            if (*q == '\n') nl++;
        float sz = u * 4.2f, y = h * 0.5f - (nl - 1) * sz * 0.6f;
        quad(0, y - sz * 1.2f, (float)w, y + (nl - 1) * sz * 1.2f + sz * 1.2f, 3, 0, 0, 0, 0.75f);
        char* line = strtok(tmp, "\n");
        while (line) {
            text(w * 0.5f, y, sz, line, 1, 1, 1, 1, 1);
            y += sz * 1.2f;
            line = strtok(NULL, "\n");
        }
    }
    glDisable(GL_BLEND);
}
