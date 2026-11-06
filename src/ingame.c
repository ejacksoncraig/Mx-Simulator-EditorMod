/* The app's tools inside the game's editor (docs/ingame-tools.md). M5: two inline hooks and a test draw.

   Hook A wraps the editor's 2D draw (0x1400613b0): the original runs, then we draw on top with the game's own helpers,
   which record into the same command list, so it shows in the same frame. Hook B wraps the key and mouse button
   handler (0x1400645d0): clicks on our panel stop here, everything else goes to the game.

   Both targets start with the same 19 bytes (push r15..rbx, sub rsp imm32), with nothing RIP-relative, so the hook is
   a 14-byte absolute jump and the trampoline runs those 19 bytes and jumps back. The bytes are checked before patching
   and put back when the DLL unloads. Offline only, --editor only (docs/design.md, rule 2). */
#include "mod.h"
#include "curvetool.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── what the game has (all RVAs, verified by disassembly 2026-11-05) ── */
#define RVA_EDITOR_DRAW (g_build->rva_editor_draw)      /* void(void): the editor's whole 2D pass */
#define RVA_KEY_HANDLER (g_build->rva_key_handler)      /* int(int key, int down): 1 = handled */
#define FRAME_EDITOR_DRAW (g_build->frame_editor_draw)      /* its sub rsp */
#define FRAME_KEY_HANDLER (g_build->frame_key_handler)

#define RVA_PTR_WINDOW (g_build->rva_ptr_window)      /* -> window: int W @+0x58, H @+0x5c, mouse x @+0x60, y @+0x64 (px, y down),
                                         float ortho half extents @+0x68, +0x6c */
#define RVA_PTR_KEYS (g_build->rva_ptr_keys)        /* -> key codes: left mouse button @+0x20, pan button @+0x24 */
#define RVA_PTR_COLOUR (g_build->rva_ptr_colour)      /* -> float rgba used by every vertex */
#define RVA_PTR_BATCH_FLAG (g_build->rva_ptr_batch_flag)  /* -> int: 1 while vertices go in, 0 before the batch ends */
#define RVA_PTR_WHITE_TEX (g_build->rva_ptr_white_tex)   /* -> int texture id the panel backdrop binds */
#define RVA_EDITOR_TERRAIN (g_build->rva_editor_terrain)
#define RVA_VIEW_X (g_build->rva_view_x)         /* float, view centre in feet */
#define RVA_VIEW_Z (g_build->rva_view_z)
#define RVA_ZOOM (g_build->rva_zoom)            /* double: the Zoom slider; the window shows sizeFt / 2^zoom feet tall */
#define T_SIZE_FT 0x10c8u

#define RVA_MATRIX_MODE (g_build->rva_matrix_mode)      /* void(int): 0x1700 modelview */
#define RVA_IDENTITY (g_build->rva_identity)
#define RVA_TRANSLATE (g_build->rva_translate)        /* void(float x, y, z) */
#define RVA_SCALE (g_build->rva_scale)
#define RVA_PUSH (g_build->rva_push)
#define RVA_POP (g_build->rva_pop)
#define RVA_ENABLE (g_build->rva_enable)           /* void(int cap) */
#define RVA_DISABLE (g_build->rva_disable)
#define RVA_BIND_TEXTURE (g_build->rva_bind_texture)     /* void(int target, int id) */
#define RVA_BLEND_FUNC (g_build->rva_blend_func)       /* void(int src, int dst) */
#define RVA_BEGIN (g_build->rva_begin)             /* void(int primitive): 1 lines, 7 quads */
#define RVA_VERTEX (g_build->rva_vertex)           /* void(float x, y, z) */
#define RVA_END (g_build->rva_end)
#define RVA_NEXT_ROW (g_build->rva_next_row)         /* void(float *pos, float *scale, float dx, float dy): translate, pos += (dx, dy) */
#define RVA_BUTTON (g_build->rva_button)           /* void(button *, int on, float *pos, float *scale, float width, float align) */

#define GL_BLEND 0x0be2
#define GL_TEXTURE_2D 0x0de1
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_MODELVIEW 0x1700

typedef void (*v_fn)(void);
typedef void (*i_fn)(int);
typedef void (*ii_fn)(int, int);
typedef void (*fff_fn)(float, float, float);
typedef void (*row_fn)(float *, float *, float, float);
typedef struct {
    const char *label;
    int rect[4]; /* x0, y0, x1, y1 in window px, y down; written by the game's draw */
    int pad[2];
} game_button;
typedef void (*button_fn)(game_button *, int, float *, float *, float, float);
typedef uintptr_t (*draw_fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef int (*key_fn)(int, int);

/* ── inline hooks ── */
typedef struct {
    const char *name;
    unsigned char *target;
    unsigned char saved[19];
    unsigned char *trampoline;
    int installed;
} hook;

static const unsigned char PROLOGUE[15] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                           0x55, 0x57, 0x56, 0x53, 0x48, 0x81, 0xec};
enum { PATCH_LEN = 19 };

static void abs_jump(unsigned char *at, const void *to) /* jmp qword ptr [rip+0]; dq to (14 bytes) */
{
    at[0] = 0xff, at[1] = 0x25, at[2] = at[3] = at[4] = at[5] = 0;
    uint64_t addr = (uint64_t)(uintptr_t)to;
    memcpy(at + 6, &addr, 8);
}

static int write_code(unsigned char *at, const unsigned char *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

/* Only patches when the 19 bytes are exactly what the game was checked with. */
static int hook_install(hook *h, uintptr_t base, uint32_t rva, uint32_t frame, void *detour)
{
    h->target = (unsigned char *)(base + rva);
    if (!mx_readable(h->target, PATCH_LEN)) {
        mx_log("hook %s: %p not readable", h->name, (void *)h->target);
        return 0;
    }
    uint32_t imm;
    memcpy(&imm, h->target + 15, 4);
    if (memcmp(h->target, PROLOGUE, sizeof PROLOGUE) != 0 || imm != frame) {
        char hex[PATCH_LEN * 3 + 1];
        for (int i = 0; i < PATCH_LEN; i++)
            sprintf(hex + 3 * i, "%02x ", h->target[i]);
        mx_log("hook %s: prologue isn't the one checked (%s), not patching", h->name, hex);
        return 0;
    }
    h->trampoline = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!h->trampoline) {
        mx_log("hook %s: no memory for the trampoline", h->name);
        return 0;
    }
    memcpy(h->trampoline, h->target, PATCH_LEN);
    abs_jump(h->trampoline + PATCH_LEN, h->target + PATCH_LEN);
    memcpy(h->saved, h->target, PATCH_LEN);
    unsigned char patch[PATCH_LEN];
    abs_jump(patch, detour);
    memset(patch + 14, 0x90, PATCH_LEN - 14);
    if (!write_code(h->target, patch, PATCH_LEN)) {
        mx_log("hook %s: VirtualProtect failed (%lu)", h->name, GetLastError());
        return 0;
    }
    h->installed = 1;
    mx_log("hook %s installed at %p", h->name, (void *)h->target);
    return 1;
}

