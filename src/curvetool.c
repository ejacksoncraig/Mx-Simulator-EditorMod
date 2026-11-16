/* The curve tool in the game's editor, on the game thread.
   S1: the panel and the track's curves, read from mx-tools-curves.json and read again when it changes on disk.
   S2: Curve Path worked with the game's mouse and keys, as web/curve-tool.js does it, and saved back to the file. */
#include "curvetool.h"
#include "protocol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRAB_PX 9.0    /* curve-tool.js CURVE_GRAB_PX */
#define MIN_DRAG_PX 3.0 /* CURVE_MIN_DRAG_PX */

static curve_set g_curves;
static int g_curves_gen; /* goes up whenever the curves may be renumbered (read again, one deleted, an undo) */
static int g_mode;
static int g_selected = -1;
static int g_drawing; /* clicks add points to the selected curve, until it's finished */
static int g_dirty;   /* edited here since the file was read or written */
static wchar_t g_file[MAX_PATH];
static FILETIME g_file_time;
static char g_status[200] = "not started";
static int g_frames;
static double g_inner = 10, g_outer = 20; /* the app's defaults (curve-tool.js curveBake); S5 makes them sliders */

enum { DRAG_NONE, DRAG_PULL, DRAG_HANDLE, DRAG_ANCHOR, DRAG_PROFILE, DRAG_GROUP /* a range of profile points as one */,
       DRAG_MOVE /* a whole curve */ };
static int g_point = -1;       /* the profile point picked in Curve Profile */
static int g_pending_type;     /* Segment Type with no point picked (only the widget changes, as in the game) */
static double g_granularity;   /* the profile graph's snap, 0 = none */
static double g_trace_fit = 0.05; /* Set From Terrain: how close to the ground, ft */
static int g_grid; /* the ground grid: an index into GRID_FT (0 = off) */
static struct {
    int kind, curve, anchor, out_side, moved, changed, closes, resumes, split;
    double from_px, from_py;
} g_drag;
/* DRAG_MOVE: where the press was on the map and the curve's points then, so the move is from there, not cumulative. */
static double g_move_x, g_move_z;
static pen_anchor g_move_from[CURVE_MAX_ANCHORS];

/* ── the file ── */

/* The open track's folder: the one the editor has loaded (game_track), or --track from the command line if that can't be
   read. A full path as it is, else a name under %LOCALAPPDATA%\MX Simulator. */
static int track_dir(wchar_t *out, size_t cap)
{
    wchar_t name[MAX_PATH];
    size_t n = 0;
    char open[MAX_PATH];
    if (game_track(open, sizeof open) && MultiByteToWideChar(CP_ACP, 0, open, -1, name, MAX_PATH) > 1) {
        n = wcslen(name);
    } else {
        const wchar_t *cmd = GetCommandLineW(), *at = wcsstr(cmd, L"--track");
        if (!at)
            return 0;
        at += 7;
        while (*at == L' ' || *at == L'\t')
            at++;
        if (*at == L'"') {
            for (at++; *at && *at != L'"' && n + 1 < MAX_PATH; at++)
                name[n++] = *at;
        } else {
            for (; *at && *at != L' ' && *at != L'\t' && n + 1 < MAX_PATH; at++)
                name[n++] = *at;
        }
        name[n] = 0;
    }
    if (!n)
        return 0;
    for (wchar_t *c = name; *c; c++)
        if (*c == L'/')
            *c = L'\\';
    if (wcschr(name, L':') || name[0] == L'\\')
        return swprintf(out, cap, L"%ls", name) > 0;
    wchar_t local[MAX_PATH];
    DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return got > 0 && got < MAX_PATH && swprintf(out, cap, L"%ls\\MX Simulator\\%ls", local, name) > 0;
}

static void status_now(void)
{
    snprintf(g_status, sizeof g_status, "%d curves%s", g_curves.count, g_dirty ? ", not saved" : "");
}

static void curve_undo_clear(void);
static void load(void)
{
    curve_undo_clear(); /* the curves are read again: older copies are of something else */
    g_curves_gen++;
    HANDLE f = CreateFileW(g_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        g_curves.count = 0;
        g_selected = -1;
        snprintf(g_status, sizeof g_status, "no curves file yet");
        return;
    }
    LARGE_INTEGER size;
    char *text = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart < 16 * 1024 * 1024 && (text = malloc((size_t)size.QuadPart + 1)))
        ReadFile(f, text, (DWORD)size.QuadPart, &got, NULL);
    GetFileTime(f, NULL, NULL, &g_file_time);
    CloseHandle(f);
    if (!text) {
        snprintf(g_status, sizeof g_status, "couldn't read the curves file");
        return;
    }
    static curve_set parsed;
    const char *why = curves_parse(text, got, &parsed);
    free(text);
    if (why) {
        snprintf(g_status, sizeof g_status, "curves file not read: %s", why);
        mx_log("%s", g_status);
        return; /* keep what was there */
    }
    g_curves = parsed;
    if (g_selected >= g_curves.count)
        g_selected = -1;
    g_dirty = 0;
    status_now();
    mx_log("curves: %d from %ls", g_curves.count, g_file);
}

/* Written to a temporary file next to it, then swapped in, so a crash never leaves half a file. */
int curvetool_save(void)
{
    if (!g_file[0] || !g_dirty)
        return 1;
    static char text[8 * 1024 * 1024];
    size_t n = curves_write(&g_curves, text, sizeof text);
    wchar_t tmp[MAX_PATH];
    swprintf(tmp, MAX_PATH, L"%ls.tmp", g_file);
    HANDLE f = n ? CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    DWORD put = 0;
    int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, (DWORD)n, &put, NULL) && put == n;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    ok = ok && MoveFileExW(tmp, g_file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        snprintf(g_status, sizeof g_status, "couldn't save the curves (%lu)", GetLastError());
        mx_log("%s", g_status);
        return 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(g_file, GetFileExInfoStandard, &info))
        g_file_time = info.ftLastWriteTime;
    g_dirty = 0;
    status_now();
    mx_log("curves saved: %d to %ls", g_curves.count, g_file);
    return 1;
}

static void set_live(int on);

/* Follow the track the editor has open: when it changes, save any unsaved curves to the track they belong to, then read
   the new track's. */
static void follow_track(void)
{
    wchar_t dir[MAX_PATH], file[MAX_PATH];
    if (!track_dir(dir, MAX_PATH)) {
        if (!g_file[0])
            snprintf(g_status, sizeof g_status, "no track open in the editor");
        return;
    }
    swprintf(file, MAX_PATH, L"%ls\\mx-tools-curves.json", dir);
    if (!wcscmp(file, g_file))
        return;
    if (g_file[0] && g_dirty && !curvetool_save())
        mx_log("curve tool: couldn't save the last track's curves before switching");
    set_live(0);
    g_selected = -1, g_drawing = 0, g_point = -1, g_drag.kind = DRAG_NONE;
    g_dirty = 0;
    memcpy(g_file, file, sizeof g_file);
    mx_log("curve tool: track %ls", dir);
    load();
}

static void settings_load(void);
static void settings_frame(void);
void curvetool_start(void)
{
    settings_load();
    follow_track();
}

static void live_frame(void);
static void brush_size_frame(void);

void curvetool_frame(void)
{
    settings_frame();
    brush_size_frame();
    live_frame();
    stamptool_frame(g_mode == CURVE_MODE_STAMP);
    /* About once a second: the track the editor has open, then whether the app rewrote its curves file. Edits here that
       aren't saved win, and say so. */
    if (++g_frames % 60)
        return;
    follow_track();
    if (!g_file[0])
        return;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(g_file, GetFileExInfoStandard, &info))
        return;
    if (CompareFileTime(&info.ftLastWriteTime, &g_file_time) == 0)
        return;
    if (g_dirty) {
        snprintf(g_status, sizeof g_status, "%d curves, not saved; the file changed on disk too", g_curves.count);
        return;
    }
    load();
}

int curvetool_mode(void) { return g_mode; }
const char *curvetool_status(void) { return g_status; }

static void set_live(int on);

void curvetool_set_mode(int mode)
{
    if (mode == CURVE_MODE_STAMP && !MXMOD_STAMP)
        return; /* the stamp is shelved */
    if (g_mode == CURVE_MODE_STAMP && mode != CURVE_MODE_STAMP)
        stamptool_off(); /* leaving the stamp puts its preview's ground back */
    if (mode == CURVE_MODE_STAMP)
        set_live(0);     /* one preview at a time */
    if (mode == CURVE_MODE_OFF) {
        g_drag.kind = DRAG_NONE;
        set_live(0); /* leaving the tool puts the preview's ground back */
    }
    g_mode = mode;
}

/* ── editing (web/curve-tool.js) ── */

static double to_px_x(const curve_view *v, double x) { return (x - v->cx) / v->view_ft * v->h + v->w / 2; }
static double to_px_y(const curve_view *v, double z) { return (z - v->cz) / v->view_ft * v->h + v->h / 2; }
static void to_px(void *ctx, double x, double z, double *px, double *py)
{
    const curve_view *v = ctx;
    *px = to_px_x(v, x), *py = to_px_y(v, z);
}
static double px_from(const curve_view *v, double x, double z, double px, double py)
{
    return hypot(to_px_x(v, x) - px, to_px_y(v, z) - py);
}

enum { THING_NONE, THING_HANDLE, THING_ANCHOR, THING_SEGMENT };
typedef struct {
    int type, anchor, out_side;
    pen_hit hit;
} thing;

/* Handles first, since a short one sits right by its point, then points, then the path itself. */
static thing thing_at(const curve_view *v, const curve *c, double px, double py)
{
    thing t = {THING_NONE, -1, 0, {0, 0, 0}};
    for (int i = 0; i < c->anchors; i++) {
        const pen_anchor *a = &c->a[i];
        if ((a->ox != a->x || a->oz != a->z) && px_from(v, a->ox, a->oz, px, py) <= GRAB_PX)
            return t.type = THING_HANDLE, t.anchor = i, t.out_side = 1, t;
        if ((a->ix != a->x || a->iz != a->z) && px_from(v, a->ix, a->iz, px, py) <= GRAB_PX)
            return t.type = THING_HANDLE, t.anchor = i, t.out_side = 0, t;
    }
    for (int i = 0; i < c->anchors; i++)
        if (px_from(v, c->a[i].x, c->a[i].z, px, py) <= GRAB_PX)
            return t.type = THING_ANCHOR, t.anchor = i, t;
    if (pen_nearest(c, to_px, (void *)v, px, py, GRAB_PX, &t.hit))
        t.type = THING_SEGMENT;
    return t;
}

static int g_stretch;

/* A path changed: the profile follows the new length, extending or cutting with no point moved, or stretching to fit
   (Path Change). */
static void commit_shape(curve *c)
{
    profile_fit(c, pen_length(c), g_stretch);
    g_dirty = 1;
    status_now();
}

static void delete_curve(int i)
{
    if (i < 0 || i >= g_curves.count)
        return;
    set_live(0);
    memmove(&g_curves.c[i], &g_curves.c[i + 1], (size_t)(g_curves.count - i - 1) * sizeof g_curves.c[0]);
    g_curves.count--;
    g_curves_gen++;
    g_selected = -1;
    g_drawing = 0;
    g_dirty = 1;
    status_now();
}

/* Stop adding points. A lone point isn't a curve and couldn't be clicked once let go of, so that goes. */
static void finish_curve(void)
{
    g_drawing = 0;
    if (g_selected >= 0 && g_curves.c[g_selected].anchors < 2)
        delete_curve(g_selected);
}

static int pick_other(const curve_view *v, double px, double py)
{
    for (int i = 0; i < g_curves.count; i++)
        if (i != g_selected && thing_at(v, &g_curves.c[i], px, py).type != THING_NONE) {
            g_drawing = 0;
            g_selected = i;
            g_point = -1;
            return 1;
        }
    return 0;
}

/* Curve Profile: the graph sits at the curve's first point, s along +x and height up the map (x = ax + s, z = az - h),
   like the game's Gradient Edit. On a point picks it; strictly between two points' s adds one there, ready to drag. */
static void profile_down(const curve_view *v, double px, double py, double x, double z)
{
    curve *c = g_selected >= 0 && g_selected < g_curves.count && g_curves.c[g_selected].anchors > 1 ? &g_curves.c[g_selected] : NULL;
    if (!c) {
        pick_other(v, px, py);
        g_point = -1;
        return;
    }
    double s = x - c->a[0].x, h = c->a[0].z - z;
    double u = v->view_ft * 0.01 > 1 ? v->view_ft * 0.01 : 1;
    double pick = 0.5 * u > 6 * v->view_ft / v->h ? 0.5 * u : 6 * v->view_ft / v->h; /* editor.js pickFt */
    int index = profile_pick(c, s, h, pick), inserted = 0;
    if (index < 0) {
        index = profile_insert(c, s, h); /* at the mouse exactly: the game only snaps once a point is dragged */
        if (index < 0) {
            if (pick_other(v, px, py))
                g_point = -1;
            return;
        }
        inserted = 1;
        g_dirty = 1;
        status_now();
    }
    g_point = index;
    memset(&g_drag, 0, sizeof g_drag);
    g_drag.kind = DRAG_PROFILE, g_drag.curve = g_selected, g_drag.anchor = index, g_drag.moved = inserted;
    g_drag.from_px = px, g_drag.from_py = py;
}

static void profile_flatten(curve *c)
{
    profile_point a = {0, 0, 0}, b = {0, pen_length(c), 0};
    c->p[0] = a, c->p[1] = b, c->points = 2;
    g_point = -1;
    g_dirty = 1;
    status_now();
}

static void live_cancel(void);
static void say(const char *text);

/* What Set From Terrain found, in words. */
static void say_traced(int points, const trace_result *r)
{
    static char text[128];
    snprintf(text, sizeof text, "Profile traced: %d points, %d sharp edge%s, at most %.2f ft from the ground.", points, r->corners,
             r->corners == 1 ? "" : "s", r->worst_ft);
    say(text);
}

/* Set From Terrain: the game's own ground under the path (any preview taken off first, so it reads the real ground). */
static void profile_terrain(curve *c)
{
    live_cancel();
    game_map m;
    trace_result r;
    if (!game_heights(&m) || !profile_trace_ground(c, m.heights, m.width, m.cell_ft, g_trace_fit, &r)) {
        say("Couldn't read the ground along this curve.");
        return;
    }
    g_point = -1;
    g_dirty = 1;
    status_now();
    say_traced(c->points, &r);
}

/* The range of profile points (with the section library, below). */
static int range_down(const curve_view *v, double x, double z, int shift);
static void group_move(double x, double z);

static void mouse_down(const curve_view *v, double px, double py, double x, double z, int ctrl, int alt, int shift)
{
    if (g_mode == CURVE_MODE_PROFILE) {
        if (!range_down(v, x, z, shift))
            profile_down(v, px, py, x, z);
        return;
    }
    /* Shift+drag on any part of a curve (its line, a point, a handle) moves the whole curve, handles and all. On a curve
       that isn't selected it's picked first, so one drag does both. Not while drawing: clicks add points then. */
    if (shift && !g_drawing) {
        int hit = g_selected >= 0 && g_selected < g_curves.count &&
                  thing_at(v, &g_curves.c[g_selected], px, py).type != THING_NONE ? g_selected : -1;
        for (int i = 0; hit < 0 && i < g_curves.count; i++)
            if (thing_at(v, &g_curves.c[i], px, py).type != THING_NONE)
                hit = i;
        if (hit >= 0) {
            g_selected = hit;
            g_point = -1;
            memset(&g_drag, 0, sizeof g_drag);
            g_drag.kind = DRAG_MOVE, g_drag.curve = hit, g_drag.from_px = px, g_drag.from_py = py;
            g_move_x = x, g_move_z = z;
            memcpy(g_move_from, g_curves.c[hit].a, (size_t)g_curves.c[hit].anchors * sizeof g_move_from[0]);
            return;
        }
    }
    curve *c = g_selected >= 0 ? &g_curves.c[g_selected] : NULL;
    thing at = c ? thing_at(v, c, px, py) : (thing){THING_NONE, -1, 0, {0, 0, 0}};
    memset(&g_drag, 0, sizeof g_drag);
    g_drag.from_px = px, g_drag.from_py = py, g_drag.curve = g_selected;
    if (at.type == THING_HANDLE) {
        g_drag.kind = DRAG_HANDLE, g_drag.anchor = at.anchor, g_drag.out_side = at.out_side;
    } else if (at.type == THING_ANCHOR) {
        if (ctrl) {
            memmove(&c->a[at.anchor], &c->a[at.anchor + 1], (size_t)(c->anchors - at.anchor - 1) * sizeof c->a[0]);
            c->anchors--;
            if (c->anchors < 3)
                c->closed = 0;
            commit_shape(c);
        } else if (alt) {
            pen_toggle_corner(c, at.anchor);
            commit_shape(c);
        } else {
            /* While drawing, the first point closes the shape. Once finished, the last one picks the drawing back up. */
            g_drag.kind = DRAG_ANCHOR, g_drag.anchor = at.anchor;
            g_drag.closes = g_drawing && at.anchor == 0 && c->anchors > 2 && !c->closed;
            g_drag.resumes = !g_drawing && at.anchor == c->anchors - 1 && !c->closed;
        }
    } else if (at.type == THING_SEGMENT) {
        if (c->anchors >= CURVE_MAX_ANCHORS)
            return;
        int ia, ib;
        pen_segment(c, at.hit.segment, &ia, &ib);
        pen_anchor added = pen_split(&c->a[ia], &c->a[ib], at.hit.t);
        int where = ib == 0 ? c->anchors : ib;
        memmove(&c->a[where + 1], &c->a[where], (size_t)(c->anchors - where) * sizeof c->a[0]);
        c->a[where] = added;
        c->anchors++;
        g_drag.kind = DRAG_ANCHOR, g_drag.anchor = where, g_drag.changed = 1;
    } else {
        /* Drawing goes on through anything. Otherwise a click picks a curve, or lets go of one, and only empty ground
           with nothing selected starts a new one. */
        if (!g_drawing && pick_other(v, px, py))
            return;
        if (c && !g_drawing) {
            g_selected = -1;
        } else if (!c) {
            if (g_curves.count >= CURVE_MAX)
                return;
            curve *fresh = &g_curves.c[g_curves.count];
            memset(fresh, 0, sizeof *fresh);
            fresh->a[0] = pen_new_anchor(x, z);
            fresh->anchors = 1;
            g_selected = g_curves.count++;
            g_drawing = 1;
            g_drag.kind = DRAG_PULL, g_drag.curve = g_selected, g_drag.anchor = 0, g_drag.changed = 1;
        } else if (!c->closed && c->anchors < CURVE_MAX_ANCHORS) {
            c->a[c->anchors] = pen_new_anchor(x, z);
            g_drag.kind = DRAG_PULL, g_drag.anchor = c->anchors++, g_drag.changed = 1;
        }
    }
}

