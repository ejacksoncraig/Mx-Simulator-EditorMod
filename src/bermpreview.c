/* Berm Preview: what Gradient Radial Add would do with the picked gradient, shown on the real ground while it's edited in
   Placement and Edit. No undo step: the ground under it is kept, each change is painted from that copy, and it's put back
   when the preview stops.

   One stroke of Radial Add takes a cell to (ground when the stroke began) + weight * h(s), whatever the Strength or the
   number of steps (ingame-tools.md), so with the whole wedge painted at full weight that is what's shown. Past the wedge's
   edges and the profile's end it fades over the brush's falloff.

   Only while the game's tools are in use and only in those two modes: leaving them ends it, so a stroke never lands on a
   preview. Updates wait for the mouse: the game's own drags hold it, and writes are refused then. Game thread only. */
#include "curvetool.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODE_PLACEMENT 9
#define MODE_EDIT 10

static struct {
    int on;
    float *base; /* the ground before the preview, rows x cols from (at_row, at_col) */
    int at_row, at_col, rows, cols;
    float *shown; /* what was last written, s_rows x s_cols from (s_row, s_col); NULL when nothing is */
    int s_row, s_col, s_rows, s_cols;
    float lo, hi; /* the track's height range before the preview */
    int shown_valid, wait, frames;
    gradient_copy last; /* what the preview shows now, to paint again only when it changes */
    int last_index, last_side;
    double last_deg, last_fade;
} g_pv;
static char g_track[MAX_PATH];
static char g_say[160];

static void say(const char *text)
{
    snprintf(g_say, sizeof g_say, "%s", text);
    game_status(g_say);
    mx_log("berm preview: %s", g_say);
}

int bermpreview_on(void) { return g_pv.on; }

static void forget(void)
{
    free(g_pv.base), free(g_pv.shown);
    g_pv.base = g_pv.shown = NULL;
    g_pv.shown_valid = 0;
}

static int was_shown(int gr, int gc)
{
    return g_pv.shown && gr >= g_pv.s_row && gr < g_pv.s_row + g_pv.s_rows && gc >= g_pv.s_col && gc < g_pv.s_col + g_pv.s_cols;
}

/* Everything kept must still be as left: the ground from before, with the last preview on it. */
static int untouched(const game_map *m)
{
    if (!g_pv.base)
        return 1;
    if (g_pv.at_row + g_pv.rows > m->width || g_pv.at_col + g_pv.cols > m->width)
        return 0;
    for (int r = 0; r < g_pv.rows; r++)
        for (int c = 0; c < g_pv.cols; c++) {
            int gr = g_pv.at_row + r, gc = g_pv.at_col + c;
            float want = was_shown(gr, gc) ? g_pv.shown[(size_t)(gr - g_pv.s_row) * g_pv.s_cols + (gc - g_pv.s_col)]
                                           : g_pv.base[(size_t)r * g_pv.cols + c];
            if (m->heights[(size_t)gr * m->width + gc] != want)
                return 0;
        }
    return 1;
}

/* Grow the kept ground to cover these cells. The new part comes from the game, which has no preview there. */
static int cover(const game_map *m, int r0, int r1, int c0, int c1)
{
    if (g_pv.base) {
        int er = g_pv.at_row + g_pv.rows - 1, ec = g_pv.at_col + g_pv.cols - 1;
        r0 = r0 < g_pv.at_row ? r0 : g_pv.at_row, c0 = c0 < g_pv.at_col ? c0 : g_pv.at_col;
        r1 = r1 > er ? r1 : er, c1 = c1 > ec ? c1 : ec;
        if (r0 == g_pv.at_row && c0 == g_pv.at_col && r1 == er && c1 == ec)
            return 1;
    } else if (!game_height_range(&g_pv.lo, &g_pv.hi)) {
        return 0;
    }
    int rows = r1 - r0 + 1, cols = c1 - c0 + 1;
    float *grown = malloc((size_t)rows * cols * sizeof(float));
    if (!grown)
        return 0;
    for (int r = 0; r < rows; r++)
        memcpy(grown + (size_t)r * cols, m->heights + (size_t)(r0 + r) * m->width + c0, (size_t)cols * sizeof(float));
    if (g_pv.base) {
        for (int r = 0; r < g_pv.rows; r++)
            memcpy(grown + (size_t)(g_pv.at_row - r0 + r) * cols + (g_pv.at_col - c0), g_pv.base + (size_t)r * g_pv.cols,
                   (size_t)g_pv.cols * sizeof(float));
        free(g_pv.base);
    }
    g_pv.base = grown, g_pv.at_row = r0, g_pv.at_col = c0, g_pv.rows = rows, g_pv.cols = cols;
    return 1;
}