static void hook_remove(hook *h)
{
    if (!h->installed)
        return;
    if (write_code(h->target, h->saved, PATCH_LEN))
        h->installed = 0;
    /* The trampoline stays allocated: a thread could still be returning through it. */
}

/* ── state ── */
static uintptr_t g_base;
static hook g_draw = {"editor draw"}, g_key = {"key handler"};
static char g_status[96] = "off";
static volatile LONG g_presses;
/* Diagnostics for hello: how often each detour ran, why the overlay last skipped, what it last computed. */
static volatile LONG g_draw_calls, g_overlay_runs, g_key_calls;
static const char *g_skip = "";
static float g_last[8]; /* w, h, ortho x, ortho y, units wide, units per ft, map x0 in units, view ft */
/* The game's other widgets (verified by disassembly, docs/ingame-tools.md section 2). */
#define RVA_CHOICE (g_build->rva_choice)        /* void(choice *, int on, float *pos, float *scale, float width, float align, float split) */
#define RVA_SLIDER (g_build->rva_slider)        /* void(slider *, int on, float *pos, float *scale, float width, float align, float split) */
#define RVA_READOUT (g_build->rva_readout)       /* void(const char *text, const float *colour a, const float *colour b, float width, int) */
#define RVA_PTR_READOUT_A (g_build->rva_ptr_readout_a) /* -> the readout's colours, as the panel passes them */
#define RVA_PTR_READOUT_B (g_build->rva_ptr_readout_b)
typedef struct {
    const char *label;
    const char *const *options;
    int count, selected;
    int rect[4];
} game_choice;
typedef struct {
    const char *label;
    double value, step, min, max;
    int rect[4];
    int bar[2]; /* the bar's x0, x1 in px, written by the draw */
} game_slider;
typedef char game_choice_is_40_bytes[sizeof(game_choice) == 40 ? 1 : -1];
typedef char game_slider_is_64_bytes[sizeof(game_slider) == 64 ? 1 : -1];
typedef void (*choice_fn)(game_choice *, int, float *, float *, float, float, float);
typedef void (*slider_fn)(game_slider *, int, float *, float *, float, float, float);
typedef void (*readout_fn)(const char *, const float *text, const float *box, float width, float align); /* align 0 left, 1 right */

/* Our panel's rows (the curve tool's), drawn with the game's widgets; their rects come back from the game's draw. */
enum { MAX_ROWS = 40 };
static panel_row g_panel[MAX_ROWS];
static game_button g_btn[MAX_ROWS];
static game_choice g_ch[MAX_ROWS];
static game_slider g_sl[MAX_ROWS];
static int g_row_count;
static int g_slide_row = -1; /* the slider being dragged */
static scene g_own; /* the curve tool's draw items, rebuilt each frame */

#define GAME(rva, type) ((type)(g_base + (rva)))

static uint8_t *window_ptr(void) { return *(uint8_t **)(g_base + RVA_PTR_WINDOW); }
static int win_w(void) { return *(int *)(window_ptr() + 0x58); }
static int win_h(void) { return *(int *)(window_ptr() + 0x5c); }
static int mouse_x(void) { return *(int *)(window_ptr() + 0x60); }
static int mouse_y(void) { return *(int *)(window_ptr() + 0x64); }
static float ortho_x(void) { return *(float *)(window_ptr() + 0x68); }
static float ortho_y(void) { return *(float *)(window_ptr() + 0x6c); }

static int pointers_ok(void)
{
    uintptr_t w = *(uintptr_t *)(g_base + RVA_PTR_WINDOW), k = *(uintptr_t *)(g_base + RVA_PTR_KEYS);
    return w && k && mx_readable((void *)w, 0x70) && mx_readable((void *)k, 0x28) &&
           mx_readable(*(void **)(g_base + RVA_PTR_COLOUR), 16) && mx_readable(*(void **)(g_base + RVA_PTR_BATCH_FLAG), 4) &&
           mx_readable(*(void **)(g_base + RVA_PTR_WHITE_TEX), 4) && win_h() > 0;
}

/* Our panel runs down the right edge, mirroring the game's left one: 21 units wide plus a 0.25 fade, 1 unit = H/64 px. */
static float units_wide(void) { return 64.0f * ortho_x() / ortho_y(); }
/* Our panel is always there in the editor, only as tall as its rows: 1 unit of margin plus 1.5 a row. */
static float panel_bottom(void) { return -(1.0f + 1.5f * (float)g_row_count); }
static int over_our_panel(void)
{
    float unit_px = win_h() / 64.0f;
    return g_row_count > 0 && win_h() > 0 && mouse_x() >= (units_wide() - 21.25f) * unit_px &&
           mouse_y() < -panel_bottom() * unit_px;
}
static const int *row_rect(int i)
{
    static const int none[4] = {-1, -1, -1, -1};
    int kind = g_panel[i].kind;
    return kind == ROWK_BUTTON ? g_btn[i].rect : kind == ROWK_CHOICE ? g_ch[i].rect : kind == ROWK_SLIDER ? g_sl[i].rect : none;
}
static int over_row(int i)
{
    const int *r = row_rect(i);
    return r[0] >= 0 && r[2] > r[0] && mouse_x() >= r[0] && mouse_x() < r[2] && mouse_y() >= r[1] && mouse_y() < r[3];
}

/* A slider follows the mouse like the game's: the place along its bar, in steps, between min and max. */
static void slide_to_mouse(int i)
{
    const game_slider *s = &g_sl[i];
    if (s->bar[1] <= s->bar[0])
        return;
    double t = (double)(mouse_x() - s->bar[0]) / (s->bar[1] - s->bar[0]);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double value = s->min + floor((s->max - s->min) * t / s->step + 0.5) * s->step;
    curvetool_slide(i, value > s->max ? s->max : value);
}

