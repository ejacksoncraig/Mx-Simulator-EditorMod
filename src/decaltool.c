/* Decal editing in the game's Decals mode (15), like the Track Editor app's decal tools (web/decal-paint.js), with the
   axes the game really uses (game/decals.md section 1):
     u, the image's left to right, runs along (cos rot, -sin rot) and covers width = size * f5 ft;
     v, the image's rows, runs along (sin rot, cos rot) and covers height = size ft.
   (The app has size as the width and f5 = height / width; only that meaning is swapped, its u direction is right.)

   - A click inside a decal picks the smallest one there and makes it the game's selection. Clicks on the selection's
     own centre dot or rotate dot, and on empty ground, still go to the game (move, its rotate, stamping).
   - On the selection: a corner scales it about its centre, a side's handle stretches that side with the opposite one
     staying put (Alt: both sides, about the centre; Shift: whole feet), the round handle turns it (Shift: 15 degrees).
     Each mouse move replaces the decal with the game's own call, which re-bakes only its tiles, as the game's drags do.
   - [ and ] scale by 10% ({ and }, i.e. Shift, by 1% while the game's group is empty; with a group they stay the game's
     spread and pull). Copies are the game's own d. Delete Decal. Ctrl+Z undoes our edits, newest first.
   The game's own Save writes the decals file. Game thread only. */
#include "curvetool.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MODE_DECALS 15
#define PI 3.14159265358979323846
#define SNAP_TURN (PI / 12) /* Shift while turning: 15 degrees */
#define MIN_SIDE 0.1        /* ft, the smallest width or height */
#define HANDLE_PX 7.0       /* how close the mouse must be to a handle, and half its drawn size */
#define TURN_GAP_PX 22.0    /* the turn handle sits this far past the top edge */
#define SIDE_MIN_PX 40.0    /* a side's handle hides when that side is shorter than this on screen */

enum { DT_HEADER = DECALTOOL_ROW, DT_HINT, DT_LABEL, DT_BIGGER, DT_SMALLER, DT_DELETE, DT_UNDO };

static char g_say[192];
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_say, sizeof g_say, fmt, ap);
    va_end(ap);
    game_status(g_say);
    mx_log("decals: %s", g_say);
}

/* ── the maths (pure) ── */

static void half_extents(const game_decal *d, double *hu, double *hv) { *hu = d->size * d->f5 / 2, *hv = d->size / 2.0; }

/* A point in the decal's own axes (u across the image, v down it, ft from the centre) to the world, and back. Uses the
   record's sin and cos, which are what the game draws with. */
static void to_world(const game_decal *d, double u, double v, double *x, double *z)
{
    *x = d->x + u * d->cos_rot + v * d->sin_rot;
    *z = d->z - u * d->sin_rot + v * d->cos_rot;
}

static void to_decal(const game_decal *d, double x, double z, double *u, double *v)
{
    double dx = x - d->x, dz = z - d->z;
    *u = dx * d->cos_rot - dz * d->sin_rot;
    *v = dx * d->sin_rot + dz * d->cos_rot;
}

static int contains(const game_decal *d, double x, double z)
{
    double hu, hv, u, v;
    half_extents(d, &hu, &hv);
    to_decal(d, x, z, &u, &v);
    return fabs(u) <= hu && fabs(v) <= hv;
}

/* In the game's corner order (0x3060): uv (0,0), (1,0), (1,1), (0,1). */
static void corners(const game_decal *d, double out[4][2])
{
    double hu, hv;
    half_extents(d, &hu, &hv);
    static const int sign[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int k = 0; k < 4; k++)
        to_world(d, sign[k][0] * hu, sign[k][1] * hv, &out[k][0], &out[k][1]);
}

static void set_rot(game_decal *d, double rot)
{
    rot = atan2(sin(rot), cos(rot)); /* kept in -pi..pi */
    d->rot = (float)rot, d->sin_rot = sinf(d->rot), d->cos_rot = cosf(d->rot);
}