/* Write what's changing: cells the preview moves, plus wherever the last one was (so it's put back there). */
static const char *write_ground(const float *painted)
{
    int rows = g_pv.rows, cols = g_pv.cols, top = rows, bottom = -1, left = cols, right = -1;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            if (was_shown(g_pv.at_row + r, g_pv.at_col + c) || painted[(size_t)r * cols + c] != g_pv.base[(size_t)r * cols + c]) {
                top = r < top ? r : top, bottom = r > bottom ? r : bottom;
                left = c < left ? c : left, right = c > right ? c : right;
            }
    if (bottom < 0)
        return NULL; /* nothing changes */
    int h = bottom - top + 1, w = right - left + 1;
    float *block = malloc((size_t)h * w * sizeof(float));
    if (!block)
        return "out of memory";
    for (int r = 0; r < h; r++)
        memcpy(block + (size_t)r * w, painted + (size_t)(top + r) * cols + left, (size_t)w * sizeof(float));
    const char *why = game_write_heights(g_pv.at_col + left, g_pv.at_row + top, w, h, block, g_pv.shown == NULL, 0);
    if (why) {
        free(block);
        return why;
    }
    free(g_pv.shown);
    g_pv.shown = block, g_pv.s_row = g_pv.at_row + top, g_pv.s_col = g_pv.at_col + left, g_pv.s_rows = h, g_pv.s_cols = w;
    return NULL;
}

/* Put back the ground and the height range. 0 if the write was refused: everything is still kept, to try again. A ground
   that changed under the preview (the game's undo) has nothing of ours left to put back. */
static int put_back(void)
{
    game_map m;
    if (g_pv.shown && game_heights(&m) && untouched(&m)) {
        if (write_ground(g_pv.base))
            return 0;
        game_set_height_range(g_pv.lo, g_pv.hi);
    }
    forget();
    return 1;
}

int bermpreview_lift(void)
{
    if (!g_pv.shown)
        return 1;
    if (!put_back())
        return 0;
    g_pv.wait = 30; /* a moment before it comes back, past whatever the game is about to do */
    return 1;
}

void bermpreview_toggle(void)
{
    if (g_pv.on) {
        g_pv.on = 0;
        put_back(); /* if refused, bermpreview_frame tries again */
    } else {
        g_pv.on = 1, g_pv.wait = 0;
        forget();
    }
}