static void set_colour(float r, float g, float b, float a)
{
    float *c = *(float **)(g_base + RVA_PTR_COLOUR);
    c[0] = r, c[1] = g, c[2] = b, c[3] = a;
}

/* x0 < x1, y0 < y1. Corners in the game's order (left top, left bottom, right bottom, right top: counter-clockwise),
   since the other way round faces away and isn't drawn. */
static void quad(float x0, float y0, float x1, float y1)
{
    fff_fn vertex = GAME(RVA_VERTEX, fff_fn);
    vertex(x0, y1, 0), vertex(x0, y0, 0), vertex(x1, y0, 0), vertex(x1, y1, 0);
}

/* World feet to panel units, the way the game's map camera places them (ortho, rotate 90 about x, scale
   ec * 2 * 2^zoom / sizeFt, translate -centre): ux = (x - cx) * per_ft + x0, uy = -(z - cz) * per_ft - 32, where
   per_ft = 64 / viewFt and x0 = 32 * e8 / ec is the window's middle. Exact, unlike the mouse formula's integer W/2. */
typedef struct {
    float cx, cz, per_ft, x0;
} map_view;

static void vertex_at(const map_view *v, float x, float z)
{
    GAME(RVA_VERTEX, fff_fn)((x - v->cx) * v->per_ft + v->x0, -(z - v->cz) * v->per_ft - 32.0f, 0);
}

static void draw_scene(const scene *s, const map_view *v, float u_ft)
{
    int *batch = *(int **)(g_base + RVA_PTR_BATCH_FLAG);
    for (int i = 0; i < s->items; i++) {
        const scene_item *it = &s->item[i];
        set_colour(it->rgba[0], it->rgba[1], it->rgba[2], it->rgba[3]);
        if (it->kind == SCENE_LINE) {
            if (it->count < 2)
                continue;
            const float(*p)[2] = &s->pt[it->first];
            GAME(RVA_BEGIN, i_fn)(1);
            *batch = 1;
            for (int k = 0; k + 1 < it->count; k++)
                vertex_at(v, p[k][0], p[k][1]), vertex_at(v, p[k + 1][0], p[k + 1][1]);
            if (it->closed && it->count > 2)
                vertex_at(v, p[it->count - 1][0], p[it->count - 1][1]), vertex_at(v, p[0][0], p[0][1]);
            *batch = 0;
            GAME(RVA_END, v_fn)();
        } else if (it->kind == SCENE_SEGMENTS) {
            const float(*p)[2] = &s->pt[it->first];
            GAME(RVA_BEGIN, i_fn)(1);
            *batch = 1;
            for (int k = 0; k + 1 < it->count; k += 2)
                vertex_at(v, p[k][0], p[k][1]), vertex_at(v, p[k + 1][0], p[k + 1][1]);
            *batch = 0;
            GAME(RVA_END, v_fn)();
        } else if (it->kind == SCENE_QUAD) {
            if (it->count != 4)
                continue;
            const float(*p)[2] = &s->pt[it->first];
            GAME(RVA_BEGIN, i_fn)(7);
            *batch = 1;
            for (int k = 0; k < 4; k++)
                vertex_at(v, p[k][0], p[k][1]);
            *batch = 0;
            GAME(RVA_END, v_fn)();
        } else if (it->kind == SCENE_RING) {
            /* The game's ring: an annulus from radius - width/2 to radius + width/2, as 32 quads, counter-clockwise. */
            float ux = (it->x - v->cx) * v->per_ft + v->x0, uy = -(it->z - v->cz) * v->per_ft - 32.0f;
            float r_in = (it->size - it->width * 0.5f) * u_ft * v->per_ft, r_out = (it->size + it->width * 0.5f) * u_ft * v->per_ft;
            fff_fn vertex = GAME(RVA_VERTEX, fff_fn);
            GAME(RVA_BEGIN, i_fn)(7);
            *batch = 1;
            for (int k = 0; k < 32; k++) {
                float a0 = k * (6.2831853f / 32), a1 = (k + 1) * (6.2831853f / 32);
                float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
                vertex(ux + r_in * c0, uy + r_in * s0, 0), vertex(ux + r_out * c0, uy + r_out * s0, 0);
                vertex(ux + r_out * c1, uy + r_out * s1, 0), vertex(ux + r_in * c1, uy + r_in * s1, 0);
            }
            *batch = 0;
            GAME(RVA_END, v_fn)();
        } else {
            /* Dot: a square size u across. Cross: two bars size u long. Both centred on (x, z). */
            float half = it->size * u_ft * 0.5f * v->per_ft;
            float ux = (it->x - v->cx) * v->per_ft + v->x0, uy = -(it->z - v->cz) * v->per_ft - 32.0f;
            float thick = half * 0.15f > 0.05f ? half * 0.15f : 0.05f;
            GAME(RVA_BEGIN, i_fn)(7);
            *batch = 1;
            if (it->kind == SCENE_DOT) {
                quad(ux - half, uy - half, ux + half, uy + half);
            } else {
                quad(ux - half, uy - thick, ux + half, uy + thick);
                quad(ux - thick, uy - half, ux + thick, uy + half);
            }
            *batch = 0;
            GAME(RVA_END, v_fn)();
        }
    }
}

static void brush_outline(scene *s, const curve_view *v, double mx, double mz);

/* Draw on top of the editor, in the same screen space as the game's panel (translate to the top-left, 1 unit = 1/64 of
   the height, y up): the app's scene, then our panel if one of our tools is active. */