/* A side's middle in the decal's axes: 0 left (-u), 1 right (+u), 2 top (-v), 3 bottom (+v). */
static const int SIDE[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

/* Dragging a side to (x, z): that side follows the mouse along its axis, the opposite side stays put (centre: both move,
   the centre stays), the other direction keeps its length. Across (u) changes the width: f5 = width / size. Along (v)
   changes the height: size = height, f5 = old width / height. */
static void stretch(const game_decal *from, int side, double x, double z, int centre, int whole_ft, game_decal *out)
{
    int su = SIDE[side][0], sv = SIDE[side][1];
    double hu, hv, u, v;
    half_extents(from, &hu, &hv);
    to_decal(from, x, z, &u, &v);
    double along = su ? u * su : v * sv, half = su ? hu : hv;
    double length = centre ? 2 * along : along + half;
    if (whole_ft)
        length = floor(length + 0.5);
    if (length < MIN_SIDE)
        length = MIN_SIDE;
    double shift = centre ? 0 : (length - 2 * half) / 2; /* the centre follows half the change */
    double cx, cz;
    to_world(from, su * shift, sv * shift, &cx, &cz);
    *out = *from;
    out->x = (float)cx, out->z = (float)cz;
    if (su) {
        out->f5 = (float)(length / from->size);
    } else {
        double width = (double)from->size * from->f5;
        out->size = (float)length;
        out->f5 = (float)(width / length);
    }
}

/* Dragging a corner: the size grows with the mouse's distance from the centre, about the centre; f5 stays. */
static void scale_from_corner(const game_decal *from, double x0, double z0, double x, double z, int whole_ft, game_decal *out)
{
    double was = hypot(x0 - from->x, z0 - from->z);
    *out = *from;
    if (!(was > 0))
        return;
    double size = from->size * hypot(x - from->x, z - from->z) / was;
    if (whole_ft)
        size = floor(size + 0.5);
    double least = MIN_SIDE / (from->f5 < 1 ? from->f5 : 1); /* neither side under the minimum */
    out->size = (float)(size < least ? least : size);
}

/* The game's own rule for its rotate dot (0xec740): rot = atan2(cx - mouse x, cz - mouse z). Turned from where the
   drag began, so grabbing the handle a little off its line doesn't jump. */
static double angle_to(const game_decal *d, double x, double z) { return atan2(d->x - x, d->z - z); }

static void scale_by(game_decal *d, double factor)
{
    double size = d->size * factor, least = MIN_SIDE / (d->f5 < 1 ? d->f5 : 1);
    d->size = (float)(size < least ? least : size);
}

/* ── state ── */

enum { DRAG_NONE, DRAG_CORNER, DRAG_SIDE, DRAG_TURN };
static struct {
    int kind, index, which;
    game_decal start;     /* the decal when the press began */
    double x0, z0, angle0; /* where it began */
    int moved;
} g_drag;

/* Our undo: what each edit changed, so it can be put back with the game's own calls. An entry only applies while the
   decal it left is still there (found by its 72 bytes, at its index or anywhere else after the game's own edits). */
enum { OP_REPLACE, OP_INSERT, OP_DELETE };
typedef struct {
    int op, index;
    game_decal before, after;
} undo_entry;
enum { UNDO_MAX = 64 };
static undo_entry g_undo[UNDO_MAX];
static int g_undo_count;
static char g_undo_track[MAX_PATH];

static void track_now(char *out, size_t cap)
{
    if (!game_track(out, cap))
        out[0] = 0;
}

static void push_undo(int op, int index, const game_decal *before, const game_decal *after)
{
    char track[MAX_PATH];
    track_now(track, sizeof track);
    if (strcmp(track, g_undo_track))
        g_undo_count = 0; /* another track's edits can't be undone here */
    snprintf(g_undo_track, sizeof g_undo_track, "%s", track);
    if (g_undo_count == UNDO_MAX) {
        memmove(&g_undo[0], &g_undo[1], (UNDO_MAX - 1) * sizeof g_undo[0]);
        g_undo_count--;
    }
    undo_entry *e = &g_undo[g_undo_count++];
    memset(e, 0, sizeof *e);
    e->op = op, e->index = index;
    if (before)
        e->before = *before;
    if (after)
        e->after = *after;
}

static int find_decal(const game_decal *want, int hint)
{
    int n;
    const game_decal *list = game_decal_list(&n);
    if (!list)
        return -1;
    if (hint >= 0 && hint < n && !memcmp(&list[hint], want, sizeof *want))
        return hint;
    for (int i = n - 1; i >= 0; i--)
        if (!memcmp(&list[i], want, sizeof *want))
            return i;
    return -1;
}

static int undo_available(void)
{
    char track[MAX_PATH];
    track_now(track, sizeof track);
    return g_undo_count > 0 && !strcmp(track, g_undo_track);
}

static void undo(void)
{
    if (!undo_available()) {
        say("Nothing of ours to undo on this track.");
        return;
    }
    undo_entry *e = &g_undo[g_undo_count - 1];
    const char *why = NULL;
    int at;
    if (e->op == OP_DELETE) {
        int n;
        game_decal_list(&n);
        at = e->index < n ? e->index : n;
        why = game_decal_insert(at, &e->before);
    } else if ((at = find_decal(&e->after, e->index)) < 0) {
        why = "That decal has changed since (moved, or deleted with the game's own keys), so it can't be undone.";
        g_undo_count--; /* it never can be: drop it so the ones before it can still be undone */
    } else if (e->op == OP_REPLACE) {
        why = game_decal_replace(at, &e->before);
        if (!why)
            game_decal_select(at);
    } else {
        why = game_decal_delete(at);
    }
    if (why) {
        say("%s", why);
        return;
    }
    g_undo_count--;
    static const char *const WHAT[] = {"Change undone", "Copy removed", "Deleted decal put back"};
    say("%s.%s", WHAT[e->op], undo_available() ? " Ctrl+Z again for the one before." : "");
}

int decaltool_active(void)
{
    return game_editor_mode() == MODE_DECALS && curvetool_mode() == CURVE_MODE_OFF && game_decals_known();
}

static const game_decal *selected(int *index)
{
    int n, i = game_decal_selected();
    const game_decal *list = game_decal_list(&n);
    *index = i;
    return list && i >= 0 && i < n ? &list[i] : NULL;
}

/* The smallest decal whose turned rectangle holds the point (the top-most of equal ones), or -1. */
static int smallest_at(double x, double z)
{
    int n, best = -1;
    double best_area = 0;
    const game_decal *list = game_decal_list(&n);
    for (int i = 0; list && i < n; i++) {
        double area = (double)list[i].size * list[i].size * list[i].f5;
        if (contains(&list[i], x, z) && (best < 0 || area <= best_area))
            best = i, best_area = area;
    }
    return best;
}

/* ── handles ── */

static void turn_handle(const game_decal *d, double ft_per_px, double *x, double *z)
{
    double hu, hv;
    half_extents(d, &hu, &hv);
    to_world(d, 0, -(hv + TURN_GAP_PX * ft_per_px), x, z); /* past the top edge, the side the game's rotate dot is on */
}

static int side_shown(const game_decal *d, int side, double ft_per_px)
{
    double hu, hv;
    half_extents(d, &hu, &hv);
    double length = SIDE[side][0] ? 2 * hv : 2 * hu; /* the side's own length runs across its axis */
    return length / ft_per_px >= SIDE_MIN_PX;
}

/* Which handle of d is under (x, z): DRAG_* with which = corner or side, or DRAG_NONE. */
static int handle_at(const game_decal *d, double x, double z, double ft_per_px, int *which)
{
    double r = HANDLE_PX * ft_per_px, tx, tz, c[4][2], hu, hv;
    turn_handle(d, ft_per_px, &tx, &tz);
    if (hypot(x - tx, z - tz) <= r * 1.3)
        return DRAG_TURN;
    corners(d, c);
    for (int k = 0; k < 4; k++)
        if (hypot(x - c[k][0], z - c[k][1]) <= r)
            return *which = k, DRAG_CORNER;
    half_extents(d, &hu, &hv);
    for (int k = 0; k < 4; k++) {
        double sx, sz;
        to_world(d, SIDE[k][0] * hu, SIDE[k][1] * hv, &sx, &sz);
        if (side_shown(d, k, ft_per_px) && hypot(x - sx, z - sz) <= r)
            return *which = k, DRAG_SIDE;
    }
    return DRAG_NONE;
}

/* The game's own dots on the selection (0xea690): the centre (move) and centre - 4u (sin, cos) (rotate), pick radius
   u / 2 with u = max(viewFt / 100, 1). Presses on them stay the game's. */
static int on_game_dot(const game_decal *d, double x, double z, double ft_per_px)
{
    int h = 0;
    double view_ft = 0;
    if (!ingame_view(&view_ft, &h))
        return 0;
    (void)ft_per_px;
    double u = view_ft * 0.01 > 1 ? view_ft * 0.01 : 1, r = u / 2;
    double rx = d->x - 4 * u * d->sin_rot, rz = d->z - 4 * u * d->cos_rot;
    return hypot(x - d->x, z - d->z) <= r || hypot(x - rx, z - rz) <= r;
}

/* ── mouse ── */

int decaltool_dragging(void) { return g_drag.kind != DRAG_NONE; }

int decaltool_down(double x, double z, double ft_per_px, int shift, int alt)
{
    (void)shift, (void)alt;
    if (!decaltool_active() || !(ft_per_px > 0))
        return 0;
    int index;
    const game_decal *d = selected(&index);
    int which = 0, kind = d ? handle_at(d, x, z, ft_per_px, &which) : DRAG_NONE;
    if (kind != DRAG_NONE) {
        const char *why = game_decal_editable();
        if (why) {
            say("%s", why);
            return 1; /* still ours: the game would stamp a new decal here */
        }
        memset(&g_drag, 0, sizeof g_drag);
        g_drag.kind = kind, g_drag.which = which, g_drag.index = index, g_drag.start = *d;
        g_drag.x0 = x, g_drag.z0 = z, g_drag.angle0 = angle_to(d, x, z);
        return 1;
    }
    if (d && on_game_dot(d, x, z, ft_per_px))
        return 0;
    int pick = smallest_at(x, z);
    if (pick < 0)
        return 0; /* empty ground: the game's (stamping) */
    game_decal_select(pick);
    int n;
    const game_decal *list = game_decal_list(&n);
    if (list && pick < n)
        say("Decal %d of %d picked: %.2f x %.2f ft.", pick + 1, n, list[pick].size * list[pick].f5, list[pick].size);
    return 1;
}

void decaltool_move(double x, double z, int shift, int alt)
{
    if (g_drag.kind == DRAG_NONE)
        return;
    game_decal next;
    if (g_drag.kind == DRAG_CORNER) {
        scale_from_corner(&g_drag.start, g_drag.x0, g_drag.z0, x, z, shift, &next);
    } else if (g_drag.kind == DRAG_SIDE) {
        stretch(&g_drag.start, g_drag.which, x, z, alt, shift, &next);
    } else {
        next = g_drag.start;
        double rot = g_drag.start.rot + (angle_to(&g_drag.start, x, z) - g_drag.angle0);
        if (shift)
            rot = floor(rot / SNAP_TURN + 0.5) * SNAP_TURN;
        set_rot(&next, rot);
    }
    int n;
    const game_decal *list = game_decal_list(&n);
    if (!list || g_drag.index >= n)
        return;
    if (!memcmp(&list[g_drag.index], &next, sizeof next))
        return;
    const char *why = game_decal_replace(g_drag.index, &next);
    if (why) {
        say("%s", why);
        g_drag.kind = DRAG_NONE;
        return;
    }
    g_drag.moved = 1;
}

void decaltool_up(void)
{
    if (g_drag.kind == DRAG_NONE)
        return;
    int n, kind = g_drag.kind;
    g_drag.kind = DRAG_NONE;
    const game_decal *list = game_decal_list(&n);
    if (!g_drag.moved || !list || g_drag.index >= n)
        return;
    const game_decal *d = &list[g_drag.index];
    push_undo(OP_REPLACE, g_drag.index, &g_drag.start, d);
    if (kind == DRAG_TURN)
        say("Turned to %.1f degrees. Ctrl+Z undoes it.", d->rot * 180 / PI);
    else
        say("Now %.2f ft wide, %.2f ft high (size %.3f, f5 %.3f). Ctrl+Z undoes it.", d->size * d->f5, d->size, d->size, d->f5);
}

/* ── one-step edits ── */

static void replace_selected(void (*edit)(game_decal *, double), double arg, const char *done)
{
    int index;
    const game_decal *d = selected(&index);
    if (!d) {
        say("Click a decal to pick it first.");
        return;
    }
    game_decal before = *d, after = *d;
    edit(&after, arg);
    const char *why = game_decal_replace(index, &after);
    if (why) {
        say("%s", why);
        return;
    }
    int n;
    const game_decal *list = game_decal_list(&n);
    if (list && index < n)
        push_undo(OP_REPLACE, index, &before, &list[index]);
    say(done, after.size * after.f5, after.size);
}

static void delete_selected(void)
{
    int index;
    const game_decal *d = selected(&index);
    if (!d) {
        say("Click a decal to pick it first.");
        return;
    }
    game_decal before = *d;
    const char *why = game_decal_delete(index);
    if (why) {
        say("%s", why);
        return;
    }
    push_undo(OP_DELETE, index, &before, NULL);
    say("Deleted decal %d. Ctrl+Z puts it back.", index + 1);
}

int decaltool_key(int key, int ctrl)
{
    if (!decaltool_active())
        return 0;
    if (key == 26 || (ctrl && (key == 'z' || key == 'Z'))) {
        undo();
        return 1;
    }
    int index;
    if (!selected(&index))
        return 0;
    switch (key) {
    case ']': replace_selected(scale_by, 1.10, "%.2f x %.2f ft."); return 1;
    case '[': replace_selected(scale_by, 1 / 1.10, "%.2f x %.2f ft."); return 1;
    case '}':
    case '{':
        if (game_decal_group_count() != 0)
            return 0; /* with a group, Shift+[ ] stay the game's spread and pull */
        replace_selected(scale_by, key == '}' ? 1.01 : 1 / 1.01, "%.2f x %.2f ft.");
        return 1;
    }
    return 0;
}

/* ── each frame ── */

void decaltool_frame(void)
{
    if (game_editor_mode() != MODE_DECALS || !game_decals_known()) {
        if (g_drag.kind != DRAG_NONE)
            decaltool_up();
        return;
    }
    /* The game's rotate drag writes the previous move's sin and cos, so the ground lags one move behind; put them right
       for the selection (only when they're off). */
    int index;
    if (selected(&index) && game_decal_fix_trig(index))
        mx_log("decals: decal %d's sin and cos put right for its rot", index);
}

/* ── the panel ── */

int decaltool_rows(panel_row *rows, int max, int *kinds, int *args)
{
    if (game_editor_mode() != MODE_DECALS || curvetool_mode() != CURVE_MODE_OFF)
        return 0;
    int n = 0;
    panel_row *r = NULL;
#define ROW(kind_, rowk, label_) \
    (n < max ? (memset(r = &rows[n], 0, sizeof *r), kinds[n] = (kind_), args[n] = 0, r->kind = (rowk), r->label = (label_), n++, 1) \
             : (r = NULL, 0))
    if (!game_decals_known()) {
        ROW(DT_HEADER, ROWK_HEADER, "DECALS");
        if (ROW(DT_HINT, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "Not ported to this game build yet");
        return n;
    }
    int index, count;
    game_decal_list(&count);
    const game_decal *d = selected(&index);
    if (d) {
        if (ROW(DT_HEADER, ROWK_HEADER, ""))
            snprintf(r->text, sizeof r->text, "DECAL %d OF %d", index + 1, count);
        if (ROW(DT_LABEL, ROWK_LABEL, ""))
            snprintf(r->text, sizeof r->text, "%.2f ft wide, %.2f ft high", d->size * d->f5, d->size);
        if (ROW(DT_LABEL, ROWK_LABEL, ""))
            snprintf(r->text, sizeof r->text, "size %.3f, f5 %.3f, %.1f deg", d->size, d->f5, d->rot * 180 / PI);
        ROW(DT_BIGGER, ROWK_BUTTON, "Bigger 10% (])");
        ROW(DT_SMALLER, ROWK_BUTTON, "Smaller 10% ([)");
        ROW(DT_DELETE, ROWK_BUTTON, "Delete Decal");
    } else {
        ROW(DT_HEADER, ROWK_HEADER, "DECALS");
        if (ROW(DT_HINT, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "Click a decal to pick it");
    }
    if (undo_available()) {
        static char label[48];
        snprintf(label, sizeof label, g_undo_count > 1 ? "Undo Decal Edit (%d)" : "Undo Decal Edit", g_undo_count);
        ROW(DT_UNDO, ROWK_BUTTON, label);
    }
    if (d) {
        if (ROW(DT_HINT, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "Corners size it, sides stretch");
        if (ROW(DT_HINT, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "The ring turns it");
        if (ROW(DT_HINT, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "Shift: whole ft or 15 degrees");
        if (ROW(DT_HINT, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "Alt on a side: both sides");
    }
    if (ROW(DT_HINT, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "Save rewrites the decals file;");
    if (ROW(DT_HINT, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "lines it can't read are dropped");
#undef ROW
    return n;
}

void decaltool_press(int kind, int arg)
{
    (void)arg;
    switch (kind) {
    case DT_BIGGER: replace_selected(scale_by, 1.10, "%.2f x %.2f ft."); break;
    case DT_SMALLER: replace_selected(scale_by, 1 / 1.10, "%.2f x %.2f ft."); break;
    case DT_DELETE: delete_selected(); break;
    case DT_UNDO: undo(); break;
    }
}

/* ── on the map ── */

static scene_item *add(scene *s, int kind, const float rgba[4], int count)
{
    if (s->items >= SCENE_MAX_ITEMS || s->points + count > SCENE_MAX_POINTS)
        return NULL;
    scene_item *it = &s->item[s->items++];
    memset(it, 0, sizeof *it);
    it->kind = kind, it->size = 1, it->first = s->points, it->count = count;
    memcpy(it->rgba, rgba, sizeof it->rgba);
    s->points += count;
    return it;
}

static void outline(scene *s, const game_decal *d, const float rgba[4])
{
    double c[4][2];
    corners(d, c);
    scene_item *it = add(s, SCENE_LINE, rgba, 4);
    if (!it)
        return;
    it->closed = 1;
    for (int k = 0; k < 4; k++)
        s->pt[it->first + k][0] = (float)c[k][0], s->pt[it->first + k][1] = (float)c[k][1];
}

/* A dot `px` screen px across: dot sizes are in the marker unit u = max(viewFt / 100, 1). */
static void dot_px(scene *s, double x, double z, double px, double ft_per_px, double u, const float rgba[4])
{
    scene_item *it = add(s, SCENE_DOT, rgba, 0);
    if (it)
        it->x = (float)x, it->z = (float)z, it->size = (float)(px * ft_per_px / u);
}

void decaltool_scene(scene *out, double mouse_x, double mouse_z, double ft_per_px)
{
    if (!decaltool_active() || !(ft_per_px > 0))
        return;
    static const float gold[4] = {1.0f, 0.824f, 0.290f, 0.95f}, hover[4] = {1.0f, 1.0f, 1.0f, 0.45f};
    static const float handle[4] = {1, 1, 1, 1}, edge[4] = {0, 0, 0, 0.8f};
    double view_ft = 0;
    int h = 0;
    if (!ingame_view(&view_ft, &h))
        return;
    double u = view_ft * 0.01 > 1 ? view_ft * 0.01 : 1;
    int index, n;
    const game_decal *list = game_decal_list(&n);
    const game_decal *d = selected(&index);
    /* What a click would pick, faint, unless the mouse is on the selection's handles. */
    int which, hovered = g_drag.kind == DRAG_NONE ? smallest_at(mouse_x, mouse_z) : -1;
    if (hovered >= 0 && hovered != index && !(d && handle_at(d, mouse_x, mouse_z, ft_per_px, &which) != DRAG_NONE))
        outline(out, &list[hovered], hover);
    if (!d)
        return;
    outline(out, d, gold);
    double c[4][2], hu, hv, tx, tz;
    corners(d, c);
    for (int k = 0; k < 4; k++) {
        dot_px(out, c[k][0], c[k][1], 2 * HANDLE_PX + 2, ft_per_px, u, edge);
        dot_px(out, c[k][0], c[k][1], 2 * HANDLE_PX - 2, ft_per_px, u, handle);
    }
    half_extents(d, &hu, &hv);
    for (int k = 0; k < 4; k++) {
        double sx, sz;
        if (!side_shown(d, k, ft_per_px))
            continue;
        to_world(d, SIDE[k][0] * hu, SIDE[k][1] * hv, &sx, &sz);
        dot_px(out, sx, sz, HANDLE_PX + 4, ft_per_px, u, edge);
        dot_px(out, sx, sz, HANDLE_PX, ft_per_px, u, gold);
    }
    /* The turn handle: a ring past the top edge, joined to its middle. */
    turn_handle(d, ft_per_px, &tx, &tz);
    double mx, mz;
    to_world(d, 0, -hv, &mx, &mz);
    scene_item *stem = add(out, SCENE_LINE, gold, 2);
    if (stem)
        out->pt[stem->first][0] = (float)mx, out->pt[stem->first][1] = (float)mz, out->pt[stem->first + 1][0] = (float)tx,
        out->pt[stem->first + 1][1] = (float)tz;
    scene_item *ring = add(out, SCENE_RING, handle, 0);
    if (ring)
        ring->x = (float)tx, ring->z = (float)tz, ring->size = (float)(HANDLE_PX * ft_per_px / u),
        ring->width = (float)(2.5 * ft_per_px / u);
}