/* Paint the gradient on the ground as it was before the preview. */
static const char *show(const gradient_copy *g, double fade)
{
    game_map m;
    if (!game_heights(&m))
        return "no track loaded";
    if (!untouched(&m))
        forget(); /* the game's undo changed the ground under it: start again from the ground as it is now */
    double end_s = g->p[g->points - 1].s, reach = end_s + fade, cell = m.cell_ft;
    int c0 = (int)floor((g->ax - reach) / cell), c1 = (int)ceil((g->ax + reach) / cell);
    int r0 = (int)floor((g->az - reach) / cell), r1 = (int)ceil((g->az + reach) / cell);
    c0 = c0 < 0 ? 0 : c0, r0 = r0 < 0 ? 0 : r0;
    c1 = c1 >= m.width ? m.width - 1 : c1, r1 = r1 >= m.width ? m.width - 1 : r1;
    if (c0 > c1 || r0 > r1)
        return "This gradient is off the map.";
    if (!cover(&m, r0, r1, c0, c1))
        return "out of memory";
    size_t cells = (size_t)g_pv.rows * g_pv.cols;
    float *painted = malloc(cells * sizeof(float));
    if (!painted)
        return "out of memory";
    memcpy(painted, g_pv.base, cells * sizeof(float));
    double mid, half;
    gradtool_wedge(g->ax, g->az, g->bx, g->bz, &mid, &half);
    for (int row = r0; row <= r1; row++)
        for (int col = c0; col <= c1; col++) {
            double vx = col * cell - g->ax, vz = row * cell - g->az, s = sweep_s(vx, vz, mid, half);
            double weight = sweep_weight(vx, vz, mid, half, s, end_s, fade);
            if (weight > 0) {
                size_t at = (size_t)(row - g_pv.at_row) * g_pv.cols + (col - g_pv.at_col);
                painted[at] = (float)(g_pv.base[at] + weight * profile_height(g->p, g->points, s));
            }
        }
    const char *why = write_ground(painted);
    free(painted);
    return why;
}

static int same_gradient(const gradient_copy *a, const gradient_copy *b)
{
    return !memcmp(a, b, offsetof(gradient_copy, p)) && !memcmp(a->p, b->p, (size_t)a->points * sizeof a->p[0]);
}

void bermpreview_frame(void)
{
    char track[MAX_PATH];
    if (!game_track(track, sizeof track))
        track[0] = 0;
    if (strcmp(track, g_track)) {
        /* Another track: what's kept belongs to the last one's ground, so it's dropped, not put back. */
        snprintf(g_track, sizeof g_track, "%s", track);
        g_pv.on = 0;
        forget();
    }
    if (!g_pv.on) {
        if (g_pv.shown)
            put_back(); /* a refused write from when it stopped */
        return;
    }
    int mode = game_editor_mode();
    if (curvetool_mode() != CURVE_MODE_OFF || (mode != MODE_PLACEMENT && mode != MODE_EDIT)) {
        g_pv.on = 0;
        put_back();
        say("Berm Preview is off: the ground is back as it was.");
        return;
    }
    if (g_pv.wait > 0) {
        g_pv.wait--;
        return;
    }
    if (++g_pv.frames % 2 || game_mouse_held())
        return;
    game_map m;
    if (!game_heights(&m) || m.locked)
        return;

    static gradient_copy g;
    int selected = game_selected_gradient();
    if (selected < 0 || !game_gradient_copy(selected, &g) || g.points < 1 || !(g.p[g.points - 1].s > 0)) {
        if (g_pv.shown)
            put_back();
        g_pv.shown_valid = 0;
        return;
    }
    double deg, outer, inner, fade = 0;
    int side;
    gradtool_sweep(&deg, &side);
    if (curvetool_brush_feet(&outer, &inner) && outer > inner)
        fade = outer - inner;
    if (g_pv.shown_valid && g_pv.last_index == selected && same_gradient(&g, &g_pv.last) && deg == g_pv.last_deg &&
        side == g_pv.last_side && fade == g_pv.last_fade) {
        /* Nothing to paint, but the game's undo may have taken the preview away: paint again if so. */
        if (g_pv.frames % 30 || untouched(&m))
            return;
    }
    const char *why = show(&g, fade);
    if (why) {
        g_pv.on = 0;
        put_back();
        char stopped[160];
        snprintf(stopped, sizeof stopped, "Berm Preview stopped: %s", why);
        say(stopped);
        return;
    }
    g_pv.last = g, g_pv.last_index = selected, g_pv.last_deg = deg, g_pv.last_side = side, g_pv.last_fade = fade;
    g_pv.shown_valid = 1;
}