static void overlay_draw(void)
{
    uintptr_t terrain = 0;
    if (!mx_readable((void *)(g_base + RVA_EDITOR_TERRAIN), 8) || !(terrain = *(uintptr_t *)(g_base + RVA_EDITOR_TERRAIN))) {
        g_skip = "no terrain";
        return;
    }
    if (!pointers_ok()) {
        g_skip = "pointers not readable";
        return;
    }
    const scene *sc = scene_current();
    g_row_count = curvetool_panel(g_panel, MAX_ROWS);
    g_skip = "";
    InterlockedIncrement(&g_overlay_runs);
    float size_ft = *(float *)(terrain + T_SIZE_FT);
    double zoom = *(double *)(g_base + RVA_ZOOM);
    int w = win_w(), h = win_h();
    float view_ft = (float)(size_ft / pow(2.0, zoom));
    curve_view cv = {*(float *)(g_base + RVA_VIEW_X), *(float *)(g_base + RVA_VIEW_Z), view_ft, w, h};
    curvetool_scene(&g_own, &cv, mouse_x(), mouse_y());
    double mouse_wx = (mouse_x() - w / 2) / (double)h * view_ft + cv.cx, mouse_wz = (mouse_y() - h / 2) / (double)h * view_ft + cv.cz;
    brush_outline(&g_own, &cv, mouse_wx, mouse_wz);
    if (curvetool_mode() == CURVE_MODE_OFF)
        gradtool_scene(&g_own, mouse_wx, mouse_wz);
    float unit_px = h / 64.0f, wide = units_wide();
    int *batch = *(int **)(g_base + RVA_PTR_BATCH_FLAG);
    map_view v = {*(float *)(g_base + RVA_VIEW_X), *(float *)(g_base + RVA_VIEW_Z), 64.0f / view_ft, wide * 0.5f};
    float u_ft = view_ft * 0.01f > 1.0f ? view_ft * 0.01f : 1.0f; /* the game's marker unit */
    g_last[0] = (float)w, g_last[1] = (float)h, g_last[2] = ortho_x(), g_last[3] = ortho_y(), g_last[4] = wide;
    g_last[5] = v.per_ft, g_last[6] = v.x0, g_last[7] = view_ft;

    GAME(RVA_MATRIX_MODE, i_fn)(GL_MODELVIEW);
    GAME(RVA_PUSH, v_fn)();
    GAME(RVA_IDENTITY, v_fn)();
    GAME(RVA_TRANSLATE, fff_fn)(-ortho_x(), ortho_y(), -1.0f);
    float s = ortho_y() / 32.0f;
    GAME(RVA_SCALE, fff_fn)(s, s, s);
    GAME(RVA_ENABLE, i_fn)(GL_BLEND);
    GAME(RVA_BIND_TEXTURE, ii_fn)(GL_TEXTURE_2D, **(int **)(g_base + RVA_PTR_WHITE_TEX));
    GAME(RVA_BLEND_FUNC, ii_fn)(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_scene(&g_own, &v, u_ft);
    draw_scene(sc, &v, u_ft);
    if (!g_row_count) {
        GAME(RVA_DISABLE, i_fn)(GL_BLEND);
        GAME(RVA_POP, v_fn)();
        return;
    }

    GAME(RVA_BEGIN, i_fn)(7);
    *batch = 1;
    /* Our panel's backdrop and fade, the game's colours mirrored, as tall as the rows. */
    float bottom = panel_bottom();
    set_colour(0, 0, 0, 0.5f);
    quad(wide - 21.0f, bottom, wide, 0.0f);
    /* The fade runs left from the backdrop, so start at its clear left edge to keep the game's corner order. */
    fff_fn vertex = GAME(RVA_VERTEX, fff_fn);
    set_colour(0, 0, 0, 0);
    vertex(wide - 21.25f, 0, 0), vertex(wide - 21.25f, bottom, 0);
    set_colour(0, 0, 0, 0.5f);
    vertex(wide - 21.0f, bottom, 0), vertex(wide - 21.0f, 0, 0);
    *batch = 0;
    GAME(RVA_END, v_fn)();
    GAME(RVA_DISABLE, i_fn)(GL_BLEND);

    /* The rows, laid out like the game's: in 0.5 from the edge, down 1.5 per row, 20 wide. Lit when on or hovered. */
    float pos[2] = {0, 0}, scale[2] = {unit_px, -unit_px};
    row_fn next_row = GAME(RVA_NEXT_ROW, row_fn);
    next_row(pos, scale, wide - 21.0f + 0.5f, 0);
    for (int i = 0; i < g_row_count; i++) {
        const panel_row *row = &g_panel[i];
        int hot = over_row(i) || g_slide_row == i;
        next_row(pos, scale, 0, -1.5f);
        if (row->kind == ROWK_BUTTON) {
            g_btn[i].label = row->label;
            GAME(RVA_BUTTON, button_fn)(&g_btn[i], row->on || hot, pos, scale, 20.0f, 0.0f);
        } else if (row->kind == ROWK_CHOICE) {
            g_ch[i].label = row->label, g_ch[i].options = row->options, g_ch[i].count = row->count;
            g_ch[i].selected = row->selected;
            GAME(RVA_CHOICE, choice_fn)(&g_ch[i], hot, pos, scale, 20.0f, 0.0f, 0.5f);
        } else if (row->kind == ROWK_SLIDER) {
            g_sl[i].label = row->label, g_sl[i].value = row->value, g_sl[i].step = row->step;
            g_sl[i].min = row->min, g_sl[i].max = row->max;
            GAME(RVA_SLIDER, slider_fn)(&g_sl[i], hot, pos, scale, 20.0f, 0.0f, 0.5f);
        } else if (row->kind == ROWK_HEADER || row->kind == ROWK_HINT || row->kind == ROWK_FAINT) {
            /* Text with no box: a header in the gold the tools draw with, a hint in soft grey, small print (the
               version) faint and against the right edge. */
            static const float head[4] = {1.0f, 0.824f, 0.290f, 1.0f}, hint[4] = {0.82f, 0.82f, 0.82f, 0.9f};
            static const float faint[4] = {0.85f, 0.85f, 0.85f, 0.4f}, clear[4] = {0, 0, 0, 0};
            const float *colour = row->kind == ROWK_HEADER ? head : row->kind == ROWK_HINT ? hint : faint;
            GAME(RVA_READOUT, readout_fn)(row->label && row->label[0] ? row->label : row->text, colour, clear, 20.0f,
                                          row->kind == ROWK_FAINT ? 1.0f : 0.0f);
        } else {
            GAME(RVA_READOUT, readout_fn)(row->text, *(float **)(g_base + RVA_PTR_READOUT_A),
                                          *(float **)(g_base + RVA_PTR_READOUT_B), 20.0f, 0.0f);
        }
    }
    GAME(RVA_POP, v_fn)();
}

/* ── events for the app (M7): game thread only, so no locking ── */
#define RVA_HOVERED_WIDGET (g_build->rva_hovered_widget) /* int, the game's widget under the mouse, -1 = none (on the map) */
enum { EV_DOWN = 1, EV_UP, EV_MOVE, EV_KEY, EV_TOOL_OFF };
typedef struct {
    int t, key, down, px, py, ctrl, shift, alt;
    float x, z;
} game_event;
enum { MAX_EVENTS = 256 };
static game_event g_events[MAX_EVENTS];
static int g_event_count;
static int g_last_px = -1, g_last_py = -1;
static int g_map_press; /* the left button went down on the map for our tool, so its release is ours */
static volatile LONG g_events_dropped;

/* The world point under a window px, as the game computes it (0x1400613b0, 0x1400645d0): W/2, H/2 integer. */
static float view_ft_now(void)
{
    uintptr_t terrain = *(uintptr_t *)(g_base + RVA_EDITOR_TERRAIN);
    return terrain ? (float)(*(float *)(terrain + T_SIZE_FT) / pow(2.0, *(double *)(g_base + RVA_ZOOM))) : 0;
}

static int current_view(curve_view *v)
{
    float view = view_ft_now();
    if (!(view > 0))
        return 0;
    v->cx = *(float *)(g_base + RVA_VIEW_X), v->cz = *(float *)(g_base + RVA_VIEW_Z), v->view_ft = view;
    v->w = win_w(), v->h = win_h();
    return v->h > 0;
}

static void world_at(int px, int py, float *x, float *z)
{
    int w = win_w(), h = win_h();
    float view = view_ft_now();
    *x = (float)(px - w / 2) / h * view + *(float *)(g_base + RVA_VIEW_X);
    *z = (float)(py - h / 2) / h * view + *(float *)(g_base + RVA_VIEW_Z);
}

static void queue(int t, int key, int down)
{
    if (g_event_count == MAX_EVENTS) {
        /* Full: the oldest move goes first, since a newer one says where the mouse is. Else the oldest event. */
        int drop = 0;
        for (int i = 0; i < g_event_count; i++)
            if (g_events[i].t == EV_MOVE) {
                drop = i;
                break;
            }
        memmove(&g_events[drop], &g_events[drop + 1], (size_t)(g_event_count - drop - 1) * sizeof g_events[0]);
        g_event_count--;
        InterlockedIncrement(&g_events_dropped);
    }
    game_event *e = &g_events[g_event_count++];
    e->t = t, e->key = key, e->down = down;
    e->px = mouse_x(), e->py = mouse_y();
    world_at(e->px, e->py, &e->x, &e->z);
    e->ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    e->shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    e->alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

/* Our tool has the map: the in-game curve tool, or the app's (M8a). */
static int tool_on(void) { return curvetool_mode() != CURVE_MODE_OFF || scene_current()->tool[0] != 0; }

/* The game takes over again: our tool goes off at once (no waiting for the app), and the app hears why. */
static void tool_off(int key)
{
    curvetool_set_mode(CURVE_MODE_OFF);
    scene_drop_tool();
    g_map_press = 0;
    queue(EV_TOOL_OFF, key, 1);
}

/* Keys that always go to the game, even with our tool on: pan, zoom, arrows, save, undo, Tab. */
static int always_the_games(int key, const int *codes)
{
    return key == codes[9] /* pan button, keys + 0x24 */ || key == '=' || key == '-' || key == 's' || key == 8 || key == 9 ||
           (key >= 0x100c && key <= 0x100e) /* mouse wheel and middle button (seen in M7) */ ||
           key == codes[0] || key == codes[1] || key == codes[2] || key == codes[3]; /* arrows, keys + 0x0 .. 0xc */
}

static void gradient_length_now(void);
static void brush_frame(void);

/* Each frame (SwapBuffers): one move when the mouse moved and our tool is on, or a button is held on our panel. */
void ingame_frame(void)
{
    if (!g_base || !g_draw.installed || !pointers_ok())
        return;
    curvetool_frame();
    gradtool_frame();
    gradient_length_now();
    brush_frame();
    int px = mouse_x(), py = mouse_y();
    if ((px != g_last_px || py != g_last_py) && g_slide_row >= 0 && g_slide_row < g_row_count) {
        slide_to_mouse(g_slide_row);
    } else if ((px != g_last_px || py != g_last_py) && curvetool_dragging()) {
        curve_view v;
        if (current_view(&v))
            curvetool_mouse(&v, CURVE_MOVE, px, py, 0, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    } else if ((px != g_last_px || py != g_last_py) && (tool_on() || g_map_press) && curvetool_mode() == CURVE_MODE_OFF) {
        queue(EV_MOVE, 0, 0);
    }
    g_last_px = px, g_last_py = py;
}

static const char *const EVENT_NAMES[] = {"", "down", "up", "move", "key", "toolOff"};

/* Hand the queued events to the app as JSON, with the view they go with, and empty the queue. */
int ingame_events_json(char *out, size_t cap)
{
    size_t n = 0;
    float view = g_base && pointers_ok() ? view_ft_now() : 0;
    if (g_base && pointers_ok())
        n += (size_t)snprintf(out, cap,
            "{\"view\": {\"cx\": %.9g, \"cz\": %.9g, \"viewFt\": %.9g, \"w\": %d, \"h\": %d, \"mode\": %d, \"u\": %.9g, "
            "\"tool\": \"%s\"}, \"dropped\": %ld, \"events\": [",
            *(float *)(g_base + RVA_VIEW_X), *(float *)(g_base + RVA_VIEW_Z), view, win_w(), win_h(),
            *(int *)(g_base + g_build->rva_editor_mode), view * 0.01f > 1 ? view * 0.01f : 1.0f, scene_current()->tool, g_events_dropped);
    else
        n += (size_t)snprintf(out, cap, "{\"view\": null, \"dropped\": %ld, \"events\": [", g_events_dropped);
    int i = 0;
    for (; i < g_event_count && n + 200 < cap; i++) {
        const game_event *e = &g_events[i];
        n += (size_t)snprintf(out + n, cap - n,
            "%s{\"t\": \"%s\", \"x\": %.6g, \"z\": %.6g, \"px\": %d, \"py\": %d, \"key\": %d, \"down\": %s, "
            "\"ctrl\": %s, \"shift\": %s, \"alt\": %s}",
            i ? ", " : "", EVENT_NAMES[e->t], e->x, e->z, e->px, e->py, e->key, e->down ? "true" : "false",
            e->ctrl ? "true" : "false", e->shift ? "true" : "false", e->alt ? "true" : "false");
    }
    /* Anything that didn't fit waits for the next call. */
    memmove(g_events, g_events + i, (size_t)(g_event_count - i) * sizeof g_events[0]);
    g_event_count -= i;
    n += (size_t)snprintf(out + n, cap - n, "]}");
    return (int)n;
}

/* ── brush shapes: a patch in the game's brush loop (docs/ingame-tools.md, "Brush shapes") ──
   At 0x140060035 the loop has the offset from a cell to the stroke in xmm2 (x) and xmm0 (z) and turns it into a
   distance (square, add, sqrt), which the inner/outer smoothstep and every mode but Smooth then use. While a shape
   other than Circle is chosen, those 37 bytes jump to brush_stub.asm, which asks mx_brush_distance. */
#define RVA_BRUSH_DISTANCE (g_build->rva_brush_distance)
#define RVA_BRUSH_RETURN (g_build->rva_brush_return)
enum { BRUSH_PATCH_LEN = 37 };
static const unsigned char BRUSH_ORIGINAL[BRUSH_PATCH_LEN] = {
    0xf3, 0x0f, 0x59, 0xc0, 0x41, 0x39, 0xdf, 0x89, 0xde, 0xf3, 0x0f, 0x59, 0xd2, 0x41, 0x0f, 0x4e, 0xf7, 0x03, 0x74,
    0x24, 0x48, 0xf3, 0x0f, 0x58, 0xc2, 0xf3, 0x0f, 0x5a, 0xc0, 0xf2, 0x0f, 0x51, 0xc0, 0xf2, 0x0f, 0x5a, 0xc0};
void mx_brush_stub(void);
uint64_t mx_brush_return;
static int g_brush_patched, g_brush_refused;
static volatile float g_brush_outer, g_brush_inner; /* the last stroke's, for the outline */
static int g_brush_shape;
static double g_brush_cos = 1, g_brush_sin = 0, g_brush_square_scale = 1, g_brush_oval = 0.5;

/* The distance the brush measures, in feet: the game's own for a circle (same float steps), else the shape's. The
   outer edge of the shape is where this reaches the brush's outer radius. */
float mx_brush_distance(float dz, float dx, float outer, float inner)
{
    g_brush_outer = outer, g_brush_inner = inner;
    if (g_brush_shape == BRUSH_CIRCLE) {
        float s = dz * dz + dx * dx;
        return (float)sqrt((double)s);
    }
    double lx = dx * g_brush_cos + dz * g_brush_sin, lz = -dx * g_brush_sin + dz * g_brush_cos;
    if (g_brush_shape == BRUSH_SQUARE)
        return (float)((fabs(lx) > fabs(lz) ? fabs(lx) : fabs(lz)) * g_brush_square_scale);
    return (float)sqrt(lx * lx + (lz / g_brush_oval) * (lz / g_brush_oval));
}

static void brush_patch(int on)
{
    unsigned char *at = (unsigned char *)(g_base + RVA_BRUSH_DISTANCE);
    if (on == g_brush_patched || g_brush_refused || !g_draw.installed)
        return;
    if (on) {
        if (memcmp(at, BRUSH_ORIGINAL, BRUSH_PATCH_LEN) != 0) {
            g_brush_refused = 1;
            mx_log("brush shapes: the brush loop isn't the one checked, not patching");
            return;
        }
        unsigned char patch[BRUSH_PATCH_LEN];
        abs_jump(patch, (void *)mx_brush_stub);
        memset(patch + 14, 0x90, BRUSH_PATCH_LEN - 14);
        mx_brush_return = g_base + RVA_BRUSH_RETURN;
        if (write_code(at, patch, BRUSH_PATCH_LEN))
            g_brush_patched = 1, mx_log("brush shapes on");
    } else if (write_code(at, BRUSH_ORIGINAL, BRUSH_PATCH_LEN)) {
        g_brush_patched = 0;
        mx_log("brush shapes off: the game's round brush");
    }
}

/* ── the radial sweep: the game's radial helper swapped for one with a wider or narrower sweep ──
   The helper (0x14005fa10, float(float x, float z)) takes v = cell - A; if v points behind A (dot with B - A below
   0) it drops v's part along A to B; then s = |v| and it returns the selected gradient's profile height at s
   (tail call to 0x14005f820). So the game sweeps 180 degrees centred on B and runs straight past the edges. Ours does
   the same for any sweep: inside the wedge s = |v|, outside it s = v's length along the nearer edge (0 if behind it).
   At 180 centred that is exactly the game's rule. While the sweep is the game's own, the game's bytes are back. */
#define RVA_RADIAL_HEIGHT (g_build->rva_radial_height)
#define RVA_PROFILE_HEIGHT (g_build->rva_profile_height)
enum { RADIAL_PATCH_LEN = 14 }; /* mov eax, [rip+sel]; test eax, eax; js: three whole instructions */
typedef float (*profile_fn)(float);
static unsigned char g_radial_saved[RADIAL_PATCH_LEN];
static int g_radial_patched, g_radial_refused;
static struct {
    float ax, az, bx, bz;
    double deg;
    int side;
    double mid, half;
} g_wedge;

static float radial_height(float x, float z)
{
    int sel = *(volatile int *)(g_base + g_build->rva_selected_gradient);
    int count = *(volatile int *)(g_base + g_build->rva_gradient_count);
    const uint8_t *arr = *(const uint8_t **)(g_base + g_build->rva_gradients);
    if (sel < 0 || sel >= count || !arr)
        return 0;
    const float *g = (const float *)(arr + (size_t)sel * (size_t)*(const int *)(g_base + g_build->rva_gradient_stride));
    if (*(const int *)((const uint8_t *)g + 0x1c) == 0) /* no profile points */
        return 0;
    /* The wedge only changes when the gradient or the settings do, so it's kept between cells. */
    double deg;
    int side;
    gradtool_sweep(&deg, &side);
    if (g[0] != g_wedge.ax || g[1] != g_wedge.az || g[2] != g_wedge.bx || g[3] != g_wedge.bz || deg != g_wedge.deg ||
        side != g_wedge.side) {
        g_wedge.ax = g[0], g_wedge.az = g[1], g_wedge.bx = g[2], g_wedge.bz = g[3], g_wedge.deg = deg, g_wedge.side = side;
        gradtool_wedge(g[0], g[1], g[2], g[3], &g_wedge.mid, &g_wedge.half);
    }
    double s = sweep_s((double)x - g[0], (double)z - g[1], g_wedge.mid, g_wedge.half);
    return ((profile_fn)(void *)(g_base + RVA_PROFILE_HEIGHT))((float)s);
}

static void radial_patch(int on)
{
    unsigned char *at = (unsigned char *)(g_base + RVA_RADIAL_HEIGHT);
    if (on == g_radial_patched || g_radial_refused || !g_draw.installed || !RVA_RADIAL_HEIGHT || !RVA_PROFILE_HEIGHT)
        return;
    if (on) {
        /* The helper must start as checked: its first read is the selected gradient. */
        int32_t disp;
        memcpy(&disp, at + 2, 4);
        if (!mx_readable(at, RADIAL_PATCH_LEN) || at[0] != 0x8b || at[1] != 0x05 || at[6] != 0x85 || at[7] != 0xc0 ||
            at[8] != 0x0f || at[9] != 0x88 || (uintptr_t)(at + 6 + disp) != g_base + g_build->rva_selected_gradient) {
            g_radial_refused = 1;
            mx_log("radial sweep: the radial helper isn't the one checked, not patching");
            return;
        }
        memcpy(g_radial_saved, at, RADIAL_PATCH_LEN);
        unsigned char patch[RADIAL_PATCH_LEN];
        abs_jump(patch, (void *)radial_height);
        if (write_code(at, patch, RADIAL_PATCH_LEN))
            g_radial_patched = 1, mx_log("radial sweep on");
    } else if (write_code(at, g_radial_saved, RADIAL_PATCH_LEN)) {
        g_radial_patched = 0;
        mx_log("radial sweep off: the game's own");
    }
}

/* Each frame, between the game's frames: the patch follows the chosen shape, and the shape's numbers are set. */
static void brush_frame(void)
{
    double sweep_deg;
    int side;
    radial_patch(gradtool_sweep(&sweep_deg, &side));
    g_brush_shape = curvetool_brush_shape();
    double angle = curvetool_brush_angle() * 3.14159265358979 / 180;
    g_brush_cos = cos(angle), g_brush_sin = sin(angle);
    /* A turned square sits inside the brush's round reach (the game only visits cells within it), so its corners fit. */
    g_brush_square_scale = fmod(curvetool_brush_angle(), 90.0) != 0 ? sqrt(2.0) : 1;
    g_brush_oval = curvetool_brush_oval();
    brush_patch(g_brush_shape != BRUSH_CIRCLE);
}

/* The shape's outline at the mouse, inner and outer, while a game brush mode is on (the game still draws its circle). */
static void brush_outline(scene *s, const curve_view *v, double mx, double mz)
{
    int mode = game_editor_mode();
    if (g_brush_shape == BRUSH_CIRCLE || mode < 0 || mode > 8 || mode == 2 || !(g_brush_outer > 0) || s->items + 2 > SCENE_MAX_ITEMS)
        return;
    (void)v;
    static const float edge[4] = {1, 1, 1, 0.9f}, inside[4] = {1, 1, 1, 0.45f};
    float radii[2] = {g_brush_outer, g_brush_inner};
    for (int k = 0; k < 2; k++) {
        if (!(radii[k] > 0) || s->points + 65 > SCENE_MAX_POINTS)
            continue;
        scene_item *it = &s->item[s->items++];
        memset(it, 0, sizeof *it);
        it->kind = SCENE_LINE, it->closed = 1, it->first = s->points;
        memcpy(it->rgba, k ? inside : edge, sizeof it->rgba);
        int n = g_brush_shape == BRUSH_SQUARE ? 4 : 64;
        for (int i = 0; i < n; i++) {
            double lx, lz;
            if (g_brush_shape == BRUSH_SQUARE) {
                static const double corner[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
                lx = corner[i][0] * radii[k] / g_brush_square_scale, lz = corner[i][1] * radii[k] / g_brush_square_scale;
            } else {
                double a = i * 2 * 3.14159265358979 / n;
                lx = cos(a) * radii[k], lz = sin(a) * radii[k] * g_brush_oval;
            }
            /* back from the brush's turned frame to the map */
            s->pt[s->points][0] = (float)(mx + lx * g_brush_cos - lz * g_brush_sin);
            s->pt[s->points][1] = (float)(mz + lx * g_brush_sin + lz * g_brush_cos);
            s->points++;
            it->count++;
        }
    }
}

/* ── detours ── */
/* Gradient Length: before the game draws, and again at the end of the frame, so B and the profile are ours whichever
   order the game's drag step runs in. */
static void gradient_length_now(void)
{
    if (!pointers_ok() || curvetool_gradient_length() == GRADIENT_LENGTH_LOCKED)
        return;
    float x, z;
    world_at(mouse_x(), mouse_y(), &x, &z);
    game_gradient_length_frame(x, z, curvetool_gradient_length());
}

static uintptr_t draw_detour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    gradient_length_now();
    uintptr_t r = ((draw_fn)(void *)g_draw.trampoline)(a, b, c, d);
    InterlockedIncrement(&g_draw_calls);
    overlay_draw();
    return r;
}

static int g_our_press;   /* the left button went down on our panel, so its release is ours too */
static int g_place_press; /* the left button placed a saved gradient, so its release is ours too */

static int key_detour(int key, int down)
{
    InterlockedIncrement(&g_key_calls);
    if (pointers_ok()) {
        int left = *(int *)(*(uint8_t **)(g_base + RVA_PTR_KEYS) + 0x20);
        if (key == left && down && over_our_panel()) {
            g_our_press = 1;
            for (int i = 0; i < g_row_count; i++)
                if (over_row(i)) {
                    InterlockedIncrement(&g_presses);
                    if (g_panel[i].kind == ROWK_SLIDER) {
                        g_slide_row = i;
                        slide_to_mouse(i);
                    } else {
                        curvetool_press(i);
                    }
                    break;
                }
            return 1; /* ours: the editor never sees it, so it can't paint under the panel */
        }
        if (key == left && !down && (g_our_press || g_place_press)) {
            g_our_press = g_place_press = 0;
            g_slide_row = -1;
            return 1;
        }
        /* Typing a section's name: every key is ours (the game would act on s, f, numbers...); the mouse still works. */
        if (curvetool_typing() && key != left) {
            if (down && key < 0x1000)
                curvetool_type(key); /* each key also comes as a raw 0x10xx code; the character is the one */
            return 1;
        }
        if (curvetool_mode() == CURVE_MODE_OFF && !tool_on()) {
            const int *codes = *(int **)(g_base + RVA_PTR_KEYS);
            if (gradtool_placing()) {
                /* Placing a saved gradient: a click on the map puts its A there, Esc cancels. Pan, zoom and the rest
                   still go to the game, so the spot can be found first. */
                int hovered = *(int *)(g_base + RVA_HOVERED_WIDGET);
                if (key == left && down && hovered < 0 && mouse_x() >= 21.25f * (win_h() / 64.0f)) {
                    float x, z;
                    world_at(mouse_x(), mouse_y(), &x, &z);
                    gradtool_place(x, z);
                    g_place_press = 1;
                    return 1;
                }
                if (key != left && !always_the_games(key, codes) && !(key >= '0' && key <= '9')) {
                    if (down && key < 0x1000)
                        gradtool_key(key);
                    return 1;
                }
            } else if (down && key < 0x1000 && gradtool_key(key)) {
                return 1; /* Del in Gradient Placement or Edit */
            }
        }
        int own = curvetool_mode() != CURVE_MODE_OFF;
        curve_view v;
        if (key == left && !down && g_map_press) {
            g_map_press = 0;
            if (own && current_view(&v))
                curvetool_mouse(&v, CURVE_UP, mouse_x(), mouse_y(), 0, 0);
            else
                queue(EV_UP, key, 0);
            return 1;
        }
        if (key == 's' && down && !curvetool_save())
            mx_log("the game saves; our curves didn't (see the panel)");
        /* A press that began on the game's side ends there, wherever the mouse is now. */
        if (tool_on()) {
            const int *codes = *(int **)(g_base + RVA_PTR_KEYS);
            int hovered = *(int *)(g_base + RVA_HOVERED_WIDGET);
            if (down && ((key == left && hovered >= 0 && hovered < 16) || (key >= '0' && key <= '9'))) {
                tool_off(key); /* a game mode button or mode key: the game's tool takes over */
            } else if (key == left && hovered < 0 && mouse_x() >= 21.25f * (win_h() / 64.0f)) {
                /* On the map, not on the game's panel (its backdrop is 21.25 units wide, with no widget there). */
                if (down) {
                    g_map_press = 1;
                    if (own && current_view(&v))
                        curvetool_mouse(&v, CURVE_DOWN, mouse_x(), mouse_y(), (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
                                        (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
                    else
                        queue(EV_DOWN, key, 1);
                }
                return 1; /* our tool's click: the map doesn't paint */
            } else if (key != left && !always_the_games(key, codes)) {
                if (own) {
                    if (down && key < 0x1000)
                        curvetool_key(key); /* each key also comes as a raw 0x10xx code; the character is the one */
                } else {
                    queue(EV_KEY, key, down);
                }
                return 1;
            }
        }
    }
    return ((key_fn)(void *)g_key.trampoline)(key, down);
}

/* ── start, stop, status ── */
void ingame_start(uintptr_t base)
{
    g_base = base;
    char v[8];
    DWORD n = GetEnvironmentVariableA("MXMOD_NOHOOKS", v, sizeof v);
    if (n > 0 && n < sizeof v && strcmp(v, "0") != 0) {
        snprintf(g_status, sizeof g_status, "off (MXMOD_NOHOOKS)");
        mx_log("hooks off: MXMOD_NOHOOKS is set");
        return;
    }
    n = GetEnvironmentVariableA("MXMOD_READONLY", v, sizeof v);
    if (n > 0 && n < sizeof v && strcmp(v, "0") != 0) {
        snprintf(g_status, sizeof g_status, "off (MXMOD_READONLY)");
        mx_log("hooks off: MXMOD_READONLY is set, and patching code is a write");
        return;
    }
    /* The online tripwire (tripwire.c, armed since the DLL loaded): no code is patched in a session that could still
       reach a server. */
    if (!tripwire_armed()) {
        snprintf(g_status, sizeof g_status, "off (the online tripwire couldn't be armed)");
        mx_log("hooks off: the online tripwire couldn't be armed, so the game's code is left alone");
        return;
    }
    if (!hook_install(&g_draw, base, RVA_EDITOR_DRAW, FRAME_EDITOR_DRAW, (void *)draw_detour)) {
        snprintf(g_status, sizeof g_status, "failed (editor draw)");
        return;
    }
    if (!hook_install(&g_key, base, RVA_KEY_HANDLER, FRAME_KEY_HANDLER, (void *)key_detour)) {
        hook_remove(&g_draw);
        snprintf(g_status, sizeof g_status, "failed (key handler)");
        return;
    }
    snprintf(g_status, sizeof g_status, "on");
    curvetool_start();
}

void ingame_stop(void)
{
    brush_patch(0);
    radial_patch(0);
    hook_remove(&g_key);
    hook_remove(&g_draw);
}

const char *ingame_status(void) { return g_status; }

int ingame_debug_json(char *out, size_t cap)
{
    return snprintf(out, cap,
        "{\"drawCalls\": %ld, \"overlayRuns\": %ld, \"keyCalls\": %ld, \"skip\": \"%s\", \"w\": %g, \"h\": %g, "
        "\"orthoX\": %g, \"orthoY\": %g, \"unitsWide\": %g, \"unitsPerFt\": %g, \"midUnits\": %g, \"viewFt\": %g, "
        "\"sceneItems\": %d, \"scenePoints\": %d, \"tool\": \"%s\", \"curveMode\": %d, \"curves\": \"%s\", "
        "\"ownItems\": %d, \"tripwire\": %s, \"blocked\": %ld, \"row0\": [%d, %d, %d, %d], \"mouse\": [%d, %d]}",
        g_draw_calls, g_overlay_runs, g_key_calls, g_skip, g_last[0], g_last[1], g_last[2], g_last[3], g_last[4],
        g_last[5], g_last[6], g_last[7], scene_current()->items, scene_current()->points, scene_current()->tool,
        curvetool_mode(), curvetool_status(), g_own.items, tripwire_armed() ? "true" : "false", tripwire_blocked(), g_btn[0].rect[0], g_btn[0].rect[1], g_btn[0].rect[2],
        g_btn[0].rect[3],
        g_base && pointers_ok() ? mouse_x() : -1, g_base && pointers_ok() ? mouse_y() : -1);
}
long ingame_presses(void) { return g_presses; }