static void mouse_move(double px, double py, double x, double z, int alt)
{
    if (g_drag.kind == DRAG_GROUP) { /* first: on a game gradient, g_drag.curve is the gradient's index */
        group_move(x, z);
        return;
    }
    if (g_drag.kind == DRAG_NONE || g_drag.curve < 0 || g_drag.curve >= g_curves.count)
        return;
    if (g_drag.kind == DRAG_PROFILE) {
        curve *c = &g_curves.c[g_drag.curve];
        int index = profile_drag(c, g_drag.anchor, x - c->a[0].x, c->a[0].z - z, g_granularity);
        g_drag.moved = 1;
        g_dirty = 1;
        if (index < 0) { /* dragged past a neighbour: the game deletes the point and stops the drag */
            g_drag.kind = DRAG_NONE;
            g_point = -1;
            status_now();
            return;
        }
        g_point = index;
        return;
    }
    if (!g_drag.moved && hypot(px - g_drag.from_px, py - g_drag.from_py) < MIN_DRAG_PX)
        return;
    g_drag.moved = 1;
    if (g_drag.kind == DRAG_MOVE) {
        curve *c = &g_curves.c[g_drag.curve];
        double dx = x - g_move_x, dz = z - g_move_z;
        for (int k = 0; k < c->anchors; k++) {
            const pen_anchor *f = &g_move_from[k];
            pen_anchor *a = &c->a[k];
            a->x = f->x + dx, a->ix = f->ix + dx, a->ox = f->ox + dx;
            a->z = f->z + dz, a->iz = f->iz + dz, a->oz = f->oz + dz;
        }
        return;
    }
    pen_anchor *a = &g_curves.c[g_drag.curve].a[g_drag.anchor];
    if (g_drag.kind == DRAG_PULL) {
        pen_pull(a, x, z, alt);
    } else if (g_drag.kind == DRAG_HANDLE) {
        if (alt)
            g_drag.split = 1;
        pen_move_handle(a, g_drag.out_side, x, z, g_drag.split);
    } else {
        pen_move_anchor(a, x, z);
    }
}

static void mouse_up(void)
{
    int kind = g_drag.kind;
    g_drag.kind = DRAG_NONE;
    if (kind == DRAG_GROUP) {
        status_now();
        return;
    }
    if (kind == DRAG_NONE || g_drag.curve < 0 || g_drag.curve >= g_curves.count)
        return;
    curve *c = &g_curves.c[g_drag.curve];
    if (kind == DRAG_PROFILE) {
        status_now(); /* a profile edit keeps the path, so nothing to fit */
        return;
    }
    if (g_drag.closes && !g_drag.moved) {
        c->closed = 1;
        g_drawing = 0;
        commit_shape(c);
    } else if (g_drag.resumes && !g_drag.moved) {
        g_drawing = 1;
    } else if (g_drag.moved || g_drag.changed) {
        commit_shape(c);
    }
}

static void act_begin(void);
static void act_end(void);
static void curve_undo(int redo);
static int g_undo_n, g_redo_n; /* curve undo and redo steps (the undo section, before curvetool_press) */
static int g_mouse_act; /* a press on the map began an undo step; its release ends it */
void curvetool_mouse(const curve_view *v, int type, double px, double py, int ctrl, int alt, int shift)
{
    double x = (px - v->w / 2) / v->h * v->view_ft + v->cx, z = (py - v->h / 2) / v->h * v->view_ft + v->cz;
    if (g_mode == CURVE_MODE_STAMP) {
        stamptool_mouse(v, type, x, z, ctrl);
    } else if (type == CURVE_DOWN) {
        if (g_mouse_act) /* a release that never came: that step ends here */
            g_mouse_act = 0, act_end();
        act_begin(), g_mouse_act = 1; /* a press and its drag are one undo step */
        mouse_down(v, px, py, x, z, ctrl, alt, shift);
    } else if (type == CURVE_MOVE) {
        mouse_move(px, py, x, z, alt);
    } else {
        mouse_up();
        if (g_mouse_act)
            g_mouse_act = 0, act_end();
    }
}

int curvetool_dragging(void) { return g_mode == CURVE_MODE_STAMP ? stamptool_dragging() : g_drag.kind != DRAG_NONE; }

static int key_inner(int key);
int curvetool_key(int key)
{
    if (g_mode == CURVE_MODE_STAMP)
        return stamptool_key(key);
    int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (key == 26 || key == 25 || (ctrl && (key == 'z' || key == 'y'))) { /* Ctrl+Z / Ctrl+Y, as a control character or not */
        curve_undo(key == 25 || key == 'y');
        return 1;
    }
    act_begin();
    int used = key_inner(key);
    act_end();
    return used;
}
static int key_inner(int key)
{
    if (curvetool_range_key(key)) /* Esc lets go of a range first */
        return 1;
    if (key == 13 || key == 27) { /* Enter, Esc: finish */
        finish_curve();
        return 1;
    }
    if (key == 127 && g_selected >= 0) { /* Delete: the selected curve */
        delete_curve(g_selected);
        return 1;
    }
    if (key == 'f' && g_mode == CURVE_MODE_PROFILE && g_selected >= 0 && g_curves.c[g_selected].anchors > 1) {
        profile_flatten(&g_curves.c[g_selected]); /* f flattens, as in the game's Gradient Edit */
        return 1;
    }
    return 0;
}

/* ── baking into the game's ground: Live (no undo step) and Apply (one), core/game_link.py CurveInGame's rules ── */

static struct {
    int on;
    float *base;               /* the game's ground before the preview, rows x cols from (at_row, at_col) */
    int at_row, at_col, rows, cols;
    float *shown;              /* what was last written, s_rows x s_cols from (s_row, s_col); NULL when nothing is */
    int s_row, s_col, s_rows, s_cols;
    curve last;                /* the curve last previewed, to preview again only when it changes */
    double settings[8];        /* and the bake settings it used */
    int last_index, frames, wait;
    float lo, hi;              /* the track's height range before the preview (game_height_range); a write only widens it */
    float after_lo, after_hi;  /* and as our last write left it, so a widening by the game's own tools is kept */
    int have_range;
} g_live;
static double g_strength = 1, g_bank = 0;
static const double BANK_MAX = 0.5; /* ft of rise per ft out from the centre line: 0.5 is about 27 degrees */
static int g_add = 0, g_limit = CURVE_LIMIT_NONE, g_bake_side = CURVE_SIDE_BOTH;
static int g_ease_plain = 1; /* Ease Plain Points: curvemath.c eased */

static void live_forget(void)
{
    free(g_live.base), free(g_live.shown);
    g_live.base = g_live.shown = NULL;
    g_live.last_index = -1;
    g_live.have_range = 0;
}
/* The height range as it was before the preview. Save scales terrain.png to it, so a preview that reached higher or
   lower must not leave it wider (bermpreview.c does the same). */
static void live_put_range_back(void)
{
    float lo, hi;
    if (!g_live.have_range || !game_height_range(&lo, &hi))
        return;
    /* Past what our last write left, the game's own brush went there meanwhile: that part stays. */
    lo = lo < g_live.after_lo ? lo : g_live.lo;
    hi = hi > g_live.after_hi ? hi : g_live.hi;
    game_set_height_range(lo, hi);
}

/* Everything saved must still be as left: the ground from before, with the last preview on it. */
static int live_untouched(const game_map *m)
{
    if (!g_live.base)
        return 1;
    for (int r = 0; r < g_live.rows; r++)
        for (int c = 0; c < g_live.cols; c++) {
            int gr = g_live.at_row + r, gc = g_live.at_col + c;
            float want = g_live.base[(size_t)r * g_live.cols + c];
            if (g_live.shown && gr >= g_live.s_row && gr < g_live.s_row + g_live.s_rows && gc >= g_live.s_col &&
                gc < g_live.s_col + g_live.s_cols)
                want = g_live.shown[(size_t)(gr - g_live.s_row) * g_live.s_cols + (gc - g_live.s_col)];
            if (m->heights[(size_t)gr * m->width + gc] != want)
                return 0;
        }
    return 1;
}

/* Grow the saved ground to cover these cells. The new part comes from the game, which has no preview there. */
static int live_cover(const game_map *m, int row0, int row1, int col0, int col1)
{
    int r0 = row0, r1 = row1, c0 = col0, c1 = col1;
    if (g_live.base) {
        r0 = r0 < g_live.at_row ? r0 : g_live.at_row, c0 = c0 < g_live.at_col ? c0 : g_live.at_col;
        int er = g_live.at_row + g_live.rows - 1, ec = g_live.at_col + g_live.cols - 1;
        r1 = r1 > er ? r1 : er, c1 = c1 > ec ? c1 : ec;
        if (r0 == g_live.at_row && c0 == g_live.at_col && r1 == er && c1 == ec)
            return 1;
    } else {
        g_live.have_range = game_height_range(&g_live.lo, &g_live.hi); /* the first cover: nothing of ours is on it yet */
        g_live.after_lo = g_live.lo, g_live.after_hi = g_live.hi;
    }
    int rows = r1 - r0 + 1, cols = c1 - c0 + 1;
    float *grown = malloc((size_t)rows * cols * sizeof(float));
    if (!grown)
        return 0;
    for (int r = 0; r < rows; r++)
        memcpy(grown + (size_t)r * cols, m->heights + (size_t)(r0 + r) * m->width + c0, (size_t)cols * sizeof(float));
    if (g_live.base) {
        for (int r = 0; r < g_live.rows; r++)
            memcpy(grown + (size_t)(g_live.at_row - r0 + r) * cols + (g_live.at_col - c0), g_live.base + (size_t)r * g_live.cols,
                   (size_t)g_live.cols * sizeof(float));
        free(g_live.base);
    }
    g_live.base = grown, g_live.at_row = r0, g_live.at_col = c0, g_live.rows = rows, g_live.cols = cols;
    return 1;
}

/* Write what's changing: cells the paint moved, plus wherever the last preview was (so it's put back there). */
static const char *live_write(const double *painted, int final)
{
    int rows = g_live.rows, cols = g_live.cols, top = rows, bottom = -1, left = cols, right = -1;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int gr = g_live.at_row + r, gc = g_live.at_col + c;
            int was_shown = g_live.shown && gr >= g_live.s_row && gr < g_live.s_row + g_live.s_rows && gc >= g_live.s_col &&
                            gc < g_live.s_col + g_live.s_cols;
            if (was_shown || (float)painted[(size_t)r * cols + c] != g_live.base[(size_t)r * cols + c]) {
                top = r < top ? r : top, bottom = r > bottom ? r : bottom;
                left = c < left ? c : left, right = c > right ? c : right;
            }
        }
    if (bottom < 0)
        return NULL; /* nothing changes */
    int h = bottom - top + 1, w = right - left + 1;
    float *block = malloc((size_t)h * w * sizeof(float));
    if (!block)
        return "out of memory";
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            block[(size_t)r * w + c] = (float)painted[(size_t)(top + r) * cols + left + c];
    const char *why = game_write_heights(g_live.at_col + left, g_live.at_row + top, w, h, block, g_live.shown == NULL, final);
    if (why) {
        free(block);
        return why;
    }
    free(g_live.shown);
    g_live.shown = block, g_live.s_row = g_live.at_row + top, g_live.s_col = g_live.at_col + left, g_live.s_rows = h,
    g_live.s_cols = w;
    if (g_live.have_range)
        game_height_range(&g_live.after_lo, &g_live.after_hi);
    return NULL;
}

/* Paint the curve on the ground as it was before the preview. final leaves it as one undo step. */
static const char *live_show(const curve *c, int final)
{
    game_map m;
    if (!game_heights(&m))
        return "no track loaded";
    if (!live_untouched(&m)) {
        live_forget();
        return "The ground under the preview changed in the game, so it stopped. Turn Live on to start over.";
    }
    int b[4];
    if (!curve_bounds(c, m.cell_ft, g_outer, m.width, b))
        return "This curve is off the map.";
    if (!live_cover(&m, b[0], b[1], b[2], b[3]))
        return "out of memory";
    size_t cells = (size_t)g_live.rows * g_live.cols;
    double *painted = malloc(cells * sizeof(double));
    if (!painted)
        return "out of memory";
    for (size_t i = 0; i < cells; i++)
        painted[i] = g_live.base[i];
    if (final)
        live_put_range_back(); /* Apply widens it again only as far as the baked curve needs */
    const char *why = curve_paint(painted, g_live.at_row, g_live.at_col, g_live.rows, g_live.cols, m.width, m.cell_ft, c,
                                  g_inner, g_outer, g_strength, g_add, g_limit, g_bank, g_bake_side, NULL)
                          ? live_write(painted, final)
                          : "couldn't bake this curve";
    free(painted);
    if (final && !why)
        live_forget();
    return why;
}

/* Put back the ground the preview covered. No undo step. */
static void live_cancel(void)
{
    game_map m;
    if (g_live.shown && game_heights(&m) && live_untouched(&m)) {
        size_t cells = (size_t)g_live.rows * g_live.cols;
        double *ground = malloc(cells * sizeof(double));
        if (ground) {
            for (size_t i = 0; i < cells; i++)
                ground[i] = g_live.base[i];
            if (!live_write(ground, 0))
                live_put_range_back();
            free(ground);
        }
    }
    live_forget();
}

int curvetool_before_save(void)
{
    /* The game reads the ground as it is: the preview comes off, and comes back after a moment (live_forget makes it). */
    if (!g_live.shown)
        return 1;
    game_map m;
    if (game_heights(&m) && live_untouched(&m)) {
        size_t cells = (size_t)g_live.rows * g_live.cols;
        double *ground = malloc(cells * sizeof(double));
        if (!ground)
            return 0;
        for (size_t i = 0; i < cells; i++)
            ground[i] = g_live.base[i];
        const char *why = live_write(ground, 0);
        free(ground);
        if (why)
            return 0; /* still on the ground, and still kept, so it can come off later */
        live_put_range_back(); /* before the game's save scales terrain.png to it */
    }
    live_forget();
    g_live.wait = 30;
    return 1;
}

static void say(const char *text)
{
    snprintf(g_status, sizeof g_status, "%s", text);
    game_status(text);
    mx_log("curve tool: %s", text);
}

static void set_live(int on)
{
    if (!on && g_live.on)
        live_cancel();
    g_live.on = on;
    g_live.last_index = -1;
}

