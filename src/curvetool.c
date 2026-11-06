/* The curve tool in the game's editor (docs/ingame-tools.md section 8), on the game thread.
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
static int g_mode;
static int g_selected = -1;
static int g_drawing; /* clicks add points to the selected curve, until it's finished */
static int g_dirty;   /* edited here since the file was read or written */
static wchar_t g_file[MAX_PATH];
static FILETIME g_file_time;
static char g_status[200] = "not started";
static int g_frames;
static double g_inner = 10, g_outer = 20; /* the app's defaults (curve-tool.js curveBake); S5 makes them sliders */

enum { DRAG_NONE, DRAG_PULL, DRAG_HANDLE, DRAG_ANCHOR, DRAG_PROFILE };
static int g_point = -1;       /* the profile point picked in Curve Profile */
static int g_pending_type;     /* Segment Type with no point picked (only the widget changes, as in the game) */
static double g_granularity;   /* the profile graph's snap, 0 = none */
static struct {
    int kind, curve, anchor, out_side, moved, changed, closes, resumes, split;
    double from_px, from_py;
} g_drag;

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

static void load(void)
{
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

void curvetool_start(void) { follow_track(); }

static void live_frame(void);

void curvetool_frame(void)
{
    live_frame();
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

/* Set From Terrain: the game's own ground under the path (any preview taken off first, so it reads the real ground). */
static void profile_terrain(curve *c)
{
    live_cancel();
    game_map m;
    if (!game_heights(&m) || !profile_from_ground(c, m.heights, m.width, m.cell_ft)) {
        say("Couldn't read the ground along this curve.");
        return;
    }
    g_point = -1;
    g_dirty = 1;
    status_now();
    static char text[96];
    snprintf(text, sizeof text, "Profile traced from the ground along the curve: %d points.", c->points);
    say(text);
}

static void mouse_down(const curve_view *v, double px, double py, double x, double z, int ctrl, int alt)
{
    if (g_mode == CURVE_MODE_PROFILE) {
        profile_down(v, px, py, x, z);
        return;
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

void curvetool_mouse(const curve_view *v, int type, double px, double py, int ctrl, int alt)
{
    double x = (px - v->w / 2) / v->h * v->view_ft + v->cx, z = (py - v->h / 2) / v->h * v->view_ft + v->cz;
    if (type == CURVE_DOWN)
        mouse_down(v, px, py, x, z, ctrl, alt);
    else if (type == CURVE_MOVE)
        mouse_move(px, py, x, z, alt);
    else
        mouse_up();
}

int curvetool_dragging(void) { return g_drag.kind != DRAG_NONE; }

int curvetool_key(int key)
{
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
    double settings[6];        /* and the bake settings it used */
    int last_index, frames;
} g_live;
static double g_strength = 1, g_bank = 0;
static int g_add = 0, g_limit = CURVE_LIMIT_NONE;

static void live_forget(void)
{
    free(g_live.base), free(g_live.shown);
    g_live.base = g_live.shown = NULL;
    g_live.last_index = -1;
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
    const char *why = curve_paint(painted, g_live.at_row, g_live.at_col, g_live.rows, g_live.cols, m.width, m.cell_ft, c,
                                  g_inner, g_outer, g_strength, g_add, g_limit, g_bank, NULL)
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
            live_write(ground, 0);
            free(ground);
        }
    }
    live_forget();
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
    if (g_selected < 0 || g_selected >= g_curves.count || g_curves.c[g_selected].anchors < 2) {
        if (g_live.shown)
            live_cancel();
        return;
    }
    if (++g_live.frames % 2)
        return;
    const curve *c = &g_curves.c[g_selected];
    double settings[6] = {g_inner, g_outer, g_bank, g_strength, g_add, g_limit};
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
    ROW_SEC_TYPED, ROW_BRUSH_SHAPE, ROW_BRUSH_ANGLE, ROW_BRUSH_OVAL, ROW_BRUSH_NOTE, ROW_HEADER, ROW_SEC_CLEAR
};
static int g_row_arg[64]; /* a list row's section, or a gradtool row's argument */
static int g_sec_choice;  /* the section chosen in the Section row */
static int g_sec_list_open;   /* the Section dropdown shows every saved section */
static int g_typing;          /* Rename Section: the keyboard is ours */
static char g_typed[48];
static char g_typed_label[64];

int curvetool_typing(void) { return g_typing; }

static void say(const char *text);

static void keep_typed(void)
{
    g_typing = 0;
    const char *why = library_rename(g_sec_choice, g_typed);
    say(why ? why : "Renamed.");
}

void curvetool_type(int key)
{
    size_t n = strlen(g_typed);
    if (key == 13 || key == 10)
        keep_typed();
    else if (key == 27)
        g_typing = 0;
    else if (key == 8 && n)
        g_typed[n - 1] = 0;
    else if (key >= 32 && key < 127 && n + 1 < sizeof g_typed)
        g_typed[n] = (char)key, g_typed[n + 1] = 0;
}
static const char *const SEGMENT_OPTIONS[] = {"Line", "Curve"};

/* Brush shapes for the game's own brush (ingame.c patches its distance while one isn't Circle). */
static const char *const SHAPE_OPTIONS[] = {"Circle", "Square", "Oval"};
static int g_brush_shape = BRUSH_CIRCLE;
static double g_brush_angle = 0, g_brush_oval = 0.5;
int curvetool_brush_shape(void) { return g_brush_shape; }
double curvetool_brush_angle(void) { return g_brush_angle; }
double curvetool_brush_oval(void) { return g_brush_oval; }

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
void curvetool_gradients_changed(void)
{
    if (g_sec_ctx == SEC_GAME)
        g_sec_start = g_sec_end = -1, g_sec_owner = -1;
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
        why = library_delete(g_sec_choice);
        if (!why) {
            g_sec_choice = g_sec_choice >= library_count() ? library_count() - 1 : g_sec_choice;
            say("Section deleted.");
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
static int g_row_kind[MAX_PANEL_ROWS]; /* g_stretch (Path Change: 0 Extend, 1 Stretch) is with commit_shape */
static const char *const RESIZE_OPTIONS[] = {"Extend", "Stretch"};
static const char *const BAKE_OPTIONS[] = {"Paint", "Add"};
static const char *const SUBMODE_OPTIONS[] = {"none", "raise", "lower"};

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

int curvetool_panel(panel_row *rows, int max)
{
    int n = 0;
    panel_row *r = NULL;
#define ROW(kind_, rowk, label_)                                \
    (n < max && n < MAX_PANEL_ROWS ? (memset(r = &rows[n], 0, sizeof *r), g_row_kind[n] = (kind_), r->kind = (rowk), \
                                      r->label = (label_), n++, 1)                                                    \
                                   : (r = NULL, 0))
    ROW(ROW_HEADER, ROWK_FAINT, "MX Tools " MXMOD_VERSION); /* so a screenshot says which version it was */
    ROW(ROW_HEADER, ROWK_HEADER, "CURVES");
    if (ROW(ROW_PATH, ROWK_BUTTON, "Curve Path"))
        r->on = g_mode == CURVE_MODE_PATH;
    if (ROW(ROW_PROFILE, ROWK_BUTTON, "Curve Profile"))
        r->on = g_mode == CURVE_MODE_PROFILE;
    /* For the game's own brush modes (0 Raise .. 8 Gradient Radial Add): the brush's shape. */
    int game_mode = game_editor_mode();
    if (g_mode == CURVE_MODE_OFF && game_mode >= 0 && game_mode <= 8) {
        ROW(ROW_HEADER, ROWK_HEADER, "BRUSH SHAPE");
        if (ROW(ROW_BRUSH_SHAPE, ROWK_CHOICE, "Brush Shape"))
            r->options = SHAPE_OPTIONS, r->count = 3, r->selected = g_brush_shape;
        if (g_brush_shape != BRUSH_CIRCLE && ROW(ROW_BRUSH_ANGLE, ROWK_SLIDER, "Brush Angle"))
            r->value = g_brush_angle, r->min = 0, r->max = 180, r->step = 5;
        if (g_brush_shape == BRUSH_OVAL && ROW(ROW_BRUSH_OVAL, ROWK_SLIDER, "Oval Width"))
            r->value = g_brush_oval, r->min = 0.1, r->max = 1, r->step = 0.05;
        if (g_brush_shape != BRUSH_CIRCLE && game_mode == 2 && ROW(ROW_BRUSH_NOTE, ROWK_LABEL, ""))
            snprintf(r->text, sizeof r->text, "Smooth stays round");
    }
    /* For the game's own gradients (Delete, Export, Saved Gradients, Gradient Length) and the radial sweep: gradtool.c. */
    if (g_mode == CURVE_MODE_OFF) {
        int room = (max < MAX_PANEL_ROWS ? max : MAX_PANEL_ROWS) - n;
        if (room > 0)
            n += gradtool_rows(rows + n, room, g_row_kind + n, g_row_arg + n);
    }
    if (g_mode == CURVE_MODE_PATH)
        ROW(g_drawing ? ROW_FINISH : ROW_NEW, ROWK_BUTTON, g_drawing ? "Finish Curve (Enter)" : "New Curve");
    const curve *c = g_selected >= 0 && g_selected < g_curves.count ? &g_curves.c[g_selected] : NULL;
    if (g_mode != CURVE_MODE_OFF && c) {
        if (ROW(ROW_LENGTH, ROWK_LABEL, ""))
            snprintf(r->text, sizeof r->text, "Length: %.2f", pen_length(c));
        if (g_mode == CURVE_MODE_PROFILE) {
            /* The game's Gradient Edit rows (docs/game/editor-gradients-ui.md section 10). */
            const profile_point *point = g_point >= 0 && g_point < c->points ? &c->p[g_point] : NULL;
            if (ROW(ROW_SEGMENT, ROWK_CHOICE, "Segment Type"))
                r->options = SEGMENT_OPTIONS, r->count = 2, r->selected = point ? point->type == 1 : g_pending_type;
            if (ROW(ROW_GRANULARITY, ROWK_SLIDER, "Granularity"))
                r->value = g_granularity, r->min = 0, r->max = 1, r->step = 1.0 / 16;
            ROW(ROW_FROM_TERRAIN, ROWK_BUTTON, "Set From Terrain");
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
            r->value = g_inner, r->min = 0, r->max = 100, r->step = 0.5;
        if (ROW(ROW_OUTER, ROWK_SLIDER, "Outer"))
            r->value = g_outer, r->min = 0.5, r->max = 150, r->step = 0.5;
        if (ROW(ROW_BANK, ROWK_SLIDER, "Bank"))
            r->value = g_bank, r->min = -1, r->max = 1, r->step = 0.01;
        if (ROW(ROW_STRENGTH, ROWK_SLIDER, "Strength"))
            r->value = g_strength, r->min = 0, r->max = 1, r->step = 0.05;
        if (ROW(ROW_BAKE_AS, ROWK_CHOICE, "Bake As"))
            r->options = BAKE_OPTIONS, r->count = 2, r->selected = g_add;
        if (ROW(ROW_SUBMODE, ROWK_CHOICE, "SubMode"))
            r->options = SUBMODE_OPTIONS, r->count = 3, r->selected = g_limit;
        if (ROW(ROW_LIVE, ROWK_BUTTON, g_live.on ? "Live Preview: On" : "Live Preview: Off"))
            r->on = g_live.on;
        ROW(ROW_APPLY, ROWK_BUTTON, "Apply To Ground");
        if (g_mode == CURVE_MODE_PATH)
            ROW(ROW_DELETE, ROWK_BUTTON, "Delete Curve");
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
    if (ctx == SEC_NONE || !library_count())
        g_typing = 0, g_sec_list_open = 0;
    if (ctx != SEC_NONE) {
        static profile_point p[CURVE_MAX_POINTS];
        int count, room, picked, owner;
        if (sec_profile(ctx, p, &count, &room, &picked, &owner)) {
            sec_follow(ctx, owner);
            if (g_sec_start >= count || g_sec_end >= count)
                g_sec_start = g_sec_end = -1; /* the profile lost points since */
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
                    for (int i = 0; i < library_count(); i++) {
                        snprintf(item_label[i], sizeof item_label[i], "%s %s", i == g_sec_choice ? ">" : "  ", names[i]);
                        if (ROW(ROW_SEC_ITEM, ROWK_BUTTON, item_label[i])) {
                            r->on = i == g_sec_choice;
                            g_row_arg[n - 1] = i;
                        }
                    }
                    if (ROW(ROW_HEADER, ROWK_HINT, ""))
                        snprintf(r->text, sizeof r->text, "Click one to use it");
                } else if (library_count() > 1 && ROW(ROW_HEADER, ROWK_HINT, "")) {
                    snprintf(r->text, sizeof r->text, "Click the name to pick another");
                }
                if (picked < 0 || picked >= count) {
                    if (ROW(ROW_HEADER, ROWK_HINT, ""))
                        snprintf(r->text, sizeof r->text, "Click the point to paste it at");
                }
                ROW(ROW_SEC_PASTE, ROWK_BUTTON, "Paste At Picked Point");
                if (g_typing) {
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
        }
    }
    if (g_mode != CURVE_MODE_OFF || g_dirty)
        ROW(ROW_SAVE, ROWK_BUTTON, g_dirty ? "Save Curves*" : "Save Curves");
#undef ROW
    return n;
}

void curvetool_slide(int row, double value)
{
    if (row < 0 || row >= MAX_PANEL_ROWS)
        return;
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
    case ROW_BRUSH_ANGLE: g_brush_angle = value; break;
    case ROW_BRUSH_OVAL: g_brush_oval = value; break;
    }
}

void curvetool_press(int row)
{
    if (row < 0 || row >= MAX_PANEL_ROWS)
        return;
    if (g_row_kind[row] >= GRADTOOL_ROW) {
        gradtool_press(g_row_kind[row], g_row_arg[row]);
        return;
    }
    switch (g_row_kind[row]) {
    case ROW_BRUSH_SHAPE: g_brush_shape = (g_brush_shape + 1) % 3; break;
    case ROW_SEC_CHOICE: g_sec_list_open = !g_sec_list_open; break;
    case ROW_SEC_ITEM:
        g_sec_choice = g_row_arg[row];
        g_sec_list_open = 0;
        g_typing = 0;
        break;
    case ROW_SEC_RENAME:
        snprintf(g_typed, sizeof g_typed, "%s", library_get(g_sec_choice) ? library_get(g_sec_choice)->name : "");
        g_typing = 1;
        g_sec_list_open = 0;
        say("Type a name: Enter keeps it, Esc cancels.");
        break;
    case ROW_SEC_KEEP: keep_typed(); break;
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
    case ROW_FINISH: finish_curve(); break;
    case ROW_NEW: g_selected = -1, g_drawing = 0; break;
    case ROW_DELETE: delete_curve(g_selected); break;
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

void curvetool_scene(scene *out, const curve_view *v, double mpx, double mpy)
{
    out->items = out->points = 0;
    out->tool[0] = 0;
    double mx = (mpx - v->w / 2) / v->h * v->view_ft + v->cx, mz = (mpy - v->h / 2) / v->h * v->view_ft + v->cz;
    for (int i = 0; i < g_curves.count; i++)
        if (i != g_selected)
            scene_curve(out, &g_curves.c[i], 0, v, mx, mz, mpx, mpy);
    if (g_selected >= 0 && g_selected < g_curves.count)
        scene_curve(out, &g_curves.c[g_selected], 1, v, mx, mz, mpx, mpy);
    sec_scene(out);
}