/* Each frame with Live on: preview again when the picked curve changed, at most every other frame. */
static void live_frame(void)
{
    if (!g_live.on)
        return;
    if (g_live.wait > 0) {
        g_live.wait--;
        return;
    }
    if (g_selected < 0 || g_selected >= g_curves.count || g_curves.c[g_selected].anchors < 2) {
        if (g_live.shown)
            live_cancel();
        return;
    }
    if (++g_live.frames % 2)
        return;
    const curve *c = &g_curves.c[g_selected];
    double settings[8] = {g_inner, g_outer, g_bank, g_strength, g_add, g_limit, g_bake_side, g_ease_plain};
    if (g_live.last_index == g_selected && !memcmp(&g_live.last, c, sizeof *c) &&
        !memcmp(g_live.settings, settings, sizeof settings))
        return;
    const char *why = live_show(c, 0);
    if (why) {
        set_live(0);
        say(why);
        return;
    }
    g_live.last = *c, g_live.last_index = g_selected;
    memcpy(g_live.settings, settings, sizeof settings);
}

static void apply_selected(void)
{
    if (g_selected < 0 || g_curves.c[g_selected].anchors < 2) {
        say("Pick a curve with at least two points first.");
        return;
    }
    const char *why = live_show(&g_curves.c[g_selected], 1);
    g_live.on = 0;
    if (why)
        say(why);
    else
        say("Applied to the ground. Backspace takes it back; s saves it.");
}

/* ── the panel (web/curve-tool.js curvePanelRows, with sliders where the app types numbers) ── */

enum {
    ROW_PATH, ROW_PROFILE, ROW_FINISH, ROW_NEW, ROW_DELETE, ROW_SAVE, ROW_LIVE, ROW_APPLY, ROW_LENGTH, ROW_RESIZE,
    ROW_INNER, ROW_OUTER, ROW_BANK, ROW_STRENGTH, ROW_BAKE_AS, ROW_SUBMODE, ROW_TURN, ROW_SEGMENT, ROW_GRANULARITY,
    ROW_FROM_TERRAIN, ROW_FLATTEN, ROW_SLOPE, ROW_HEIGHT, ROW_GRADIENT_LENGTH, ROW_SEC_START, ROW_SEC_END, ROW_SEC_SAVE,
    ROW_SEC_CHOICE, ROW_SEC_PASTE, ROW_SEC_DELETE, ROW_SEC_NONE, ROW_SEC_ITEM, ROW_SEC_RENAME, ROW_SEC_KEEP, ROW_SEC_CANCEL,
    ROW_SEC_TYPED, ROW_BRUSH_SHAPE, ROW_BRUSH_ANGLE, ROW_BRUSH_OVAL, ROW_BRUSH_TRI_WIDTH, ROW_BRUSH_TRI_LEAN, ROW_BRUSH_NOTE, ROW_HEADER, ROW_SEC_CLEAR,
    ROW_DUPLICATE, ROW_RNG_CLEAR, ROW_BRUSH_OUTER, ROW_BRUSH_INNER, ROW_PRE_CHOICE, ROW_PRE_ITEM, ROW_PRE_SAVE,
    ROW_PRE_RENAME, ROW_PRE_DELETE, ROW_PRE_TYPED, ROW_PRE_KEEP, ROW_PRE_CANCEL, ROW_STAMP, ROW_TRACE_FIT, ROW_TRACE_GAME,
    ROW_SEC_UNDO, ROW_PRE_UNDO, ROW_CURVE_UNDO, ROW_CURVE_REDO, ROW_GRID, ROW_BAKE_SIDE, ROW_EASE_PLAIN
};
/* The panel is built into BUILD_ROWS rows, then folded groups are taken out and the first MAX_PANEL_ROWS shown; the
   kinds and arguments are packed down with the rows, so a shown row's index finds them. */
enum { BUILD_ROWS = 160 };
static int g_row_arg[BUILD_ROWS]; /* a list row's section, or a gradtool row's argument */
static int g_sec_choice;  /* the section chosen in the Section row */
static int g_sec_list_open;   /* the Section dropdown shows every saved section */
enum { TYPING_NONE, TYPING_SECTION, TYPING_PRESET }; /* what a typed name is for */
static int g_typing;          /* Rename Section or Rename Preset: the keyboard is ours */
static int g_typing_kind;     /* TYPING_PRESET: which list (PRESET_*) */
static int g_pre_choice[PRESET_KINDS], g_pre_open[PRESET_KINDS]; /* each preset list's chosen one, dropdown open */
/* Undo Delete Section / Undo Delete Preset: the last few deleted, with where they were, put back newest first. */
enum { DELETED_KEEP = 16 };
static struct { section s; int at; } g_sec_deleted[DELETED_KEEP];
static struct { preset p; int at; } g_pre_deleted[PRESET_KINDS][DELETED_KEEP];
static int g_sec_deleted_n, g_pre_deleted_n[PRESET_KINDS];
static char g_typed[48];
static char g_typed_label[64];

int curvetool_typing(void) { return g_typing; }

static void say(const char *text);

static void keep_typed(void)
{
    int what = g_typing;
    g_typing = TYPING_NONE;
    const char *why = what == TYPING_SECTION ? library_rename(g_sec_choice, g_typed)
                                             : preset_rename(g_typing_kind, g_pre_choice[g_typing_kind], g_typed);
    say(why ? why : "Renamed.");
}

void curvetool_type(int key)
{
    size_t n = strlen(g_typed);
    if (key == 13 || key == 10)
        keep_typed();
    else if (key == 27)
        g_typing = TYPING_NONE;
    else if (key == 8 && n)
        g_typed[n - 1] = 0;
    else if (key >= 32 && key < 127 && n + 1 < sizeof g_typed)
        g_typed[n] = (char)key, g_typed[n + 1] = 0;
}
static const char *const SEGMENT_OPTIONS[] = {"Line", "Curve"};

/* Brush shapes for the game's own brush (ingame.c patches its distance while one isn't Circle). */
static const char *const SHAPE_OPTIONS[] = {"Circle", "Square", "Oval", "Triangle"};
static int g_brush_shape = BRUSH_CIRCLE;
static double g_brush_angle = 0, g_brush_oval = 0.5;
int curvetool_brush_shape(void) { return g_brush_shape; }
double curvetool_brush_angle(void) { return g_brush_angle; }
double curvetool_brush_oval(void) { return g_brush_oval; }
static double g_brush_tri_width = 1.1547005383792517, g_brush_tri_lean = 0; /* starts equilateral */
double curvetool_brush_tri_width(void) { return g_brush_tri_width; }
double curvetool_brush_tri_lean(void) { return g_brush_tri_lean; }

/* ── the section library on a profile: the game's Gradient Edit (its selected gradient) or our Curve Profile ── */

enum { SEC_NONE, SEC_GAME, SEC_CURVE };
static int g_sec_start = -1, g_sec_end = -1;
static int g_sec_ctx, g_sec_owner = -1; /* whose profile the marks are on */
static char g_sec_start_label[48], g_sec_end_label[48];

static int sec_context(void)
{
    if (g_mode == CURVE_MODE_PROFILE && g_selected >= 0 && g_selected < g_curves.count && g_curves.c[g_selected].anchors > 1)
        return SEC_CURVE;
    if (g_mode == CURVE_MODE_OFF && game_editor_mode() == 10)
        return SEC_GAME;
    return SEC_NONE;
}

/* The profile the library works on now: its points, room, picked point and owner (gradient or curve index). */
static int sec_profile(int ctx, profile_point *out, int *n, int *room, int *picked, int *owner)
{
    if (ctx == SEC_CURVE) {
        const curve *c = &g_curves.c[g_selected];
        memcpy(out, c->p, (size_t)c->points * sizeof c->p[0]);
        *n = c->points, *room = CURVE_MAX_POINTS, *picked = g_point, *owner = g_selected;
        return 1;
    }
    return ctx == SEC_GAME && game_selected_profile(owner, out, CURVE_MAX_POINTS, n, room, picked);
}

static void sec_follow(int ctx, int owner)
{
    if (ctx != g_sec_ctx || owner != g_sec_owner)
        g_sec_start = g_sec_end = -1, g_sec_ctx = ctx, g_sec_owner = owner;
}

/* A gradient was deleted or added, so the same index can be another gradient now. */
static int g_rng_a = -1, g_rng_b = -1; /* a range of profile points: Shift+click its first, then its last */

void curvetool_gradients_changed(void)
{
    if (g_sec_ctx == SEC_GAME)
        g_sec_start = g_sec_end = -1, g_sec_owner = -1;
    g_rng_a = g_rng_b = -1;
}

/* ── a range of profile points, dragged as one: the game's Gradient Edit (its selected gradient) or our Curve Profile ──
   Shift+click a point to start the range, Shift+click another to end it: every point from one to the other is in it.
   A plain click on a point in it drags them all (profile_group_drag); a plain click anywhere else lets go of it. The
   range belongs to one profile with one point count, so any added or deleted point lets go of it too. */
static int g_rng_ctx, g_rng_owner = -1, g_rng_n;
static struct {
    int ctx, owner, n, first, last, held;
    double ax, az;
    profile_point from[CURVE_MAX_POINTS]; /* the profile as the drag began */
} g_group;

static int rng_first(void) { return g_rng_a < g_rng_b ? g_rng_a : g_rng_b; }
static int rng_last(void) { return g_rng_a > g_rng_b ? g_rng_a : g_rng_b; }

/* The range still fits the profile being edited now (else it's let go of). */
static int rng_valid(int ctx, int owner, int n)
{
    if (ctx != g_rng_ctx || owner != g_rng_owner || n != g_rng_n || rng_last() >= n)
        g_rng_a = g_rng_b = -1, g_rng_ctx = ctx, g_rng_owner = owner, g_rng_n = n;
    return g_rng_a >= 0;
}

/* Where the graph's s = 0, h = 0 is on the map: A, the start of the gradient or curve. */
static int graph_origin(int ctx, int owner, double *ax, double *az)
{
    if (ctx == SEC_CURVE) {
        *ax = g_curves.c[owner].a[0].x, *az = g_curves.c[owner].a[0].z;
        return 1;
    }
    static gradient_copy g;
    if (!game_gradient_copy(owner, &g))
        return 0;
    *ax = g.ax, *az = g.az;
    return 1;
}

static int range_down(const curve_view *v, double x, double z, int shift)
{
    static profile_point p[CURVE_MAX_POINTS];
    int ctx = sec_context(), n, room, picked, owner;
    double ax, az;
    if (ctx == SEC_NONE || !sec_profile(ctx, p, &n, &room, &picked, &owner) || !graph_origin(ctx, owner, &ax, &az))
        return 0;
    int have = rng_valid(ctx, owner, n);
    double s = x - ax, h = az - z;
    double u = v->view_ft * 0.01 > 1 ? v->view_ft * 0.01 : 1;
    double radius = 0.5 * u > 6 * v->view_ft / v->h ? 0.5 * u : 6 * v->view_ft / v->h; /* as profile_down picks */
    int at = -1;
    double best = radius;
    for (int i = 0; i < n; i++) {
        double d = hypot(p[i].s - s, p[i].h - h);
        if (d <= best)
            at = i, best = d;
    }
    if (shift) {
        if (at < 0)
            return 0; /* Shift on empty graph: an ordinary click */
        if (!have)
            g_rng_a = at;
        g_rng_b = at;
        if (ctx == SEC_CURVE)
            g_point = at;
        return 1;
    }
    if (at >= 0 && have && rng_first() < rng_last() && at >= rng_first() && at <= rng_last()) {
        g_group.ctx = ctx, g_group.owner = owner, g_group.n = n, g_group.held = at;
        g_group.first = rng_first(), g_group.last = rng_last(), g_group.ax = ax, g_group.az = az;
        memcpy(g_group.from, p, (size_t)n * sizeof p[0]);
        memset(&g_drag, 0, sizeof g_drag);
        g_drag.kind = DRAG_GROUP, g_drag.curve = owner;
        if (ctx == SEC_CURVE)
            g_point = at;
        return 1;
    }
    g_rng_a = g_rng_b = -1; /* a plain click somewhere else lets go of the range, then does what it always does */
    return 0;
}

static void group_move(double x, double z)
{
    static profile_point out[CURVE_MAX_POINTS];
    int ctx = sec_context(), owner = ctx == SEC_CURVE ? g_selected : ctx == SEC_GAME ? game_selected_gradient() : -1;
    if (ctx != g_group.ctx || owner != g_group.owner) { /* the gradient or curve went away mid-drag */
        g_drag.kind = DRAG_NONE;
        return;
    }
    /* Granularity: ours on a curve, the game's own Granularity slider on a game gradient (0 if it can't be read). */
    double snap = ctx == SEC_CURVE ? g_granularity : 0, gs, gmin, gmax;
    if (ctx == SEC_GAME && !game_slider_get(GAME_SLIDER_GRANULARITY, &snap, &gs, &gmin, &gmax))
        snap = 0;
    profile_group_drag(g_group.from, g_group.n, g_group.first, g_group.last, g_group.held, x - g_group.ax,
                       g_group.az - z, snap, out);
    if (ctx == SEC_CURVE) {
        curve *c = &g_curves.c[owner];
        if (c->points != g_group.n) {
            g_drag.kind = DRAG_NONE;
            return;
        }
        memcpy(c->p, out, (size_t)g_group.n * sizeof out[0]);
        g_dirty = 1;
    } else {
        const char *why = game_set_profile(owner, out, g_group.n, g_group.held);
        if (why) {
            say(why);
            g_drag.kind = DRAG_NONE;
        }
    }
}

int curvetool_graph_down(const curve_view *v, double px, double py, int shift)
{
    if (sec_context() != SEC_GAME)
        return 0;
    double x = (px - v->w / 2) / v->h * v->view_ft + v->cx, z = (py - v->h / 2) / v->h * v->view_ft + v->cz;
    return range_down(v, x, z, shift);
}

int curvetool_range_key(int key)
{
    if (key != 27 || g_rng_a < 0 || sec_context() != g_rng_ctx)
        return 0;
    g_rng_a = g_rng_b = -1;
    say("Selection cleared.");
    return 1;
}

static void sec_press(int kind)
{
    int ctx = sec_context(), n, room, picked, owner;
    static profile_point p[CURVE_MAX_POINTS], out[CURVE_MAX_POINTS];
    if (!sec_profile(ctx, p, &n, &room, &picked, &owner)) {
        say("Pick a gradient (or a curve) first.");
        return;
    }
    sec_follow(ctx, owner);
    const char *why = NULL;
    if (kind == ROW_SEC_START || kind == ROW_SEC_END) {
        if (picked < 0 || picked >= n) {
            say("Pick a point on the profile first, then mark it.");
            return;
        }
        *(kind == ROW_SEC_START ? &g_sec_start : &g_sec_end) = picked;
        return;
    }
    if (kind == ROW_SEC_SAVE) {
        if (g_sec_start < 0 || g_sec_end >= n || g_sec_end <= g_sec_start)
            why = "Mark a start point, then an end point further along.";
        else
            why = library_add(p, g_sec_start, g_sec_end);
        if (!why) {
            g_sec_choice = library_count() - 1;
            static char done[96];
            snprintf(done, sizeof done, "Saved: %s.", library_get(g_sec_choice)->name);
            say(done);
            g_sec_start = g_sec_end = -1;
            return;
        }
    } else if (kind == ROW_SEC_CHOICE) {
        if (library_count())
            g_sec_choice = (g_sec_choice + 1) % library_count();
        return;
    } else if (kind == ROW_SEC_DELETE) {
        section keep = library_get(g_sec_choice) ? *library_get(g_sec_choice) : (section){0};
        why = library_delete(g_sec_choice);
        if (!why) {
            if (g_sec_deleted_n == DELETED_KEEP)
                memmove(&g_sec_deleted[0], &g_sec_deleted[1], (DELETED_KEEP - 1) * sizeof g_sec_deleted[0]), g_sec_deleted_n--;
            g_sec_deleted[g_sec_deleted_n].s = keep, g_sec_deleted[g_sec_deleted_n++].at = g_sec_choice;
            g_sec_choice = g_sec_choice >= library_count() ? library_count() - 1 : g_sec_choice;
            say("Section deleted. Undo Delete Section puts it back.");
            return;
        }
    } else if (kind == ROW_SEC_PASTE) {
        const section *s = library_get(g_sec_choice);
        int count = s ? section_paste(p, n, picked, s, room, out, &why) : -1;
        if (!s)
            why = "Save a section first.";
        if (count > 0) {
            if (ctx == SEC_CURVE) {
                curve *c = &g_curves.c[g_selected];
                memcpy(c->p, out, (size_t)count * sizeof out[0]);
                c->points = count;
                g_point = -1;
                g_dirty = 1;
                status_now();
            } else {
                why = game_set_profile(owner, out, count, -1);
            }
            if (!why) {
                g_sec_start = g_sec_end = -1;
                say("Pasted.");
                return;
            }
        }
    }
    if (why)
        say(why);
}
enum { MAX_PANEL_ROWS = 40 }; /* 1 + 40 * 1.5 units still fits the window's 64 */
static int g_row_kind[BUILD_ROWS]; /* g_stretch (Path Change: 0 Extend, 1 Stretch) is with commit_shape */
static const char *const RESIZE_OPTIONS[] = {"Extend", "Stretch"};
static const char *const BAKE_OPTIONS[] = {"Paint", "Add"};
static const char *const SUBMODE_OPTIONS[] = {"none", "raise", "lower"};
static const char *const BAKE_SIDE_OPTIONS[] = {"Both", "Left", "Right"};
static const char *const EASE_OPTIONS[] = {"Off", "On"};
static const char *const GRID_OPTIONS[] = {"Off", "5 ft", "10 ft", "25 ft", "50 ft", "100 ft"};
static const double GRID_FT[] = {0, 5, 10, 25, 50, 100};
static double g_grid_shown_ft; /* the grid spacing drawn last frame, 0 for none */

/* The smallest turning radius along the path, or a negative number if it's straight (curved.py tightest_turn). */
typedef struct {
    double (*p)[2];
    int n, cap;
} turn_points;
static void keep_turn(void *ctx, double x, double z, double along)
{
    (void)along;
    turn_points *t = ctx;
    if (t->n < t->cap)
        t->p[t->n][0] = x, t->p[t->n][1] = z, t->n++;
}
static double tightest_turn(const curve *c)
{
    static double pts[65536][2];
    turn_points t = {pts, 0, 65536};
    pen_sample(c, 2.0, keep_turn, &t);
    double best = -1, lx = 0, lz = 0, ll = 0;
    int have = 0;
    for (int i = 0; i + 1 < t.n; i++) {
        double dx = pts[i + 1][0] - pts[i][0], dz = pts[i + 1][1] - pts[i][1], l = hypot(dx, dz);
        if (l <= 1e-9)
            continue;
        if (have) {
            double turn = fabs(atan2(lx * dz - lz * dx, lx * dx + lz * dz));
            if (turn > 1e-4) {
                double r = (ll + l) / 2 / turn;
                best = best < 0 || r < best ? r : best;
            }
        }
        lx = dx, lz = dz, ll = l, have = 1;
    }
    return best;
}

/* ── the game brush's size in feet ──
   The game's Radius slider is in screen pixels (outer ft = Radius * viewFt / screenH), so its size on the ground
   changes with zoom; inner = Focus * outer. Our Outer and Inner sliders (and a brush preset) set a size in feet and
   hold it: each frame the game's Radius and Focus are set to give those feet at the current zoom. Moving the game's
   own Radius or Focus lets go of the hold. The Radius slider tops out at 128 px, but the hold writes past it (see
   brush_size_frame), so the size holds at any zoom; g_brush_capped only if it's bigger than the game's own brush
   gets at its lowest zoom. */
static int g_hold, g_hold_written, g_brush_capped;
static double g_hold_outer = 25, g_hold_inner = 12.5, g_last_radius, g_last_focus;

/* The game brush's size in feet now: what's held, else what the game's sliders give at this zoom. */
static int brush_feet(double *outer, double *inner)
{
    double r, rs, rmin, rmax, f, fs, fmin, fmax, view;
    int h;
    if (!game_slider_get(GAME_SLIDER_RADIUS, &r, &rs, &rmin, &rmax) || !game_slider_get(GAME_SLIDER_FOCUS, &f, &fs, &fmin, &fmax))
        return 0;
    if (g_hold) {
        *outer = g_hold_outer, *inner = g_hold_inner;
        return 1;
    }
    if (!ingame_view(&view, &h) || h <= 0)
        return 0;
    *outer = r * view / h, *inner = f * *outer;
    return 1;
}
int curvetool_brush_feet(double *outer, double *inner) { return brush_feet(outer, inner); }

static void hold_brush(double outer, double inner)
{
    g_hold_outer = outer < 0.5 ? 0.5 : outer;
    g_hold_inner = inner < 0 ? 0 : inner > g_hold_outer ? g_hold_outer : inner;
    g_hold = 1, g_hold_written = 0;
}

static void brush_size_frame(void)
{
    if (!g_hold)
        return;
    double r, rs, rmin, rmax, f, fs, fmin, fmax, view;
    int h;
    if (!game_slider_get(GAME_SLIDER_RADIUS, &r, &rs, &rmin, &rmax) || !game_slider_get(GAME_SLIDER_FOCUS, &f, &fs, &fmin, &fmax) ||
        !ingame_view(&view, &h) || h <= 0 || !(view > 0))
        return;
    /* The game's own slider moved (more than its step's rounding): the player took the brush back. */
    if (g_hold_written && (fabs(r - g_last_radius) > rs / 2 + 1e-9 || fabs(f - g_last_focus) > fs / 2 + 1e-9)) {
        g_hold = 0;
        say("Brush size let go: the game's Radius or Focus was moved.");
        return;
    }
    double want = g_hold_outer * h / view;
    /* Radius's own 1..128 px would stop the hold a couple of zoom steps in (a 50 ft brush needs ~150 px at zoom 2.5).
       The brush code reads Radius unclamped (outer ft = Radius * mapFt / (screenH * 2^zoom), mx.exe 1684 0x1400ed97b),
       so it's written past 128, kept to the feet the game's own 1..128 px reach somewhere in the Zoom slider's range:
       128 px at the lowest zoom is the biggest brush the game makes itself, 1 px at the highest the smallest. */
    double z, zs, zmin, zmax, lo = rmin, hi = rmax;
    if (game_slider_get(GAME_SLIDER_ZOOM, &z, &zs, &zmin, &zmax) && zmax >= zmin && z >= zmin && z <= zmax)
        lo = rmin * pow(2.0, z - zmax), hi = rmax * pow(2.0, z - zmin);
    g_brush_capped = want > hi;
    game_slider_set_unclamped(GAME_SLIDER_RADIUS, want, lo, hi);
    game_slider_set(GAME_SLIDER_FOCUS, g_hold_outer > 0 ? g_hold_inner / g_hold_outer : 0);
    game_slider_get(GAME_SLIDER_RADIUS, &g_last_radius, &rs, &rmin, &rmax);
    game_slider_get(GAME_SLIDER_FOCUS, &g_last_focus, &fs, &fmin, &fmax);
    g_hold_written = 1;
}

/* ── presets: the current settings saved under a name, and put back ── */

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static void preset_current(int kind, double *v)
{
    for (int i = 0; i < PRESET_FIELDS; i++)
        v[i] = NAN;
    if (kind == PRESET_CURVE) {
        v[PC_INNER] = g_inner, v[PC_OUTER] = g_outer, v[PC_BANK] = g_bank, v[PC_STRENGTH] = g_strength;
        v[PC_ADD] = g_add, v[PC_LIMIT] = g_limit, v[PC_SIDE] = g_bake_side;
        return;
    }
    double outer, inner, sweep;
    int side;
    if (brush_feet(&outer, &inner))
        v[PB_OUTER] = floor(outer * 100 + 0.5) / 100, v[PB_INNER] = floor(inner * 100 + 0.5) / 100;
    v[PB_SHAPE] = g_brush_shape, v[PB_ANGLE] = g_brush_angle, v[PB_OVAL] = g_brush_oval;
    v[PB_TRI_WIDTH] = g_brush_tri_width, v[PB_TRI_LEAN] = g_brush_tri_lean;
    gradtool_sweep(&sweep, &side);
    v[PB_SWEEP] = sweep, v[PB_SIDE] = side;
}

static void preset_use(int kind, int i)
{
    const preset *p = preset_get(kind, i);
    if (!p)
        return;
    const double *v = p->v;
#define HAVE(k) (v[k] == v[k]) /* not NAN */
    if (kind == PRESET_CURVE) {
        if (HAVE(PC_OUTER)) g_outer = clampd(v[PC_OUTER], 0.5, 100);
        if (HAVE(PC_INNER)) g_inner = clampd(v[PC_INNER], 0, 50);
        if (g_inner > g_outer) g_inner = g_outer;
        if (HAVE(PC_BANK)) g_bank = clampd(v[PC_BANK], -BANK_MAX, BANK_MAX);
        if (HAVE(PC_STRENGTH)) g_strength = clampd(v[PC_STRENGTH], 0, 1);
        if (HAVE(PC_ADD)) g_add = v[PC_ADD] != 0;
        if (HAVE(PC_LIMIT)) g_limit = (int)clampd(v[PC_LIMIT], 0, 2);
        if (HAVE(PC_SIDE)) g_bake_side = (int)clampd(v[PC_SIDE], 0, 2);
    } else {
        if (HAVE(PB_SHAPE)) g_brush_shape = (int)clampd(v[PB_SHAPE], 0, BRUSH_TRIANGLE);
        if (HAVE(PB_ANGLE)) g_brush_angle = clampd(v[PB_ANGLE], 0, 360);
        if (HAVE(PB_OVAL)) g_brush_oval = clampd(v[PB_OVAL], 0.1, 1);
        if (HAVE(PB_TRI_WIDTH)) g_brush_tri_width = clampd(v[PB_TRI_WIDTH], 0.25, 2.5);
        if (HAVE(PB_TRI_LEAN)) g_brush_tri_lean = clampd(v[PB_TRI_LEAN], -1, 1);
        gradtool_set_sweep(HAVE(PB_SWEEP) ? v[PB_SWEEP] : NAN, HAVE(PB_SIDE) ? (int)clampd(v[PB_SIDE], 0, 2) : -1);
        if (HAVE(PB_OUTER) && HAVE(PB_INNER))
            hold_brush(v[PB_OUTER], v[PB_INNER]);
    }
#undef HAVE
    static char done[96];
    snprintf(done, sizeof done, "Using preset: %s.", p->name);
    say(done);
}

/* A preset list's rows: the chosen one as a dropdown (a click on an item uses it), Save, Rename and Delete. */
static int preset_rows(int kind, panel_row *rows, int n, int max)
{
    panel_row *r = NULL;
#define PROW(kind_, rowk, label_)                                                                                 \
    (n < max && n < BUILD_ROWS ? (memset(r = &rows[n], 0, sizeof *r), g_row_kind[n] = (kind_), g_row_arg[n] = kind, \
                                      r->kind = (rowk), r->label = (label_), n++, 1)                                 \
                                   : (r = NULL, 0))
    PROW(ROW_HEADER, ROWK_HEADER, kind == PRESET_CURVE ? "CURVE PRESETS" : "BRUSH PRESETS");
    int count = preset_count(kind);
    if (count) {
        g_pre_choice[kind] = g_pre_choice[kind] < count ? g_pre_choice[kind] : 0;
        const char *const *names = preset_names(kind);
        static char choice_label[PRESET_KINDS][96], item_label[PRESET_KINDS][PRESET_MAX][64];
        snprintf(choice_label[kind], sizeof choice_label[kind], "%s  %s", names[g_pre_choice[kind]], g_pre_open[kind] ? "^" : "v");
        if (PROW(ROW_PRE_CHOICE, ROWK_BUTTON, choice_label[kind]))
            r->on = g_pre_open[kind];
        if (g_pre_open[kind]) {
            int list = kind == PRESET_CURVE ? PANEL_LIST_CURVE_PRESETS : PANEL_LIST_BRUSH_PRESETS, first, last;
            panel_list_range(list, count, &first, &last);
            if (first > 0 && PROW(PANEL_LIST_UP, ROWK_BUTTON, panel_list_more(list, 1, first)))
                r->list = list;
            for (int i = first; i < last; i++) {
                snprintf(item_label[kind][i], sizeof item_label[kind][i], "%s %s", i == g_pre_choice[kind] ? ">" : "  ", names[i]);
                if (PROW(ROW_PRE_ITEM, ROWK_BUTTON, item_label[kind][i])) {
                    r->on = i == g_pre_choice[kind];
                    r->list = list;
                    g_row_arg[n - 1] = kind * 1000 + i;
                }
            }
            if (last < count && PROW(PANEL_LIST_DOWN, ROWK_BUTTON, panel_list_more(list, 0, count - last)))
                r->list = list;
            if (PROW(ROW_HEADER, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, count > PANEL_LIST_SHOW ? "Click one to use it; the wheel scrolls" : "Click one to use it");
        } else if (PROW(ROW_HEADER, ROWK_HINT, "")) {
            snprintf(r->text, sizeof r->text, "Click the name to pick and use one");
        }
    } else if (PROW(ROW_HEADER, ROWK_HINT, "")) {
        snprintf(r->text, sizeof r->text, "Save these settings to reuse them");
    }
    PROW(ROW_PRE_SAVE, ROWK_BUTTON, "Save As New Preset");
    if (count) {
        if (g_typing == TYPING_PRESET && g_typing_kind == kind) {
            snprintf(g_typed_label, sizeof g_typed_label, "Name: %s_", g_typed);
            PROW(ROW_PRE_TYPED, ROWK_BUTTON, g_typed_label);
            PROW(ROW_PRE_KEEP, ROWK_BUTTON, "Keep Name (Enter)");
            PROW(ROW_PRE_CANCEL, ROWK_BUTTON, "Cancel (Esc)");
        } else {
            PROW(ROW_PRE_RENAME, ROWK_BUTTON, "Rename Preset");
        }
        PROW(ROW_PRE_DELETE, ROWK_BUTTON, "Delete Preset");
    }
    if (g_pre_deleted_n[kind]) {
        static char undo_label[PRESET_KINDS][64];
        snprintf(undo_label[kind], sizeof undo_label[kind], "Undo Delete Preset (%d)", g_pre_deleted_n[kind]);
        PROW(ROW_PRE_UNDO, ROWK_BUTTON, undo_label[kind]);
    }
#undef PROW
    return n;
}

static void preset_press(int kind_row, int arg)
{
    int kind = kind_row == ROW_PRE_ITEM ? arg / 1000 : arg;
    if (kind < 0 || kind >= PRESET_KINDS)
        return;
    const char *why = NULL;
    switch (kind_row) {
    case ROW_PRE_CHOICE:
        g_pre_open[kind] = !g_pre_open[kind];
        if (g_pre_open[kind])
            panel_list_show(kind == PRESET_CURVE ? PANEL_LIST_CURVE_PRESETS : PANEL_LIST_BRUSH_PRESETS, g_pre_choice[kind]);
        break;
    case ROW_PRE_ITEM:
        g_pre_choice[kind] = arg % 1000;
        g_pre_open[kind] = 0;
        preset_use(kind, g_pre_choice[kind]);
        break;
    case ROW_PRE_SAVE: {
        double v[PRESET_FIELDS];
        preset_current(kind, v);
        if (kind == PRESET_BRUSH && v[PB_OUTER] != v[PB_OUTER]) {
            why = "This game version's brush size isn't known to the mod.";
            break;
        }
        why = preset_add(kind, v);
        if (!why) {
            g_pre_choice[kind] = preset_count(kind) - 1;
            say("Preset saved. Rename Preset gives it your own name.");
        }
        break;
    }
    case ROW_PRE_RENAME:
        if (!preset_get(kind, g_pre_choice[kind]))
            break;
        snprintf(g_typed, sizeof g_typed, "%s", preset_get(kind, g_pre_choice[kind])->name);
        g_typing = TYPING_PRESET, g_typing_kind = kind;
        g_pre_open[kind] = 0;
        say("Type a name: Enter keeps it, Esc cancels.");
        break;
    case ROW_PRE_KEEP: keep_typed(); break;
    case ROW_PRE_CANCEL: g_typing = TYPING_NONE; break;
    case ROW_PRE_DELETE: {
        preset keep = preset_get(kind, g_pre_choice[kind]) ? *preset_get(kind, g_pre_choice[kind]) : (preset){0};
        int at = g_pre_choice[kind];
        why = preset_delete(kind, at);
        if (!why) {
            int *nd = &g_pre_deleted_n[kind];
            if (*nd == DELETED_KEEP)
                memmove(&g_pre_deleted[kind][0], &g_pre_deleted[kind][1], (DELETED_KEEP - 1) * sizeof g_pre_deleted[0][0]), (*nd)--;
            g_pre_deleted[kind][*nd].p = keep, g_pre_deleted[kind][(*nd)++].at = at;
            int count = preset_count(kind);
            g_pre_choice[kind] = g_pre_choice[kind] >= count ? (count ? count - 1 : 0) : g_pre_choice[kind];
            say("Preset deleted. Undo Delete Preset puts it back.");
        }
        break;
    }
    case ROW_PRE_UNDO: {
        int *nd = &g_pre_deleted_n[kind];
        if (!*nd)
            break;
        why = preset_insert(kind, g_pre_deleted[kind][*nd - 1].at, &g_pre_deleted[kind][*nd - 1].p);
        if (!why) {
            int at = g_pre_deleted[kind][--(*nd)].at, count = preset_count(kind);
            g_pre_choice[kind] = at < count ? at : count - 1;
            say("Preset put back.");
        }
        break;
    }
    }
    if (why)
        say(why);
}

static panel_row g_all[BUILD_ROWS];
static int build_panel(panel_row *rows, int max);
static int fold_panel(int n, panel_row *out, int max_out);
int curvetool_panel(panel_row *out, int max_out) { return fold_panel(build_panel(g_all, BUILD_ROWS), out, max_out); }

static int build_panel(panel_row *rows, int max)
{
    int n = 0;
    panel_row *r = NULL;
#define ROW(kind_, rowk, label_)                                \
    (n < max && n < BUILD_ROWS ? (memset(r = &rows[n], 0, sizeof *r), g_row_kind[n] = (kind_), r->kind = (rowk), \
                                      r->label = (label_), n++, 1)                                                    \
                                   : (r = NULL, 0))
    ROW(ROW_HEADER, ROWK_TITLE, "Jcraig's Editor Mod " MXMOD_VERSION); /* so a screenshot says which version it was */
    ROW(ROW_HEADER, ROWK_HEADER, "VIEW");
    if (ROW(ROW_GRID, ROWK_CHOICE, "Ground Grid"))
        r->options = GRID_OPTIONS, r->count = 6, r->selected = g_grid;
    if (g_grid > 0 && g_grid_shown_ft > GRID_FT[g_grid] && ROW(ROW_HEADER, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "Zoomed out: lines every %.0f ft", g_grid_shown_ft);
    else if (g_grid > 0 && g_grid_shown_ft == 0 && ROW(ROW_HEADER, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "Zoom in to see the grid");
    ROW(ROW_HEADER, ROWK_HEADER, "CURVES");
    if (ROW(ROW_PATH, ROWK_BUTTON, "Curve Path"))
        r->on = g_mode == CURVE_MODE_PATH;
    if (ROW(ROW_PROFILE, ROWK_BUTTON, "Curve Profile"))
        r->on = g_mode == CURVE_MODE_PROFILE;
#if MXMOD_STAMP
    if (ROW(ROW_STAMP, ROWK_BUTTON, "Rut Stamp"))
        r->on = g_mode == CURVE_MODE_STAMP;
#endif
    if (g_mode == CURVE_MODE_STAMP) {
        int room = max - n;
        if (room > 0)
            n += stamptool_rows(rows + n, room, g_row_kind + n, g_row_arg + n);
    }
    /* For the game's own brush modes (0 Raise .. 8 Gradient Radial Add): the brush's shape. */
    int game_mode = game_editor_mode();
    if (g_mode == CURVE_MODE_OFF && game_mode >= 0 && game_mode <= 8) {
        ROW(ROW_HEADER, ROWK_HEADER, "BRUSH");
        double outer_ft, inner_ft;
        if (brush_feet(&outer_ft, &inner_ft)) {
            /* The size in feet, held across zoom (brush_size_frame); the game's own Radius and Focus still work. */
            if (ROW(ROW_BRUSH_OUTER, ROWK_SLIDER, "Outer (ft)"))
                r->value = outer_ft, r->min = 0.5, r->max = 100, r->step = 0.5;
            if (ROW(ROW_BRUSH_INNER, ROWK_SLIDER, "Inner (ft)"))
                r->value = inner_ft, r->min = 0, r->max = 50, r->step = 0.5;
            if (g_hold && g_brush_capped && ROW(ROW_HEADER, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "Zoom out to reach the full size");
        }
        if (ROW(ROW_BRUSH_SHAPE, ROWK_CHOICE, "Brush Shape"))
            r->options = SHAPE_OPTIONS, r->count = BRUSH_TRIANGLE + 1, r->selected = g_brush_shape;
        if (g_brush_shape != BRUSH_CIRCLE && ROW(ROW_BRUSH_ANGLE, ROWK_SLIDER, "Brush Angle"))
            r->value = g_brush_angle, r->min = 0, r->max = 360, r->step = 5; /* a lopsided triangle needs the full turn */
        if (g_brush_shape == BRUSH_OVAL && ROW(ROW_BRUSH_OVAL, ROWK_SLIDER, "Oval Width"))
            r->value = g_brush_oval, r->min = 0.1, r->max = 1, r->step = 0.05;
        if (g_brush_shape == BRUSH_TRIANGLE) {
            if (ROW(ROW_BRUSH_TRI_WIDTH, ROWK_SLIDER, "Triangle Width"))
                r->value = g_brush_tri_width, r->min = 0.25, r->max = 2.5, r->step = 0.05;
            if (ROW(ROW_BRUSH_TRI_LEAN, ROWK_SLIDER, "Triangle Lean"))
                r->value = g_brush_tri_lean, r->min = -1, r->max = 1, r->step = 0.05;
            if (ROW(ROW_HEADER, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "Lean 1 or -1: right angle");
        }
        if (g_brush_shape != BRUSH_CIRCLE && game_mode == 2 && !(g_build && g_build->smooth_shaped) && ROW(ROW_BRUSH_NOTE, ROWK_LABEL, ""))
            snprintf(r->text, sizeof r->text, "Smooth stays round");
    }
    /* For the game's own gradients (Delete, Export, Saved Gradients, Gradient Length) and the radial sweep: gradtool.c. */
    if (g_mode == CURVE_MODE_OFF) {
        int room = max - n;
        if (room > 0)
            n += gradtool_rows(rows + n, room, g_row_kind + n, g_row_arg + n);
        room = max - n;
        if (room > 0)
            n += decaltool_rows(rows + n, room, g_row_kind + n, g_row_arg + n); /* Decals mode (15) only */
        room = max - n;
        if (room > 0)
            n += statuetool_rows(rows + n, room, g_row_kind + n, g_row_arg + n); /* Statues mode (13) only */
        if (game_mode >= 0 && game_mode <= 8)
            n = preset_rows(PRESET_BRUSH, rows, n, max); /* size, shape and sweep, after the sweep's own rows */
    }
    if (g_mode == CURVE_MODE_PATH)
        ROW(g_drawing ? ROW_FINISH : ROW_NEW, ROWK_BUTTON, g_drawing ? "Finish Curve (Enter)" : "New Curve");
    const curve *c = g_selected >= 0 && g_selected < g_curves.count ? &g_curves.c[g_selected] : NULL;
    if ((g_mode == CURVE_MODE_PATH || g_mode == CURVE_MODE_PROFILE) && c) {
        if (ROW(ROW_LENGTH, ROWK_LABEL, ""))
            snprintf(r->text, sizeof r->text, "Length: %.2f", pen_length(c));
        if (g_mode == CURVE_MODE_PROFILE) {
            /* The game's Gradient Edit rows. */
            const profile_point *point = g_point >= 0 && g_point < c->points ? &c->p[g_point] : NULL;
            if (ROW(ROW_SEGMENT, ROWK_CHOICE, "Segment Type"))
                r->options = SEGMENT_OPTIONS, r->count = 2, r->selected = point ? point->type == 1 : g_pending_type;
            if (ROW(ROW_GRANULARITY, ROWK_SLIDER, "Granularity"))
                r->value = g_granularity, r->min = 0, r->max = 1, r->step = 1.0 / 16;
            ROW(ROW_FROM_TERRAIN, ROWK_BUTTON, "Set From Terrain");
            if (ROW(ROW_TRACE_FIT, ROWK_SLIDER, "Terrain Fit (ft)"))
                r->value = g_trace_fit, r->min = 0.01, r->max = 0.25, r->step = 0.01;
            ROW(ROW_FLATTEN, ROWK_BUTTON, "Flatten");
            if (point) {
                if (ROW(ROW_SLOPE, ROWK_LABEL, ""))
                    snprintf(r->text, sizeof r->text, "Slope: %.3f", profile_slope_readout(c, g_point));
                if (ROW(ROW_HEIGHT, ROWK_LABEL, ""))
                    snprintf(r->text, sizeof r->text, "Height: %.2f", point->h);
            }
        }
        if (g_mode == CURVE_MODE_PATH && ROW(ROW_RESIZE, ROWK_CHOICE, "Path Change"))
            r->options = RESIZE_OPTIONS, r->count = 2, r->selected = g_stretch;
        if (ROW(ROW_INNER, ROWK_SLIDER, "Inner"))
            r->value = g_inner, r->min = 0, r->max = 50, r->step = 0.5;
        if (ROW(ROW_OUTER, ROWK_SLIDER, "Outer"))
            r->value = g_outer, r->min = 0.5, r->max = 100, r->step = 0.5;
        if (ROW(ROW_BANK, ROWK_SLIDER, "Bank"))
            r->value = g_bank, r->min = -BANK_MAX, r->max = BANK_MAX, r->step = 0.01;
        if (ROW(ROW_BAKE_SIDE, ROWK_CHOICE, "Bake Side"))
            r->options = BAKE_SIDE_OPTIONS, r->count = 3, r->selected = g_bake_side;
        if (g_bake_side != CURVE_SIDE_BOTH && ROW(ROW_HEADER, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "%s of the line, from the curve's start",
                     g_bake_side == CURVE_SIDE_LEFT ? "Left" : "Right");
        if (ROW(ROW_EASE_PLAIN, ROWK_CHOICE, "Ease Plain Points"))
            r->options = EASE_OPTIONS, r->count = 2, r->selected = g_ease_plain;
        if (ROW(ROW_STRENGTH, ROWK_SLIDER, "Strength"))
            r->value = g_strength, r->min = 0, r->max = 1, r->step = 0.05;
        if (ROW(ROW_BAKE_AS, ROWK_CHOICE, "Bake As"))
            r->options = BAKE_OPTIONS, r->count = 2, r->selected = g_add;
        if (ROW(ROW_SUBMODE, ROWK_CHOICE, "SubMode"))
            r->options = SUBMODE_OPTIONS, r->count = 3, r->selected = g_limit;
        n = preset_rows(PRESET_CURVE, rows, n, max); /* Inner, Outer, Bank, Strength, Bake As, SubMode */
        if (ROW(ROW_LIVE, ROWK_BUTTON, g_live.on ? "Live Preview: On" : "Live Preview: Off"))
            r->on = g_live.on;
        ROW(ROW_APPLY, ROWK_BUTTON, "Apply To Ground");
        if (g_mode == CURVE_MODE_PATH) {
            if (ROW(ROW_HEADER, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "Shift+drag a curve to move all of it");
            ROW(ROW_DUPLICATE, ROWK_BUTTON, "Duplicate Curve");
            ROW(ROW_DELETE, ROWK_BUTTON, "Delete Curve");
        }
        if (ROW(ROW_TURN, ROWK_LABEL, "")) {
            double turn = tightest_turn(c);
            if (turn < 0)
                snprintf(r->text, sizeof r->text, "Straight");
            else if (turn < g_outer)
                snprintf(r->text, sizeof r->text, "Tightest turn %.1f: sides will fold", turn);
            else
                snprintf(r->text, sizeof r->text, "Tightest turn: %.1f", turn);
        }
    }
    /* The section library, on the profile being edited (the game's Gradient Edit, or our Curve Profile). Typing a name
       only lasts while its row is there, so the keyboard always comes back. */
    int ctx = sec_context();
    if ((ctx == SEC_NONE || !library_count()) && g_typing == TYPING_SECTION)
        g_typing = TYPING_NONE;
    if (ctx == SEC_NONE || !library_count())
        g_sec_list_open = 0;
    if (ctx != SEC_NONE) {
        static profile_point p[CURVE_MAX_POINTS];
        int count, room, picked, owner;
        if (sec_profile(ctx, p, &count, &room, &picked, &owner)) {
            sec_follow(ctx, owner);
            if (g_sec_start >= count || g_sec_end >= count)
                g_sec_start = g_sec_end = -1; /* the profile lost points since */
            /* The game's own Set From Terrain button rounds off sharp edges; this one keeps them. */
            if (ctx == SEC_GAME) {
                ROW(ROW_HEADER, ROWK_HEADER, "TRACE THE GROUND");
                ROW(ROW_TRACE_GAME, ROWK_BUTTON, "Set From Terrain (Sharp)");
                if (ROW(ROW_TRACE_FIT, ROWK_SLIDER, "Terrain Fit (ft)"))
                    r->value = g_trace_fit, r->min = 0.01, r->max = 0.25, r->step = 0.01;
            }
            /* Moving several points at once: the range is drawn on the graph (range_scene). */
            ROW(ROW_HEADER, ROWK_HEADER, "MOVE SEVERAL POINTS");
            if (!rng_valid(ctx, owner, count)) {
                if (ROW(ROW_HEADER, ROWK_HINT, ""))
                    snprintf(r->text, sizeof r->text, "Shift+click first point, then last");
            } else if (rng_first() == rng_last()) {
                if (ROW(ROW_HEADER, ROWK_HINT, ""))
                    snprintf(r->text, sizeof r->text, "Now Shift+click the last point");
            } else {
                if (ROW(ROW_HEADER, ROWK_HINT, ""))
                    snprintf(r->text, sizeof r->text, "Points %d-%d: drag one to move all", rng_first() + 1, rng_last() + 1);
            }
            if (g_rng_a >= 0)
                ROW(ROW_RNG_CLEAR, ROWK_BUTTON, "Clear Selection (Esc)");
            /* Saving: three steps, each spelled out in a hint, with the marks drawn on the graph (sec_scene). */
            ROW(ROW_HEADER, ROWK_HEADER, "SAVE A PROFILE SECTION");
            if (g_sec_start < 0 && ROW(ROW_HEADER, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "Keep a piece (a jump...) to reuse");
            if (g_sec_start < 0) {
                if (ROW(ROW_HEADER, ROWK_HINT, ""))
                    snprintf(r->text, sizeof r->text, "1. Click where it starts on the graph");
                ROW(ROW_SEC_START, ROWK_BUTTON, "Use Point As Section Start");
            } else {
                snprintf(g_sec_start_label, sizeof g_sec_start_label, "Start: point %d (change)", g_sec_start + 1);
                if (ROW(ROW_SEC_START, ROWK_BUTTON, g_sec_start_label))
                    r->on = 1;
                if (g_sec_end < 0) {
                    if (ROW(ROW_HEADER, ROWK_HINT, ""))
                        snprintf(r->text, sizeof r->text, "2. Click where it ends (further on)");
                    ROW(ROW_SEC_END, ROWK_BUTTON, "Use Point As Section End");
                } else {
                    snprintf(g_sec_end_label, sizeof g_sec_end_label, "End: point %d (change)", g_sec_end + 1);
                    if (ROW(ROW_SEC_END, ROWK_BUTTON, g_sec_end_label))
                        r->on = 1;
                    if (ROW(ROW_HEADER, ROWK_HINT, ""))
                        snprintf(r->text, sizeof r->text, "3. Save it to your sections");
                    ROW(ROW_SEC_SAVE, ROWK_BUTTON, "Save Section");
                }
                ROW(ROW_SEC_CLEAR, ROWK_BUTTON, "Clear Start And End");
            }
            /* Using one: the chosen section is a dropdown (v closed, ^ open); paste puts it on the picked point. */
            ROW(ROW_HEADER, ROWK_HEADER, "YOUR SECTIONS");
            if (library_count()) {
                g_sec_choice = g_sec_choice < library_count() ? g_sec_choice : 0;
                const char *const *names = library_names();
                static char choice_label[96], item_label[SECTION_MAX][64];
                snprintf(choice_label, sizeof choice_label, "%s  %s", names[g_sec_choice], g_sec_list_open ? "^" : "v");
                if (ROW(ROW_SEC_CHOICE, ROWK_BUTTON, choice_label))
                    r->on = g_sec_list_open;
                if (g_sec_list_open) {
                    int sections = library_count(), first, last;
                    panel_list_range(PANEL_LIST_SECTIONS, sections, &first, &last);
                    if (first > 0 && ROW(PANEL_LIST_UP, ROWK_BUTTON, panel_list_more(PANEL_LIST_SECTIONS, 1, first)))
                        r->list = PANEL_LIST_SECTIONS;
                    for (int i = first; i < last; i++) {
                        snprintf(item_label[i], sizeof item_label[i], "%s %s", i == g_sec_choice ? ">" : "  ", names[i]);
                        if (ROW(ROW_SEC_ITEM, ROWK_BUTTON, item_label[i])) {
                            r->on = i == g_sec_choice;
                            r->list = PANEL_LIST_SECTIONS;
                            g_row_arg[n - 1] = i;
                        }
                    }
                    if (last < sections && ROW(PANEL_LIST_DOWN, ROWK_BUTTON, panel_list_more(PANEL_LIST_SECTIONS, 0, sections - last)))
                        r->list = PANEL_LIST_SECTIONS;
                    if (ROW(ROW_HEADER, ROWK_HINT, ""))
                        snprintf(r->text, sizeof r->text, sections > PANEL_LIST_SHOW ? "Click one to use it; the wheel scrolls" : "Click one to use it");
                } else if (library_count() > 1 && ROW(ROW_HEADER, ROWK_HINT, "")) {
                    snprintf(r->text, sizeof r->text, "Click the name to pick another");
                }
                if (picked < 0 || picked >= count) {
                    if (ROW(ROW_HEADER, ROWK_HINT, ""))
                        snprintf(r->text, sizeof r->text, "Click the point to paste it at");
                }
                ROW(ROW_SEC_PASTE, ROWK_BUTTON, "Paste At Picked Point");
                if (g_typing == TYPING_SECTION) {
                    snprintf(g_typed_label, sizeof g_typed_label, "Name: %s_", g_typed);
                    ROW(ROW_SEC_TYPED, ROWK_BUTTON, g_typed_label);
                    ROW(ROW_SEC_KEEP, ROWK_BUTTON, "Keep Name (Enter)");
                    ROW(ROW_SEC_CANCEL, ROWK_BUTTON, "Cancel (Esc)");
                } else {
                    ROW(ROW_SEC_RENAME, ROWK_BUTTON, "Rename Section");
                }
                ROW(ROW_SEC_DELETE, ROWK_BUTTON, "Delete Section");
            } else if (ROW(ROW_SEC_NONE, ROWK_HINT, "")) {
                snprintf(r->text, sizeof r->text, "None saved yet: see the steps above");
            }
            if (g_sec_deleted_n) {
                static char undo_label[64];
                snprintf(undo_label, sizeof undo_label, "Undo Delete Section (%d)", g_sec_deleted_n);
                ROW(ROW_SEC_UNDO, ROWK_BUTTON, undo_label);
            }
        }
    }
    if (g_mode == CURVE_MODE_PATH || g_mode == CURVE_MODE_PROFILE || g_dirty)
        ROW(ROW_SAVE, ROWK_BUTTON, g_dirty ? "Save Curves*" : "Save Curves");
    if ((g_mode == CURVE_MODE_PATH || g_mode == CURVE_MODE_PROFILE) && (g_undo_n || g_redo_n)) {
        static char undo_label[48], redo_label[48];
        snprintf(undo_label, sizeof undo_label, "Undo Curve Edit (Ctrl+Z)  %d", g_undo_n);
        snprintf(redo_label, sizeof redo_label, "Redo (Ctrl+Y)  %d", g_redo_n);
        if (g_undo_n)
            ROW(ROW_CURVE_UNDO, ROWK_BUTTON, undo_label);
        if (g_redo_n)
            ROW(ROW_CURVE_REDO, ROWK_BUTTON, redo_label);
    }
#undef ROW
    return n;
}

/* ── folding groups and scrolling lists ── */
enum { MAX_FOLDS = 48, FOLD_KEY = 48 };
static char g_folds[MAX_FOLDS][FOLD_KEY]; /* the folded groups, by header with its numbers taken out */
static int g_fold_count, g_folds_read;
static int g_row_head[MAX_PANEL_ROWS];          /* a shown row is a header a click folds */
static int g_row_list[MAX_PANEL_ROWS];          /* a shown row's scrolling list, 0 = none */
static char g_head_key[MAX_PANEL_ROWS][FOLD_KEY];
static char g_head_label[MAX_PANEL_ROWS][72];
static int g_list_first[PANEL_LISTS];

/* "GRADIENT 12 OF 93" and "GRADIENT 3 OF 93" are the same group: the key drops digits. */
static void fold_key(const char *text, char *key)
{
    int k = 0;
    for (; *text && k < FOLD_KEY - 1; text++)
        if (*text < '0' || *text > '9')
            key[k++] = *text;
    key[k] = 0;
}
static int fold_find(const char *key)
{
    for (int i = 0; i < g_fold_count; i++)
        if (!strcmp(g_folds[i], key))
            return i;
    return -1;
}
static int folds_path(wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH];
    return mx_data_dir(dir, MAX_PATH) && swprintf(out, cap, L"%ls\\panel-folds.txt", dir) > 0;
}
static void folds_read(void)
{
    g_folds_read = 1;
    wchar_t path[MAX_PATH];
    if (!folds_path(path, MAX_PATH))
        return;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    static char text[MAX_FOLDS * (FOLD_KEY + 2)];
    DWORD got = 0;
    ReadFile(f, text, sizeof text - 1, &got, NULL);
    CloseHandle(f);
    text[got] = 0;
    for (char *line = strtok(text, "\r\n"); line && g_fold_count < MAX_FOLDS; line = strtok(NULL, "\r\n"))
        if (line[0] && fold_find(line) < 0)
            snprintf(g_folds[g_fold_count++], FOLD_KEY, "%s", line);
}
static void folds_write(void)
{
    wchar_t path[MAX_PATH];
    if (!folds_path(path, MAX_PATH))
        return;
    static char text[MAX_FOLDS * (FOLD_KEY + 2)];
    int n = 0;
    for (int i = 0; i < g_fold_count; i++)
        n += snprintf(text + n, sizeof text - n, "%s\r\n", g_folds[i]);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD put = 0;
    WriteFile(f, text, (DWORD)n, &put, NULL);
    CloseHandle(f);
}
static void fold_toggle(const char *key)
{
    int i = fold_find(key);
    if (i >= 0) {
        memmove(g_folds[i], g_folds[i + 1], (size_t)(g_fold_count - i - 1) * FOLD_KEY);
        g_fold_count--;
    } else if (g_fold_count < MAX_FOLDS) {
        snprintf(g_folds[g_fold_count++], FOLD_KEY, "%s", key);
    }
    folds_write();
}

/* Takes the folded groups' rows out (a header stays, marked [+]; an open one [-]), packs the kinds and arguments down
   with the rows, and hands over the first max_out. A row past them is cut off as before, but now only the groups the
   user left open count. */
static int fold_panel(int n, panel_row *out, int max_out)
{
    if (!g_folds_read)
        folds_read();
    if (max_out > MAX_PANEL_ROWS)
        max_out = MAX_PANEL_ROWS;
    int shown = 0, folded = 0;
    for (int i = 0; i < n && shown < max_out; i++) {
        panel_row row = g_all[i];
        int head = row.kind == ROWK_HEADER;
        if (head) {
            const char *text = row.label && row.label[0] ? row.label : row.text;
            if (!text[0]) {
                head = 0; /* a blank header (a spacer) folds nothing */
            } else {
                fold_key(text, g_head_key[shown]);
                folded = fold_find(g_head_key[shown]) >= 0;
                snprintf(g_head_label[shown], sizeof g_head_label[shown], "%s %s", folded ? "[+]" : "[-]", text);
                row.label = g_head_label[shown];
            }
        } else if (row.kind == ROWK_TITLE) {
            folded = 0;
        } else if (folded) {
            continue;
        }
        out[shown] = row;
        g_row_kind[shown] = g_row_kind[i], g_row_arg[shown] = g_row_arg[i]; /* shown <= i: packing down is safe */
        g_row_head[shown] = head;
        g_row_list[shown] = row.list;
        shown++;
    }
    return shown;
}

int curvetool_header(int row) { return row >= 0 && row < MAX_PANEL_ROWS && g_row_head[row]; }

void panel_list_range(int list, int count, int *first, int *last)
{
    int *f = list > 0 && list < PANEL_LISTS ? &g_list_first[list] : NULL;
    int top = count > PANEL_LIST_SHOW ? count - PANEL_LIST_SHOW : 0;
    int start = f ? *f : 0;
    start = start < 0 ? 0 : start > top ? top : start;
    if (f)
        *f = start;
    *first = start;
    *last = start + PANEL_LIST_SHOW < count ? start + PANEL_LIST_SHOW : count;
}
void panel_list_show(int list, int index)
{
    if (list <= 0 || list >= PANEL_LISTS)
        return;
    if (index < g_list_first[list])
        g_list_first[list] = index;
    else if (index >= g_list_first[list] + PANEL_LIST_SHOW)
        g_list_first[list] = index - PANEL_LIST_SHOW + 1;
}
const char *panel_list_more(int list, int up, int hidden)
{
    static char label[PANEL_LISTS][2][40];
    int l = list > 0 && list < PANEL_LISTS ? list : 0;
    snprintf(label[l][up != 0], sizeof label[l][0], up ? "^  %d more above" : "v  %d more below", hidden);
    return label[l][up != 0];
}
static void list_scroll(int list, int by)
{
    if (list > 0 && list < PANEL_LISTS)
        g_list_first[list] += by; /* panel_list_range clamps it on the next build */
}
int curvetool_wheel(int row, int notches)
{
    if (row < 0 || row >= MAX_PANEL_ROWS || !g_row_list[row] || !notches)
        return 0;
    list_scroll(g_row_list[row], -notches); /* the wheel away from you goes up the list */
    return 1;
}

void curvetool_slide(int row, double value)
{
    if (row < 0 || row >= MAX_PANEL_ROWS)
        return;
    if (g_row_kind[row] >= STATUETOOL_ROW) {
        statuetool_slide(g_row_kind[row], value);
        return;
    }
    if (g_row_kind[row] >= DECALTOOL_ROW)
        return; /* the decal rows have no sliders */
    if (g_row_kind[row] >= STAMPTOOL_ROW) {
        stamptool_slide(g_row_kind[row], value);
        return;
    }
    if (g_row_kind[row] >= GRADTOOL_ROW) {
        gradtool_slide(g_row_kind[row], value);
        return;
    }
    switch (g_row_kind[row]) {
    case ROW_INNER:
        g_inner = value;
        if (g_outer < g_inner)
            g_outer = g_inner; /* inner can't pass outer */
        break;
    case ROW_OUTER:
        g_outer = value;
        if (g_inner > g_outer)
            g_inner = g_outer;
        break;
    case ROW_BANK: g_bank = value; break;
    case ROW_STRENGTH: g_strength = value; break;
    case ROW_GRANULARITY: g_granularity = value; break;
    case ROW_TRACE_FIT: g_trace_fit = value; break;
    case ROW_BRUSH_ANGLE: g_brush_angle = value; break;
    case ROW_BRUSH_OUTER:
    case ROW_BRUSH_INNER: {
        double outer, inner;
        if (!brush_feet(&outer, &inner))
            break;
        if (g_row_kind[row] == ROW_BRUSH_OUTER)
            hold_brush(value, inner > value ? value : inner);
        else
            hold_brush(value > outer ? value : outer, value); /* inner can't pass outer: outer grows with it */
        break;
    }
    case ROW_BRUSH_OVAL: g_brush_oval = value; break;
    case ROW_BRUSH_TRI_WIDTH: g_brush_tri_width = value; break;
    case ROW_BRUSH_TRI_LEAN: g_brush_tri_lean = value; break;
    }
}

/* Duplicate Curve: a copy 10 ft to the side, as the game's Copy Gradient offsets its copy (10 * (dz, -dx) / |AB|, with
   AB the curve's first segment here). The copy is the one selected, ready to drag into place. */
static void duplicate_curve(int i)
{
    if (i < 0 || i >= g_curves.count)
        return;
    if (g_curves.count >= CURVE_MAX) {
        say("There's no room for another curve (64 at most).");
        return;
    }
    set_live(0);
    curve *src = &g_curves.c[i], *dst = &g_curves.c[g_curves.count];
    *dst = *src;
    double dx = src->anchors > 1 ? src->a[1].x - src->a[0].x : 1, dz = src->anchors > 1 ? src->a[1].z - src->a[0].z : 0;
    double len = hypot(dx, dz);
    double ox = len > 1e-9 ? 10 * dz / len : 10, oz = len > 1e-9 ? -10 * dx / len : 0;
    for (int k = 0; k < dst->anchors; k++) {
        pen_anchor *a = &dst->a[k];
        a->x += ox, a->ix += ox, a->ox += ox;
        a->z += oz, a->iz += oz, a->oz += oz;
    }
    g_selected = g_curves.count++;
    g_drawing = 0;
    g_point = -1;
    g_dirty = 1;
    status_now();
    say("Duplicated: the copy is 10 ft to the side and selected.");
}

/* Set From Terrain (Sharp) on the game's selected gradient: the same trace, along its straight line from A to B. */
static void game_gradient_terrain(void)
{
    static gradient_copy g;
    static curve line;
    int owner = game_selected_gradient();
    if (owner < 0 || !game_gradient_copy(owner, &g)) {
        say("Pick a gradient first.");
        return;
    }
    live_cancel();
    if (!bermpreview_lift()) {
        say("Berm Preview couldn't come off the ground. Try again.");
        return;
    }
    line.closed = 0, line.anchors = 2;
    line.a[0] = pen_new_anchor(g.ax, g.az), line.a[1] = pen_new_anchor(g.bx, g.bz);
    game_map m;
    trace_result r;
    if (!game_heights(&m) || !profile_trace_ground(&line, m.heights, m.width, m.cell_ft, g_trace_fit, &r)) {
        say("Couldn't read the ground along this gradient.");
        return;
    }
    const char *why = game_set_profile(owner, line.p, line.points, -1);
    if (why) {
        say(why);
        return;
    }
    g_sec_start = g_sec_end = -1;
    g_rng_a = g_rng_b = -1;
    say_traced(line.points, &r);
}

/* ── undo for curve edits (Ctrl+Z, Ctrl+Y redo) ──
   Each action (a press and drag on the map, a panel button, a key) takes a compact copy of every curve first: the
   count, the selection, then each curve's header and only the points in use. When the action ends and the curves
   differ from that copy (more than the selection), it goes on the undo list. Undo swaps the copy back in, and the
   curves as they were go on redo. Cleared when the curves file is read again (another track, or the app rewrote it). */
enum { CURVE_UNDO_KEEP = 64 };
typedef struct {
    unsigned char *bytes;
    size_t len;
} curve_snap;
static curve_snap g_undo[CURVE_UNDO_KEEP], g_redo[CURVE_UNDO_KEEP];
static int g_undo_n, g_redo_n;
static curve_snap g_pending; /* taken when the outermost action began */
static int g_act_depth;

static curve_snap snap_take(void)
{
    curve_snap s = {NULL, 0};
    size_t len = 2 * sizeof(int);
    for (int i = 0; i < g_curves.count; i++)
        len += 3 * sizeof(int) + (size_t)g_curves.c[i].anchors * sizeof(pen_anchor) +
               (size_t)g_curves.c[i].points * sizeof(profile_point);
    unsigned char *b = malloc(len), *at = b;
    if (!b)
        return s;
#define PUT(src, n) (memcpy(at, (src), (n)), at += (n))
    PUT(&g_curves.count, sizeof(int));
    PUT(&g_selected, sizeof(int));
    for (int i = 0; i < g_curves.count; i++) {
        const curve *c = &g_curves.c[i];
        PUT(&c->closed, sizeof(int));
        PUT(&c->anchors, sizeof(int));
        PUT(&c->points, sizeof(int));
        PUT(c->a, (size_t)c->anchors * sizeof(pen_anchor));
        PUT(c->p, (size_t)c->points * sizeof(profile_point));
    }
#undef PUT
    s.bytes = b, s.len = len;
    return s;
}
static int snap_put(const curve_snap *s)
{
    const unsigned char *at = s->bytes, *end = s->bytes + s->len;
    int count = 0, selected = -1;
#define GET(dst, n) (at + (n) <= end ? (memcpy((dst), at, (n)), at += (n), 1) : 0)
    if (!GET(&count, sizeof(int)) || !GET(&selected, sizeof(int)) || count < 0 || count > CURVE_MAX)
        return 0;
    for (int i = 0; i < count; i++) {
        curve *c = &g_curves.c[i];
        if (!GET(&c->closed, sizeof(int)) || !GET(&c->anchors, sizeof(int)) || !GET(&c->points, sizeof(int)) ||
            c->anchors < 0 || c->anchors > CURVE_MAX_ANCHORS || c->points < 0 || c->points > CURVE_MAX_POINTS ||
            !GET(c->a, (size_t)c->anchors * sizeof(pen_anchor)) || !GET(c->p, (size_t)c->points * sizeof(profile_point)))
            return 0;
    }
#undef GET
    g_curves.count = count;
    g_curves_gen++;
    g_selected = selected >= 0 && selected < count ? selected : -1;
    return 1;
}
static void snap_free(curve_snap *s)
{
    free(s->bytes);
    s->bytes = NULL, s->len = 0;
}
static void snap_push(curve_snap *list, int *n, curve_snap s)
{
    if (*n == CURVE_UNDO_KEEP) {
        snap_free(&list[0]);
        memmove(&list[0], &list[1], (CURVE_UNDO_KEEP - 1) * sizeof list[0]);
        (*n)--;
    }
    list[(*n)++] = s;
}
static void curve_undo_clear(void)
{
    while (g_undo_n)
        snap_free(&g_undo[--g_undo_n]);
    while (g_redo_n)
        snap_free(&g_redo[--g_redo_n]);
    snap_free(&g_pending);
    g_act_depth = g_mouse_act = 0;
}
static void act_begin(void)
{
    if (g_act_depth++ == 0) {
        snap_free(&g_pending);
        g_pending = snap_take();
    }
}
/* The same curves, maybe another one selected: a click that only picks a curve isn't an edit. */
static int same_curves(const curve_snap *a, const curve_snap *b)
{
    size_t skip = 2 * sizeof(int);
    return a->bytes && b->bytes && a->len == b->len && a->len >= skip && !memcmp(a->bytes, b->bytes, sizeof(int)) &&
           !memcmp(a->bytes + skip, b->bytes + skip, a->len - skip);
}
static void act_end(void)
{
    if (g_act_depth <= 0 || --g_act_depth > 0 || !g_pending.bytes)
        return;
    curve_snap now = snap_take();
    int same = same_curves(&now, &g_pending);
    snap_free(&now);
    if (same) {
        snap_free(&g_pending);
        return;
    }
    snap_push(g_undo, &g_undo_n, g_pending);
    g_pending.bytes = NULL, g_pending.len = 0;
    while (g_redo_n)
        snap_free(&g_redo[--g_redo_n]);
}
static void curve_undo(int redo)
{
    curve_snap *from = redo ? g_redo : g_undo, *to = redo ? g_undo : g_redo;
    int *from_n = redo ? &g_redo_n : &g_undo_n, *to_n = redo ? &g_undo_n : &g_redo_n;
    if (!*from_n) {
        say(redo ? "Nothing to redo." : "No curve edit to undo.");
        return;
    }
    curve_snap now = snap_take();
    curve_snap back = from[--(*from_n)];
    if (!now.bytes || !snap_put(&back)) {
        from[(*from_n)++] = back; /* kept, nothing changed */
        snap_free(&now);
        say("Couldn't undo that.");
        return;
    }
    snap_free(&back);
    snap_push(to, to_n, now);
    g_dirty = 1;
    g_drawing = 0, g_point = -1, g_drag.kind = DRAG_NONE;
    g_rng_a = g_rng_b = -1, g_sec_start = g_sec_end = -1;
    say(redo ? "Redone. Ctrl+Z undoes it again." : "Undone. Ctrl+Y redoes it.");
}

/* ── curves in the game's gradient group (gradtool.c) ──
   Gradient Placement's group can hold curves as well as the game's gradients, so they move as one. A curve there is
   its index, good while curvetool_curves_gen() stays the same. A group move is one curve undo step too (begin/end), so
   Ctrl+Z in Curve Path doesn't skip back past it. */
int curvetool_curves_gen(void) { return g_curves_gen; }
int curvetool_curve_count(void) { return g_curves.count; }

static double g_line_pts[CURVE_MAX_ANCHORS * 17][2];

int curvetool_curve_line(int i, double (*out)[2], int max)
{
    if (i < 0 || i >= g_curves.count || g_curves.c[i].anchors < 2)
        return 0;
    return pen_points(&g_curves.c[i], 16, out, max);
}

/* The curve whose line is nearest (x, z) within reach ft, -1 if none; *dist how far. */
int curvetool_curve_near(double x, double z, double reach, double *dist)
{
    int best = -1;
    double best_d = reach;
    for (int i = 0; i < g_curves.count; i++) {
        int n = curvetool_curve_line(i, g_line_pts, CURVE_MAX_ANCHORS * 17);
        for (int k = 0; k + 1 < n; k++) {
            double ax = g_line_pts[k][0], az = g_line_pts[k][1], dx = g_line_pts[k + 1][0] - ax, dz = g_line_pts[k + 1][1] - az;
            double run = dx * dx + dz * dz, t = run > 0 ? ((x - ax) * dx + (z - az) * dz) / run : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            double d = hypot(x - (ax + t * dx), z - (az + t * dz));
            if (d <= best_d)
                best = i, best_d = d;
        }
    }
    if (dist)
        *dist = best_d;
    return best;
}

/* Halfway along the curve, for the group's box. */
int curvetool_curve_middle(int i, double *x, double *z)
{
    if (i < 0 || i >= g_curves.count || g_curves.c[i].anchors < 2)
        return 0;
    static const double half = 0.5;
    double at[1][2];
    if (!pen_spots(&g_curves.c[i], &half, 1, 1.0, at))
        return 0;
    *x = at[0][0], *z = at[0][1];
    return 1;
}

/* Its first point, which a move shifts like every other: how the group's undo knows the curve is still where it left it. */
int curvetool_curve_first(int i, double *x, double *z)
{
    if (i < 0 || i >= g_curves.count || g_curves.c[i].anchors < 1)
        return 0;
    *x = g_curves.c[i].a[0].x, *z = g_curves.c[i].a[0].z;
    return 1;
}

/* Every point and handle moved by (dx, dz); the profile stays. Like the game's gradients, the ground doesn't change. */
void curvetool_curve_shift(int i, double dx, double dz)
{
    if (i < 0 || i >= g_curves.count || (dx == 0 && dz == 0))
        return;
    curve *c = &g_curves.c[i];
    for (int k = 0; k < c->anchors; k++)
        pen_move_anchor(&c->a[k], c->a[k].x + dx, c->a[k].z + dz);
    g_dirty = 1;
    status_now();
}

void curvetool_group_move_begin(void) { act_begin(); }
void curvetool_group_move_end(void) { act_end(); }

static void press_inner(int row);
void curvetool_press(int row)
{
    if (row >= 0 && row < MAX_PANEL_ROWS && (g_row_kind[row] == ROW_CURVE_UNDO || g_row_kind[row] == ROW_CURVE_REDO)) {
        curve_undo(g_row_kind[row] == ROW_CURVE_REDO);
        return;
    }
    act_begin();
    press_inner(row);
    act_end();
}

static void press_inner(int row)
{
    if (row < 0 || row >= MAX_PANEL_ROWS)
        return;
    if (g_row_head[row]) {
        fold_toggle(g_head_key[row]);
        return;
    }
    if (g_row_kind[row] == PANEL_LIST_UP || g_row_kind[row] == PANEL_LIST_DOWN) {
        list_scroll(g_row_list[row], g_row_kind[row] == PANEL_LIST_UP ? -(PANEL_LIST_SHOW / 2) : PANEL_LIST_SHOW / 2);
        return;
    }
    if (g_row_kind[row] >= STATUETOOL_ROW) {
        statuetool_press(g_row_kind[row], g_row_arg[row]);
        return;
    }
    if (g_row_kind[row] >= DECALTOOL_ROW) {
        decaltool_press(g_row_kind[row], g_row_arg[row]);
        return;
    }
    if (g_row_kind[row] >= STAMPTOOL_ROW) {
        stamptool_press(g_row_kind[row], g_row_arg[row]);
        return;
    }
    if (g_row_kind[row] >= GRADTOOL_ROW) {
        gradtool_press(g_row_kind[row], g_row_arg[row]);
        return;
    }
    switch (g_row_kind[row]) {
    case ROW_BRUSH_SHAPE: g_brush_shape = (g_brush_shape + 1) % (BRUSH_TRIANGLE + 1); break;
    case ROW_GRID: g_grid = (g_grid + 1) % 6; break;
    case ROW_SEC_CHOICE:
        g_sec_list_open = !g_sec_list_open;
        if (g_sec_list_open)
            panel_list_show(PANEL_LIST_SECTIONS, g_sec_choice);
        break;
    case ROW_SEC_ITEM:
        g_sec_choice = g_row_arg[row];
        g_sec_list_open = 0;
        g_typing = 0;
        break;
    case ROW_SEC_RENAME:
        snprintf(g_typed, sizeof g_typed, "%s", library_get(g_sec_choice) ? library_get(g_sec_choice)->name : "");
        g_typing = TYPING_SECTION;
        g_sec_list_open = 0;
        say("Type a name: Enter keeps it, Esc cancels.");
        break;
    case ROW_SEC_KEEP: keep_typed(); break;
    case ROW_SEC_UNDO:
        if (g_sec_deleted_n) {
            const char *why = library_insert(g_sec_deleted[g_sec_deleted_n - 1].at, &g_sec_deleted[g_sec_deleted_n - 1].s);
            if (why) {
                say(why);
            } else {
                g_sec_choice = g_sec_deleted[--g_sec_deleted_n].at;
                g_sec_choice = g_sec_choice < library_count() ? g_sec_choice : library_count() - 1;
                say("Section put back.");
            }
        }
        break;
    case ROW_SEC_CANCEL: g_typing = 0; break;
    case ROW_SEC_CLEAR: g_sec_start = g_sec_end = -1; break;
    case ROW_SEC_START:
    case ROW_SEC_END:
    case ROW_SEC_SAVE:
    case ROW_SEC_PASTE:
    case ROW_SEC_DELETE:
        g_typing = 0;
        sec_press(g_row_kind[row]);
        break;
    case ROW_RESIZE: g_stretch = !g_stretch; break;
    case ROW_BAKE_AS: g_add = !g_add; break;
    case ROW_SUBMODE: g_limit = (g_limit + 1) % 3; break;
    case ROW_BAKE_SIDE: g_bake_side = (g_bake_side + 1) % 3; break;
    case ROW_EASE_PLAIN:
        g_ease_plain = !g_ease_plain;
        curve_ease_plain(g_ease_plain);
        break;
    case ROW_SEGMENT:
        /* Writes to the picked point (the segment from it to the next one); with none picked only the widget changes. */
        if (g_selected >= 0 && g_point >= 0 && g_point < g_curves.c[g_selected].points) {
            profile_point *p = &g_curves.c[g_selected].p[g_point];
            p->type = p->type == 1 ? 0 : 1;
            g_dirty = 1;
            status_now();
        } else {
            g_pending_type = !g_pending_type;
        }
        break;
    case ROW_FROM_TERRAIN:
        if (g_selected >= 0)
            profile_terrain(&g_curves.c[g_selected]);
        break;
    case ROW_TRACE_GAME: game_gradient_terrain(); break;
    case ROW_FLATTEN:
        if (g_selected >= 0)
            profile_flatten(&g_curves.c[g_selected]);
        break;
    case ROW_PATH:
    case ROW_PROFILE: {
        int mode = g_row_kind[row] == ROW_PATH ? CURVE_MODE_PATH : CURVE_MODE_PROFILE;
        curvetool_set_mode(g_mode == mode ? CURVE_MODE_OFF : mode); /* a second click hands the map back to the game */
        break;
    }
    case ROW_STAMP: curvetool_set_mode(g_mode == CURVE_MODE_STAMP ? CURVE_MODE_OFF : CURVE_MODE_STAMP); break;
    case ROW_FINISH: finish_curve(); break;
    case ROW_NEW: g_selected = -1, g_drawing = 0; break;
    case ROW_DELETE: delete_curve(g_selected); break;
    case ROW_DUPLICATE: duplicate_curve(g_selected); break;
    case ROW_PRE_CHOICE:
    case ROW_PRE_ITEM:
    case ROW_PRE_SAVE:
    case ROW_PRE_RENAME:
    case ROW_PRE_KEEP:
    case ROW_PRE_CANCEL:
    case ROW_PRE_DELETE:
    case ROW_PRE_UNDO:
        if (g_row_kind[row] != ROW_PRE_RENAME && g_row_kind[row] != ROW_PRE_KEEP)
            g_typing = g_typing == TYPING_PRESET ? TYPING_NONE : g_typing;
        preset_press(g_row_kind[row], g_row_arg[row]);
        break;
    case ROW_RNG_CLEAR: g_rng_a = g_rng_b = -1; break;
    case ROW_SAVE: curvetool_save(); break;
    case ROW_LIVE: set_live(!g_live.on); break;
    case ROW_APPLY: apply_selected(); break;
    }
}

/* ── drawing, the way the app paints them (web/curve-tool.js paintCurve) ── */

static const float C_PATH[4] = {0.498f, 0.890f, 1.0f, 1.0f};       /* #7fe3ff */
static const float C_FAINT[4] = {0.498f, 0.890f, 1.0f, 0.75f};
static const float C_HANDLE[4] = {0.561f, 0.816f, 1.0f, 1.0f};     /* #8fd0ff */
static const float C_ANCHOR[4] = {1, 1, 1, 1};
static const float C_FIRST[4] = {0.490f, 1.0f, 0.541f, 1.0f};      /* #7dff8a */
static const float C_INNER[4] = {1, 1, 1, 0.7f};
static const float C_OUTER[4] = {1, 1, 1, 0.3f};
static const float C_STRETCH[4] = {0.498f, 0.890f, 1.0f, 0.6f};
static const float C_MARKER[4] = {1.0f, 0.824f, 0.290f, 1.0f};      /* #ffd24a */

static scene_item *add_item(scene *s, int kind, const float rgba[4])
{
    if (s->items >= SCENE_MAX_ITEMS)
        return NULL;
    scene_item *it = &s->item[s->items++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    memcpy(it->rgba, rgba, sizeof it->rgba);
    it->size = 1;
    it->first = s->points;
    return it;
}

static void add_line(scene *s, const double (*pts)[2], int n, const float rgba[4])
{
    if (n < 2 || s->points + n > SCENE_MAX_POINTS)
        return;
    scene_item *it = add_item(s, SCENE_LINE, rgba);
    if (!it)
        return;
    for (int i = 0; i < n; i++)
        s->pt[s->points + i][0] = (float)pts[i][0], s->pt[s->points + i][1] = (float)pts[i][1];
    s->points += n;
    it->count = n;
}

static void add_dot(scene *s, double x, double z, float size, const float rgba[4])
{
    scene_item *it = add_item(s, SCENE_DOT, rgba);
    if (it)
        it->x = (float)x, it->z = (float)z, it->size = size;
}

static scene_item *add_points(scene *s, int kind, const double (*pts)[2], int n, const float rgba[4])
{
    if (s->points + n > SCENE_MAX_POINTS)
        return NULL;
    scene_item *it = add_item(s, kind, rgba);
    if (!it)
        return NULL;
    for (int i = 0; i < n; i++)
        s->pt[s->points + i][0] = (float)pts[i][0], s->pt[s->points + i][1] = (float)pts[i][1];
    s->points += n;
    it->count = n;
    return it;
}


/* ── the ground grid ──
   Faint lines every Grid ft over the track (0 .. its size), under everything else we draw, kept off the game's left
   panel. Every 100 ft (or every 5th line, for 50 and 100 ft grids) is brighter. Zoomed out so far that lines would be
   under 8 px apart, it draws every 2nd, 5th or 10th line and the panel says so. */

static void grid_scene(scene *s, const curve_view *v)
{
    g_grid_shown_ft = 0;
    if (g_grid <= 0 || g_grid >= (int)(sizeof GRID_FT / sizeof GRID_FT[0]) || v->h <= 0 || !(v->view_ft > 0))
        return;
    game_map m;
    if (!game_heights(&m) || m.width < 2 || !(m.cell_ft > 0))
        return;
    double size = (m.width - 1) * (double)m.cell_ft, px_per_ft = v->h / v->view_ft;
    double step = GRID_FT[g_grid];
    static const int MORE[] = {1, 2, 5, 10, 20, 50, 100};
    int k = 0;
    while (k < 6 && step * MORE[k] * px_per_ft < 8)
        k++;
    step *= MORE[k];
    if (step * px_per_ft < 8)
        return;
    double major = step >= 50 ? step * 5 : 100;
    /* What's on screen, in feet: from the game's panel edge (21.25 units) to the right edge, top to bottom. */
    double left = (21.25 * v->h / 64.0 - v->w / 2.0) / v->h * v->view_ft + v->cx;
    double right = (v->w / 2.0) / v->h * v->view_ft + v->cx;
    double top = -0.5 * v->view_ft + v->cz, bottom = 0.5 * v->view_ft + v->cz;
    double x0 = left > 0 ? left : 0, x1 = right < size ? right : size, z0 = top > 0 ? top : 0, z1 = bottom < size ? bottom : size;
    if (x1 <= x0 || z1 <= z0)
        return;
    enum { MAX_LINES = 1200 };
    static double minor_pts[MAX_LINES * 2][2], major_pts[MAX_LINES * 2][2];
    int nmin = 0, nmaj = 0;
    for (int axis = 0; axis < 2; axis++) {
        double lo = axis ? z0 : x0, hi = axis ? z1 : x1;
        for (double t = ceil(lo / step) * step; t <= hi + 1e-9; t += step) {
            int is_major = fabs(t / major - floor(t / major + 0.5)) < 1e-6;
            double (*pts)[2] = is_major ? major_pts : minor_pts;
            int *n = is_major ? &nmaj : &nmin;
            if (*n + 2 > MAX_LINES * 2)
                continue;
            if (axis == 0)
                pts[*n][0] = t, pts[*n][1] = z0, pts[*n + 1][0] = t, pts[*n + 1][1] = z1;
            else
                pts[*n][0] = x0, pts[*n][1] = t, pts[*n + 1][0] = x1, pts[*n + 1][1] = t;
            *n += 2;
        }
    }
    static const float minor[4] = {1, 1, 1, 0.13f}, major_c[4] = {1, 1, 1, 0.3f};
    if (nmin)
        add_points(s, SCENE_SEGMENTS, (const double(*)[2])minor_pts, nmin, minor);
    if (nmaj)
        add_points(s, SCENE_SEGMENTS, (const double(*)[2])major_pts, nmaj, major_c);
    g_grid_shown_ft = step;
}

static void add_ring(scene *s, double x, double z, float radius, float width, const float rgba[4])
{
    scene_item *it = add_item(s, SCENE_RING, rgba);
    if (it)
        it->x = (float)x, it->z = (float)z, it->size = radius, it->width = width;
}

/* The game's Gradient Edit graph at the curve's first point (web/gradient-overlay.js graphGeometry, editor.js
   paintProfileGraph): a light backdrop from the lowest to the highest 10 ft line (at least 0 to 100 ft), the 10 ft and
   2.5 ft grid, the profile's outline closed along 0 ft, and a ring at each point, the picked one lit. */
static void scene_graph(scene *s, const curve *c)
{
    static const float back[4] = {1, 1, 1, 0.25f}, major[4] = {0, 0, 0, 0.25f}, minor[4] = {0, 0, 0, 0.0625f};
    static const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 1}, grey[4] = {0.5f, 0.5f, 0.5f, 1};
    int n = c->points;
    if (n < 2)
        return;
    double ax = c->a[0].x, az = c->a[0].z, s0 = c->p[0].s, sn = c->p[n - 1].s, span = sn - s0;
    int lo = 0, hi = 10;
    for (int i = 0; i < n; i++) {
        int f = (int)floor(c->p[i].h / 10), up = (int)ceil(c->p[i].h / 10);
        lo = f < lo ? f : lo, hi = up > hi ? up : hi;
    }
    double bottom = lo * 10.0, top = hi * 10.0;
#define AT(sv, hv) {ax + (sv), az - (hv)}
    double quad[4][2] = {AT(s0, top), AT(s0, bottom), AT(sn, bottom), AT(sn, top)};
    add_points(s, SCENE_QUAD, (const double(*)[2])quad, 4, back);
    static double grid[8192][2];
    int g = 0;
    /* 10 ft lines, then the 2.5 ft ones (which the game starts at lo, not 4 * lo, below 0 ft). */
    for (int pass = 0; pass < 2; pass++) {
        g = 0;
        double step = pass ? 2.5 : 10;
        int from = lo, to = pass ? 4 * hi : hi;
        for (int i = from; i <= to && g + 2 <= 8192; i++) {
            double gh = i * step, a[2] = AT(s0, gh), b[2] = AT(sn, gh);
            grid[g][0] = a[0], grid[g][1] = a[1], grid[g + 1][0] = b[0], grid[g + 1][1] = b[1], g += 2;
        }
        for (int j = 0; j * step <= span && g + 2 <= 8192; j++) {
            double a[2] = AT(s0 + j * step, bottom), b[2] = AT(s0 + j * step, top);
            grid[g][0] = a[0], grid[g][1] = a[1], grid[g + 1][0] = b[0], grid[g + 1][1] = b[1], g += 2;
        }
        add_points(s, SCENE_SEGMENTS, (const double(*)[2])grid, g, pass ? minor : major);
    }
    /* The outline: from (s0, 0) through the profile to (sN, 0), Curve segments every 0.25 ft. */
    static double outline[16384][2];
    int o = 0;
    outline[o][0] = ax + s0, outline[o][1] = az, o++;
    for (int i = 0; i < n && o < 16380; i++) {
        if (c->p[i].type == 1 && i < n - 1) {
            for (double sv = c->p[i].s; sv < c->p[i + 1].s && o < 16380; sv += 0.25)
                outline[o][0] = ax + sv, outline[o][1] = az - profile_height(c->p, n, sv), o++;
        } else {
            outline[o][0] = ax + c->p[i].s, outline[o][1] = az - c->p[i].h, o++;
        }
    }
    outline[o][0] = ax + sn, outline[o][1] = az, o++;
    scene_item *line = add_points(s, SCENE_LINE, (const double(*)[2])outline, o, black);
    if (line)
        line->closed = 1;
    for (int i = 0; i < n; i++) {
        double x = ax + c->p[i].s, z = az - c->p[i].h;
        add_ring(s, x, z, 0.5f, 0.375f, i == g_point ? black : grey);
        add_ring(s, x, z, 0.5f, 0.25f, i == g_point ? white : black);
    }
#undef AT
}

static void scene_curve(scene *s, const curve *c, int chosen, const curve_view *v, double mx, double mz, double mpx, double mpy)
{
    static double path[CURVE_MAX_ANCHORS * 49][2], side[CURVE_MAX_ANCHORS * 49][2];
    int n = pen_points(c, 48, path, CURVE_MAX_ANCHORS * 49);
    add_line(s, path, n, chosen ? C_PATH : C_FAINT);
    if (!chosen) {
        for (int i = 0; i < c->anchors; i++)
            add_dot(s, c->a[i].x, c->a[i].z, 0.35f, C_FAINT);
        return;
    }
    if (g_mode == CURVE_MODE_PATH && c->anchors > 1) {
        /* How wide it'll bake: solid to the inner lines, fading out to the outer ones (offsetLines, 32 a segment). */
        int m = pen_points(c, 32, path, CURVE_MAX_ANCHORS * 49);
        const double widths[4] = {g_inner, -g_inner, g_outer, -g_outer};
        for (int k = 0; k < 4; k++) {
            pen_offset((const double(*)[2])path, m, widths[k], side);
            add_line(s, (const double(*)[2])side, m, k < 2 ? C_INNER : C_OUTER);
        }
    }
    if (g_mode == CURVE_MODE_PATH) {
        /* The stretch out to the mouse isn't part of the curve yet. */
        if (g_drawing && g_drag.kind == DRAG_NONE && !c->closed && thing_at(v, c, mpx, mpy).type == THING_NONE) {
            pen_anchor end = pen_new_anchor(mx, mz);
            double stretch[33][2];
            for (int k = 0; k <= 32; k++)
                pen_bezier(&c->a[c->anchors - 1], &end, k / 32.0, &stretch[k][0], &stretch[k][1]);
            add_line(s, (const double(*)[2])stretch, 33, C_STRETCH);
        }
        for (int i = 0; i < c->anchors; i++) {
            const pen_anchor *a = &c->a[i];
            double hs[2][2] = {{a->ix, a->iz}, {a->ox, a->oz}};
            for (int k = 0; k < 2; k++) {
                if (hs[k][0] == a->x && hs[k][1] == a->z)
                    continue;
                double arm[2][2] = {{a->x, a->z}, {hs[k][0], hs[k][1]}};
                add_line(s, (const double(*)[2])arm, 2, C_HANDLE);
                add_dot(s, hs[k][0], hs[k][1], 0.42f, C_HANDLE);
            }
            add_dot(s, a->x, a->z, 0.55f, i == 0 ? C_FIRST : C_ANCHOR);
        }
    }
    /* Where each profile point falls on the curve (yellow dots), by the fraction of the way along; the picked one bigger. */
    if (c->anchors > 1 && c->points > 0) {
        static const float picked[4] = {1.0f, 0.953f, 0.420f, 1.0f}; /* #fff36b */
        static double d[CURVE_MAX_POINTS], spots[CURVE_MAX_POINTS][2];
        for (int i = 0; i < c->points; i++)
            d[i] = c->p[i].s;
        int path_drag = g_drag.kind != DRAG_NONE && g_drag.kind != DRAG_PROFILE && !g_stretch;
        double total = path_drag ? -1 : c->p[c->points - 1].s;
        if (pen_spots(c, d, c->points, total, spots))
            for (int i = 0; i < c->points; i++)
                add_dot(s, spots[i][0], spots[i][1], i == g_point ? 0.85f : 0.5f, i == g_point ? picked : C_MARKER);
    }
    if (g_mode == CURVE_MODE_PROFILE && c->anchors > 1) {
        scene_graph(s, c);
        /* Where the mouse is along the graph, as a ring on the curve, to find the spot before adding a point there. */
        double sv = mx - c->a[0].x;
        if (c->points && sv >= c->p[0].s && sv <= c->p[c->points - 1].s) {
            double spot[1][2];
            if (pen_spots(c, &sv, 1, c->p[c->points - 1].s, spot))
                add_ring(s, spot[0][0], spot[0][1], 0.42f, 0.14f, C_MARKER);
        }
    }
}

/* The section being marked, on the profile graph (the game's Gradient Edit graph or ours, both at A with s along +x
   and height up the map): a green ring at its start, an orange one at its end, the piece between drawn over the
   outline. So it's plain what Save Section will keep. */
static void sec_scene(scene *s)
{
    int ctx = sec_context();
    if (ctx == SEC_NONE || g_sec_start < 0 || ctx != g_sec_ctx)
        return;
    static profile_point p[CURVE_MAX_POINTS];
    int n, room, picked, owner;
    double ax, az;
    if (!sec_profile(ctx, p, &n, &room, &picked, &owner) || owner != g_sec_owner || g_sec_start >= n)
        return;
    if (ctx == SEC_CURVE) {
        ax = g_curves.c[g_selected].a[0].x, az = g_curves.c[g_selected].a[0].z;
    } else {
        static gradient_copy g;
        if (!game_gradient_copy(owner, &g))
            return;
        ax = g.ax, az = g.az;
    }
    static const float start[4] = {0.490f, 1.0f, 0.541f, 1.0f}, end[4] = {1.0f, 0.58f, 0.2f, 1.0f};
    int last = g_sec_end > g_sec_start && g_sec_end < n ? g_sec_end : -1;
    if (last > 0) {
        static double piece[CURVE_MAX_POINTS * 4][2];
        int k = 0;
        for (int i = g_sec_start; i < last && k < CURVE_MAX_POINTS * 4 - 2; i++) {
            if (p[i].type == 1) /* a Curve segment: follow it, as the outline does */
                for (double sv = p[i].s; sv < p[i + 1].s && k < CURVE_MAX_POINTS * 4 - 2; sv += (p[i + 1].s - p[i].s) / 8)
                    piece[k][0] = ax + sv, piece[k][1] = az - profile_height(p, n, sv), k++;
            else
                piece[k][0] = ax + p[i].s, piece[k][1] = az - p[i].h, k++;
        }
        piece[k][0] = ax + p[last].s, piece[k][1] = az - p[last].h, k++;
        add_points(s, SCENE_LINE, (const double(*)[2])piece, k, start);
        add_ring(s, ax + p[last].s, az - p[last].h, 0.9f, 0.3f, end);
    }
    add_ring(s, ax + p[g_sec_start].s, az - p[g_sec_start].h, 0.9f, 0.3f, start);
}

/* The selected range on the profile graph: a gold ring on every point in it and the profile between them in gold, so
   it's plain which points a drag moves. */
static void range_scene(scene *s)
{
    static profile_point p[CURVE_MAX_POINTS];
    int ctx = sec_context(), n, room, picked, owner;
    double ax, az;
    if (ctx == SEC_NONE || g_rng_a < 0 || !sec_profile(ctx, p, &n, &room, &picked, &owner) || !rng_valid(ctx, owner, n) ||
        !graph_origin(ctx, owner, &ax, &az))
        return;
    int first = rng_first(), last = rng_last();
    if (last > first) {
        static double piece[CURVE_MAX_POINTS * 4][2];
        int k = 0;
        for (int i = first; i < last && k < CURVE_MAX_POINTS * 4 - 2; i++) {
            if (p[i].type == 1) /* a Curve segment: follow it, as the outline does */
                for (double sv = p[i].s; sv < p[i + 1].s && k < CURVE_MAX_POINTS * 4 - 2; sv += (p[i + 1].s - p[i].s) / 8)
                    piece[k][0] = ax + sv, piece[k][1] = az - profile_height(p, n, sv), k++;
            else
                piece[k][0] = ax + p[i].s, piece[k][1] = az - p[i].h, k++;
        }
        piece[k][0] = ax + p[last].s, piece[k][1] = az - p[last].h, k++;
        add_points(s, SCENE_LINE, (const double(*)[2])piece, k, C_MARKER);
    }
    for (int i = first; i <= last; i++)
        add_ring(s, ax + p[i].s, az - p[i].h, i == g_rng_a ? 1.0f : 0.75f, 0.3f, C_MARKER);
}

void curvetool_scene(scene *out, const curve_view *v, double mpx, double mpy)
{
    out->items = out->points = 0;
    out->tool[0] = 0;
    grid_scene(out, v); /* first, so everything else is drawn over it */
    double mx = (mpx - v->w / 2) / v->h * v->view_ft + v->cx, mz = (mpy - v->h / 2) / v->h * v->view_ft + v->cz;
    for (int i = 0; i < g_curves.count; i++)
        if (i != g_selected)
            scene_curve(out, &g_curves.c[i], 0, v, mx, mz, mpx, mpy);
    if (g_selected >= 0 && g_selected < g_curves.count)
        scene_curve(out, &g_curves.c[g_selected], 1, v, mx, mz, mpx, mpy);
    sec_scene(out);
    range_scene(out);
    if (g_mode == CURVE_MODE_STAMP)
        stamptool_scene(out, mx, mz);
}

/* ── the curve tool's settings, kept between sessions ──
   %LOCALAPPDATA%\MX Simulator\mxtools\curve-settings.txt, one "name value" a line. Read once at start; written when
   they've stopped changing for half a second, so a slider drag doesn't write every frame. A missing or bad line leaves
   that setting at its default. */
static int settings_path(wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH];
    return mx_data_dir(dir, MAX_PATH) && swprintf(out, cap, L"%ls\\curve-settings.txt", dir) > 0;
}
static void settings_now(double *v)
{
    v[0] = g_inner, v[1] = g_outer, v[2] = g_bank, v[3] = g_strength, v[4] = g_add, v[5] = g_limit, v[6] = g_stretch;
    v[7] = g_granularity, v[8] = g_trace_fit, v[9] = g_grid, v[10] = g_bake_side, v[11] = g_ease_plain;
}
enum { SETTINGS_N = 12 };
static const char *const SETTING_NAMES[SETTINGS_N] = {"inner", "outer", "bank", "strength", "bake_add", "submode",
                                                     "path_stretch", "granularity", "terrain_fit", "grid", "bake_side",
                                                     "ease_plain"};
static double g_settings_saved[SETTINGS_N];
static int g_settings_quiet = -1; /* frames since they last changed; -1 = nothing to write */

static void settings_load(void)
{
    settings_now(g_settings_saved);
    wchar_t path[MAX_PATH];
    if (!settings_path(path, MAX_PATH))
        return;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    char text[1024];
    DWORD got = 0;
    ReadFile(f, text, sizeof text - 1, &got, NULL);
    CloseHandle(f);
    text[got] = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char name[32];
        double x;
        if (sscanf(line, "%31s %lf", name, &x) != 2 || x != x)
            continue;
        if (!strcmp(name, "inner")) g_inner = clampd(x, 0, 50);
        else if (!strcmp(name, "outer")) g_outer = clampd(x, 0.5, 100);
        else if (!strcmp(name, "bank")) g_bank = clampd(x, -BANK_MAX, BANK_MAX);
        else if (!strcmp(name, "strength")) g_strength = clampd(x, 0, 1);
        else if (!strcmp(name, "bake_add")) g_add = x != 0;
        else if (!strcmp(name, "submode")) g_limit = (int)clampd(x, 0, 2);
        else if (!strcmp(name, "path_stretch")) g_stretch = x != 0;
        else if (!strcmp(name, "granularity")) g_granularity = clampd(x, 0, 1);
        else if (!strcmp(name, "terrain_fit")) g_trace_fit = clampd(x, 0.01, 1);
        else if (!strcmp(name, "grid")) g_grid = (int)clampd(x, 0, 5);
        else if (!strcmp(name, "bake_side")) g_bake_side = (int)clampd(x, 0, 2);
        else if (!strcmp(name, "ease_plain")) g_ease_plain = x != 0;
    }
    curve_ease_plain(g_ease_plain);
    if (g_inner > g_outer)
        g_inner = g_outer;
    settings_now(g_settings_saved);
    mx_log("curve tool: settings read from curve-settings.txt");
}

static void settings_frame(void)
{
    double v[SETTINGS_N];
    settings_now(v);
    if (memcmp(v, g_settings_saved, sizeof v)) {
        memcpy(g_settings_saved, v, sizeof v);
        g_settings_quiet = 0; /* changed: wait for it to settle */
        return;
    }
    if (g_settings_quiet < 0 || ++g_settings_quiet < 30)
        return;
    g_settings_quiet = -1;
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    if (!settings_path(path, MAX_PATH) || swprintf(tmp, MAX_PATH, L"%ls.tmp", path) <= 0)
        return;
    char text[1024];
    int n = 0;
    for (int i = 0; i < SETTINGS_N; i++)
        n += snprintf(text + n, sizeof text - n, "%s %.17g\r\n", SETTING_NAMES[i], v[i]);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, (DWORD)n, &put, NULL) && put == (DWORD)n;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        mx_log("curve tool: couldn't write curve-settings.txt");
}
