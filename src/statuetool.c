/* A group of statues in the game's Statues mode (13), so the game's Snap to Gradient can run on many at once:
   - Ctrl+click on a statue adds it to the group or takes it out. If the group is empty, the game's own selection
     joins it too, so "click one, Ctrl+click the rest" works.
   - Ctrl+drag on empty ground draws a box; every statue inside joins the group.
   - A plain drag on a statue in the group moves the whole group by the mouse's offset. Each keeps its turn and its
     height above the ground (the game's update puts it back on the ground at the new spot).
   - Duplicate Group (or d with the mouse on the map) copies every group statue with the game's own add, beside the group
     or centred on the mouse; the group becomes the copies. Undo takes them back off the end of the list.
   - Snap Group to Gradient runs the game's own snap math (game_statue_snap, a copy of 1684 0xe4291) on each statue in
     the group against the selected gradient. Undo (the panel button or Ctrl+Z) puts a move or snap back, newest first.
   - A statue in one of the app's linked pieces (mx-tools-groups.json, statuelinks.c) brings its whole piece: Ctrl+click
     adds or drops all of it, a Ctrl+drag box that takes one part takes the rest, Duplicate keeps each copy linked, and
     Snap moves each piece as one rigid thing (its middle onto the gradient, every part turned and moved the same).
   Other presses without Ctrl, and every other key, stay the game's. The group is indices into the game's list, so
   it's cleared whenever the list's length changes (the game's d, Del or a reload). Game thread only. */
#include "curvetool.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODE_STATUES 13
#define MAX_GROUP 4096
#define MAX_UNDO 16
#define PICK_MIN_PX 6.0 /* the pick radius is the game's u / 2, but never less than this on screen */
#define COPY_SIDE_FT 10.0 /* Duplicate Group's button puts the copies this far along +x (Copy Gradient's 10 ft) */

enum { ST_HEADER = STATUETOOL_ROW, ST_HINT, ST_LABEL, ST_SNAP, ST_CLEAR, ST_UNDO, ST_COPY, ST_SCALE_BY, ST_SCALE, ST_REAL_SIZE };

#define FILE_CAP 1024
static double g_scale_pct = 110; /* Scale by (%): each press multiplies the statues' present size by this */

static char g_say[512];
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_say, sizeof g_say, fmt, ap);
    va_end(ap);
    game_status(g_say);
    mx_log("statues: %s", g_say);
}

static int g_group[MAX_GROUP], g_group_n;
static int g_list_n = -1; /* the statue count the group was made against */

static struct {
    int on;
    double x0, z0, x1, z1;
} g_box;

/* A group move in progress: where the press was, and each member's pose then (g_group's order). */
static struct {
    int on, wrote;
    double x0, z0;
    statue_pose start[MAX_GROUP];
} g_move;

typedef struct {
    const char *what; /* "Move", "Snap" or "Copy", for the panel and the messages */
    int copy_base;    /* Copy: the list's length before, so the copies are copy_base .. copy_base + n - 1 */
    int n;
    int *index;
    statue_pose *before, *after;
    char **files; /* Scale: per statue its old mesh, old shape and new mesh (malloc'ed), 3 * n */
} undo_step;
static undo_step g_undo[MAX_UNDO];
static int g_undo_n;

static void undo_free(undo_step *u)
{
    if (u->files)
        for (int k = 0; k < 3 * u->n; k++)
            free(u->files[k]);
    free(u->index), free(u->before), free(u->after), free(u->files);
    memset(u, 0, sizeof *u);
}

static int undo_alloc(undo_step *u, const char *what, int n)
{
    memset(u, 0, sizeof *u);
    u->what = what;
    u->index = malloc(sizeof(int) * n);
    u->before = malloc(sizeof(statue_pose) * n);
    u->after = malloc(sizeof(statue_pose) * n);
    if (u->index && u->before && u->after)
        return 1;
    undo_free(u);
    return 0;
}

/* Takes ownership of u; the oldest step goes when the list is full. */
static void undo_push(undo_step *u)
{
    if (g_undo_n == MAX_UNDO) {
        undo_free(&g_undo[0]);
        memmove(&g_undo[0], &g_undo[1], sizeof g_undo[0] * (MAX_UNDO - 1));
        g_undo_n--;
    }
    g_undo[g_undo_n++] = *u;
}

static void undo_clear(void)
{
    for (int k = 0; k < g_undo_n; k++)
        undo_free(&g_undo[k]);
    g_undo_n = 0;
}

int statuetool_active(void)
{
    return game_editor_mode() == MODE_STATUES && curvetool_mode() == CURVE_MODE_OFF && game_statues_known();
}

static int in_group(int i)
{
    for (int k = 0; k < g_group_n; k++)
        if (g_group[k] == i)
            return k;
    return -1;
}

static void group_add(int i)
{
    if (i >= 0 && in_group(i) < 0 && g_group_n < MAX_GROUP)
        g_group[g_group_n++] = i;
}

static void group_remove(int i)
{
    int k = in_group(i);
    if (k >= 0)
        memmove(&g_group[k], &g_group[k + 1], (size_t)(--g_group_n - k) * sizeof g_group[0]);
}

/* The game's pick radius for statues (0xeab75): u / 2, u = max(viewFt / 100, 1). */
static double pick_radius(double ft_per_px)
{
    double view_ft = 0;
    int h = 0;
    double u = ingame_view(&view_ft, &h) && view_ft * 0.01 > 1 ? view_ft * 0.01 : 1;
    double r = u / 2, min = PICK_MIN_PX * ft_per_px;
    return r > min ? r : min;
}

/* The nearest statue within r of (x, z), or -1. */
static int statue_near(double x, double z, double r)
{
    int n = game_statue_count(), best = -1;
    double best_d = r;
    for (int i = 0; i < n; i++) {
        statue_pose p;
        if (!game_statue_pose(i, &p))
            continue;
        double d = hypot(p.x - x, p.z - z);
        if (d <= best_d)
            best = i, best_d = d;
    }
    return best;
}

/* ── mouse ── */

static double g_ft_per_px;

int statuetool_dragging(void) { return g_box.on || g_move.on; }

/* A plain press on a group statue: remember every member's pose, so the drag can offset them all from there. */
static int move_begin(double x, double z)
{
    const char *why = game_statue_editable();
    if (why) {
        say("%s", why);
        return 1; /* still ours: the game would move just the one statue */
    }
    for (int k = 0; k < g_group_n; k++)
        if (!game_statue_pose(g_group[k], &g_move.start[k])) {
            say("A statue in the group isn't there any more. Clear the group and pick again.");
            return 1;
        }
    g_move.on = 1, g_move.wrote = 0, g_move.x0 = x, g_move.z0 = z;
    return 1;
}

/* A plain press the game gets: on the next frame, if the game picked a statue near it, its texture name is shown. */
static struct {
    int on;
    double x, z, r;
} g_pick;

int statuetool_down(double x, double z, int ctrl)
{
    if (!statuetool_active())
        return 0;
    double view_ft = 0;
    int h = 0;
    g_ft_per_px = ingame_view(&view_ft, &h) && h > 0 ? view_ft / h : 1;
    double r = pick_radius(g_ft_per_px);
    int hit = statue_near(x, z, r);
    if (!ctrl) {
        if (hit >= 0 && in_group(hit) >= 0)
            return move_begin(x, z);
        g_pick.on = 1, g_pick.x = x, g_pick.z = z, g_pick.r = r;
        return 0;
    }
    if (hit < 0) {
        g_box.on = 1, g_box.x0 = g_box.x1 = x, g_box.z0 = g_box.z1 = z;
        return 1;
    }
    static int members[MAX_GROUP];
    int piece = statuelinks_piece(hit);
    int parts = piece >= 0 ? statuelinks_members(piece, members, MAX_GROUP) : 0;
    if (parts > 1) {
        int leaving = in_group(hit) >= 0;
        for (int k = 0; k < parts; k++) {
            if (leaving)
                group_remove(members[k]);
            else
                group_add(members[k]);
        }
        g_list_n = game_statue_count();
        say("%s (%d linked statues) %s the group: %d in it.", statuelinks_name(piece), parts, leaving ? "out of" : "in",
            g_group_n);
        return 1;
    }
    if (in_group(hit) >= 0) {
        group_remove(hit);
        say("Statue %d out of the group: %d in it.", hit + 1, g_group_n);
        return 1;
    }
    int sel = game_statue_selected(), joined = 0;
    if (g_group_n == 0 && sel >= 0 && sel != hit)
        group_add(sel), joined = 1;
    group_add(hit);
    g_list_n = game_statue_count();
    if (joined)
        say("Statues %d and %d (the game's selection) in the group: %d in it.", hit + 1, sel + 1, g_group_n);
    else
        say("Statue %d in the group: %d in it.", hit + 1, g_group_n);
    return 1;
}

void statuetool_move(double x, double z)
{
    if (g_box.on)
        g_box.x1 = x, g_box.z1 = z;
    if (!g_move.on)
        return;
    double dx = x - g_move.x0, dz = z - g_move.z0;
    for (int k = 0; k < g_group_n; k++) {
        statue_pose p = g_move.start[k];
        p.x = (float)(p.x + dx), p.z = (float)(p.z + dz);
        const char *why = game_statue_set_pose(g_group[k], &p);
        if (why) {
            /* Stop where it is; the release still records what was moved, so it can be undone. */
            say("%s", why);
            g_move.on = 0;
            break;
        }
        g_move.wrote = 1;
    }
}

/* The release after a group move: one undo step for every statue that ended up somewhere else. */
static void move_end(void)
{
    g_move.on = 0;
    if (!g_move.wrote)
        return; /* a click without a drag: nothing moved */
    undo_step u;
    if (!undo_alloc(&u, "Move", g_group_n)) {
        say("Moved the group, but there was no memory to keep its undo.");
        return;
    }
    for (int k = 0; k < g_group_n; k++) {
        statue_pose now;
        if (!game_statue_pose(g_group[k], &now) ||
            (now.x == g_move.start[k].x && now.z == g_move.start[k].z && now.rot == g_move.start[k].rot))
            continue;
        u.index[u.n] = g_group[k], u.before[u.n] = g_move.start[k], u.after[u.n] = now;
        u.n++;
    }
    if (u.n == 0) {
        undo_free(&u);
        return;
    }
    undo_push(&u);
    say("Moved %d statue%s. Ctrl+Z undoes it.", u.n, u.n == 1 ? "" : "s");
}

void statuetool_up(void)
{
    if (g_move.on || g_move.wrote) {
        move_end();
        g_move.wrote = 0;
        return;
    }
    if (!g_box.on)
        return;
    g_box.on = 0;
    double x0 = fmin(g_box.x0, g_box.x1), x1 = fmax(g_box.x0, g_box.x1);
    double z0 = fmin(g_box.z0, g_box.z1), z1 = fmax(g_box.z0, g_box.z1);
    int n = game_statue_count(), added = 0, before = g_group_n;
    for (int i = 0; i < n; i++) {
        statue_pose p;
        if (game_statue_pose(i, &p) && p.x >= x0 && p.x <= x1 && p.z >= z0 && p.z <= z1 && in_group(i) < 0)
            group_add(i), added++;
    }
    /* A linked piece comes whole, even with only some of its parts in the box. */
    static int members[MAX_GROUP];
    for (int k = before; k < g_group_n; k++) {
        int parts = statuelinks_members(statuelinks_piece(g_group[k]), members, MAX_GROUP);
        for (int q = 0; q < parts; q++)
            if (in_group(members[q]) < 0)
                group_add(members[q]), added++;
    }
    g_list_n = n;
    if (g_group_n == before && !added)
        say("No statues in that box. Ctrl+drag around their centres.");
    else
        say("%d statue%s added: %d in the group.", added, added == 1 ? "" : "s", g_group_n);
}

/* ── the edits ── */

#define PI_D 3.14159265358979323846

/* A turn into (-pi, pi], minus 4 pi again for a statue that stays upright (rot <= -2 pi), as the game's snap leaves it. */
static float wrap_turn(double angle, float old_rot)
{
    double a = remainder(angle, 2 * PI_D);
    if (old_rot <= -2 * PI_D)
        a -= 4 * PI_D;
    return (float)a;
}

/* One linked piece snapped as a rigid thing: its middle goes where the game's Snap to Gradient would put a statue standing
   there, turned as its first part is; every part then turns by that same amount about the middle and moves with it. A
   part whose rot means "don't turn" (3.25 or more) keeps its rot but still swings round with the rest. */
static const char *snap_piece(const int *idx, int n, undo_step *u)
{
    static statue_pose was[MAX_GROUP];
    double cx = 0, cz = 0;
    for (int k = 0; k < n; k++) {
        if (!game_statue_pose(idx[k], &was[k]))
            return "A statue in the group isn't there any more.";
        cx += was[k].x, cz += was[k].z;
    }
    cx /= n, cz /= n;
    float nx, nz, nrot, r0 = was[0].rot;
    const char *why = game_snap_point((float)cx, (float)cz, r0, &nx, &nz, &nrot);
    if (why)
        return why;
    double turn = r0 < 3.25f ? remainder((double)nrot - r0, 2 * PI_D) : 0, c = cos(turn), s = sin(turn);
    for (int k = 0; k < n; k++) {
        /* Turned the way a statue's rot turns it: local +x points along (cos rot, -sin rot). */
        double ox = was[k].x - cx, oz = was[k].z - cz;
        statue_pose p = was[k];
        p.x = (float)(nx + ox * c + oz * s);
        p.z = (float)(nz - ox * s + oz * c);
        if (was[k].rot < 3.25f && turn != 0)
            p.rot = wrap_turn((double)was[k].rot + turn, was[k].rot);
        if ((why = game_statue_set_pose(idx[k], &p)) != NULL)
            return why;
        u->index[u->n] = idx[k], u->before[u->n] = was[k];
        game_statue_pose(idx[k], &u->after[u->n]);
        u->n++;
    }
    return NULL;
}

static void snap_group(void)
{
    if (g_group_n == 0) {
        say("Ctrl+click statues (or Ctrl+drag a box) to make a group first.");
        return;
    }
    const char *why = game_statue_editable();
    if (!why && game_selected_gradient() < 0)
        why = "Pick a gradient first (in Gradient Placement or Edit).";
    if (why) {
        say("%s", why);
        return;
    }
    undo_step u;
    if (!undo_alloc(&u, "Snap", g_group_n)) {
        say("Out of memory.");
        return;
    }
    static char done[MAX_GROUP];
    static int piece_idx[MAX_GROUP];
    memset(done, 0, sizeof done);
    int failed = 0, pieces = 0;
    const char *first_why = NULL;
    for (int k = 0; k < g_group_n; k++) {
        if (done[k])
            continue;
        int i = g_group[k], piece = statuelinks_piece(i), n = 0;
        /* The group's statues in the same linked piece go together; any other statue snaps alone, as the game does. */
        for (int q = k; q < g_group_n; q++)
            if (!done[q] && (q == k || (piece >= 0 && statuelinks_piece(g_group[q]) == piece)))
                piece_idx[n++] = g_group[q], done[q] = 1;
        if (n > 1) {
            int before = u.n;
            const char *err = snap_piece(piece_idx, n, &u);
            if (err) {
                if (!first_why)
                    first_why = err;
                failed += n - (u.n - before);
            } else {
                pieces++;
            }
            continue;
        }
        statue_pose was;
        if (!game_statue_pose(i, &was)) {
            failed++;
            continue;
        }
        const char *err = game_statue_snap(i);
        if (err) {
            if (!first_why)
                first_why = err;
            failed++;
            continue;
        }
        u.index[u.n] = i, u.before[u.n] = was;
        game_statue_pose(i, &u.after[u.n]);
        u.n++;
    }
    if (u.n == 0) {
        undo_free(&u);
        say("%s", first_why ? first_why : "None of the group's statues could be snapped.");
        return;
    }
    undo_push(&u);
    char as_pieces[64] = "";
    if (pieces)
        snprintf(as_pieces, sizeof as_pieces, ", %d linked piece%s kept whole", pieces, pieces == 1 ? "" : "s");
    if (failed)
        say("Snapped %d statues to the gradient%s; %d couldn't be (%s). Ctrl+Z undoes it.", u.n, as_pieces, failed,
            first_why ? first_why : "not there any more");
    else
        say("Snapped %d statue%s to the gradient%s. Ctrl+Z undoes it.", u.n, u.n == 1 ? "" : "s", as_pieces);
}

/* Duplicate Group: a copy of every group statue (same object, turn and height above the ground), moved by (dx, dz), added
   at the end of the list with the game's own add. The group becomes the copies, so a drag places them. */
static void duplicate_group(double dx, double dz)
{
    if (g_group_n == 0) {
        say("Ctrl+click statues (or Ctrl+drag a box) to make a group first.");
        return;
    }
    if (statuetool_dragging()) {
        say("Let go of the mouse button first.");
        return;
    }
    const char *why = game_statue_editable();
    if (!why && !game_statue_copies_known())
        why = "This game version's statue copy isn't known to the mod.";
    if (why) {
        say("%s", why);
        return;
    }
    undo_step u;
    if (!undo_alloc(&u, "Copy", g_group_n)) {
        say("Out of memory.");
        return;
    }
    u.copy_base = game_statue_count();
    const char *first_why = NULL;
    for (int k = 0; k < g_group_n; k++) {
        statue_pose p;
        int got = -1;
        if (!game_statue_pose(g_group[k], &p)) {
            first_why = "A statue in the group isn't there any more.";
            break;
        }
        first_why = game_statue_copy(g_group[k], (float)(p.x + dx), (float)(p.z + dz), p.rot, &got);
        if (first_why || got != u.copy_base + u.n)
            break; /* the copies must be one run at the end, so the undo can take them off */
        u.index[u.n] = got;
        game_statue_pose(got, &u.after[u.n]);
        u.n++;
    }
    if (u.n)
        game_statues_changed();
    g_list_n = game_statue_count();
    if (u.n == 0) {
        undo_free(&u);
        say("%s", first_why ? first_why : "The game didn't add the copies.");
        return;
    }
    int asked = g_group_n;
    statuelinks_copied(g_group, u.index, u.n); /* g_group[k] was copied to u.index[k] */
    memcpy(g_group, u.index, sizeof(int) * u.n);
    g_group_n = u.n;
    undo_push(&u);
    if (u.n < asked)
        say("Copied %d of %d statues (%s). The group is the copies; Ctrl+Z takes them off.", u.n, asked,
            first_why ? first_why : "the game stopped");
    else
        say("Copied %d statue%s. The group is now the copies: drag a gold one to place them. Ctrl+Z takes them off.", u.n,
            u.n == 1 ? "" : "s");
}

/* ── resizing (statuescale.c makes the files; the statue is pointed at them, keeping its index) ── */

/* What Scale works on: the group, or the game's selected statue when there's no group. */
static int scale_targets(const int **list)
{
    static int one;
    if (g_group_n) {
        *list = g_group;
        return g_group_n;
    }
    one = game_statue_selected();
    *list = &one;
    return one >= 0;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* by: times the present size (1.1 = 10% bigger), or 0 for back to the real size. */
static void scale_statues(double by)
{
    const int *list;
    int n = scale_targets(&list);
    if (!n) {
        say("Click a statue (or make a group) to resize first.");
        return;
    }
    if (statuetool_dragging()) {
        say("Let go of the mouse button first.");
        return;
    }
    const char *why = game_statue_editable();
    if (!why && !game_statue_copies_known())
        why = "This game version's statue add isn't known to the mod.";
    if (why) {
        say("%s", why);
        return;
    }
    undo_step u;
    if (!undo_alloc(&u, "Size", n) || !(u.files = calloc((size_t)n * 3, sizeof(char *)))) {
        undo_free(&u);
        say("Out of memory.");
        return;
    }
    static char mesh[FILE_CAP], shape[FILE_CAP], new_mesh[FILE_CAP], new_shape[FILE_CAP];
    const char *first_why = NULL;
    int failed = 0, same = 0;
    double total = 1;
    for (int k = 0; k < n; k++) {
        int i = list[k];
        if (!game_statue_files(i, mesh, shape, FILE_CAP)) {
            failed++;
            continue;
        }
        double f = by > 0 ? by : 1 / statuescale_factor(mesh);
        why = statuescale_files(mesh, shape, f, new_mesh, new_shape, FILE_CAP, &total);
        if (!why && _stricmp(new_mesh, mesh) == 0 && _stricmp(new_shape, shape) == 0) {
            same++;
            continue;
        }
        if (!why)
            why = game_statue_set_files(i, new_mesh, new_shape[0] ? new_shape : NULL);
        if (why) {
            if (!first_why)
                first_why = why;
            failed++;
            continue;
        }
        u.index[u.n] = i;
        u.files[3 * u.n] = dup_str(mesh), u.files[3 * u.n + 1] = dup_str(shape), u.files[3 * u.n + 2] = dup_str(new_mesh);
        u.n++;
    }
    if (u.n)
        game_statues_changed();
    if (u.n == 0) {
        undo_free(&u);
        if (same)
            say(by > 0 ? "Already that size." : "Already at its real size.");
        else
            say("%s", first_why ? first_why : "None of them could be resized.");
        return;
    }
    undo_push(&u);
    char size[48];
    if (n == 1)
        snprintf(size, sizeof size, " (now %.4g%% of its real size)", total * 100);
    else
        size[0] = 0;
    if (failed)
        say("Resized %d statue%s%s; %d couldn't be: %s Ctrl+Z undoes it.", u.n, u.n == 1 ? "" : "s", size, failed,
            first_why ? first_why : "not there any more.");
    else if (by > 0)
        say("Resized %d statue%s by %.4g%%%s. Press s to save. Ctrl+Z undoes it.", u.n, u.n == 1 ? "" : "s", by * 100, size);
    else
        say("%d statue%s back to real size. Ctrl+Z undoes it.", u.n, u.n == 1 ? "" : "s");
}

static void undo_scale(undo_step *u)
{
    static char mesh[FILE_CAP], shape[FILE_CAP];
    int done = 0, changed = 0;
    for (int k = u->n - 1; k >= 0; k--) {
        /* Only a statue still on the files this step gave it. */
        if (!game_statue_files(u->index[k], mesh, shape, FILE_CAP) || !u->files[3 * k + 2] ||
            _stricmp(mesh, u->files[3 * k + 2]) != 0 || !u->files[3 * k]) {
            changed++;
            continue;
        }
        const char *old_shape = u->files[3 * k + 1] && u->files[3 * k + 1][0] ? u->files[3 * k + 1] : NULL;
        if (!game_statue_set_files(u->index[k], u->files[3 * k], old_shape))
            done++;
        else
            changed++;
    }
    if (done)
        game_statues_changed();
    if (changed)
        say("Size undone for %d statues; %d had changed since, so they were left.", done, changed);
    else
        say("Size undone (%d statue%s).%s", done, done == 1 ? "" : "s", g_undo_n > 1 ? " Ctrl+Z again for the one before." : "");
}

/* Undoing a Copy: the copies come off the end of the list, newest first. Only while the list is as the copy left it. */
static int undo_copy(undo_step *u)
{
    if (game_statue_count() != u->copy_base + u->n) {
        say("The statue list changed since the copy, so it can't be taken off.");
        return 0;
    }
    int done = 0;
    const char *why = NULL;
    for (int k = u->n - 1; k >= 0 && !why; k--)
        if (!(why = game_statue_drop_last()))
            done++;
    if (done)
        game_statues_changed();
    g_list_n = game_statue_count();
    /* The group was the copies (or some of them): what's left of it is whatever's still in the list. */
    for (int k = g_group_n - 1; k >= 0; k--)
        if (g_group[k] >= g_list_n)
            group_remove(g_group[k]);
    if (why)
        say("Took off %d of %d copies (%s).", done, u->n, why);
    else
        say("Copy undone (%d statue%s taken off).%s", done, done == 1 ? "" : "s", g_undo_n > 1 ? " Ctrl+Z again for the one before." : "");
    return 1;
}

static void undo(void)
{
    if (g_undo_n == 0) {
        say("Nothing of ours to undo.");
        return;
    }
    if (statuetool_dragging()) {
        say("Let go of the mouse button first.");
        return;
    }
    const char *why = game_statue_editable();
    if (why) {
        say("%s", why);
        return;
    }
    undo_step *u = &g_undo[g_undo_n - 1];
    if (strcmp(u->what, "Copy") == 0 || strcmp(u->what, "Size") == 0) {
        if (u->files)
            undo_scale(u);
        else
            undo_copy(u);
        undo_free(u);
        g_undo_n--;
        return;
    }
    int done = 0, moved = 0;
    for (int k = u->n - 1; k >= 0; k--) {
        statue_pose now;
        /* Only a statue still where the step left it: one moved since (the game's drag) is left alone. */
        if (!game_statue_pose(u->index[k], &now) || now.x != u->after[k].x || now.z != u->after[k].z ||
            now.rot != u->after[k].rot) {
            moved++;
            continue;
        }
        if (!game_statue_set_pose(u->index[k], &u->before[k]))
            done++;
    }
    const char *what = u->what;
    undo_free(u);
    g_undo_n--;
    if (moved)
        say("%s undone for %d statues; %d had moved since, so they were left.", what, done, moved);
    else
        say("%s undone (%d statue%s).%s", what, done, done == 1 ? "" : "s", g_undo_n ? " Ctrl+Z again for the one before." : "");
}

/* The group's centre: the middle of its statues' centres. */
static int group_centre(double *cx, double *cz)
{
    double sx = 0, sz = 0;
    int got = 0;
    for (int k = 0; k < g_group_n; k++) {
        statue_pose p;
        if (game_statue_pose(g_group[k], &p))
            sx += p.x, sz += p.z, got++;
    }
    if (!got)
        return 0;
    *cx = sx / got, *cz = sz / got;
    return 1;
}

int statuetool_key(int key, int ctrl, double mouse_x, double mouse_z)
{
    if (!statuetool_active())
        return 0;
    if (g_undo_n && (key == 26 || (ctrl && (key == 'z' || key == 'Z')))) {
        undo();
        return 1;
    }
    /* d with a group copies the group, centred on the mouse (the game's d copies its one statue to the mouse). */
    if (key == 'd' && !ctrl && g_group_n && game_statue_copies_known()) {
        double cx, cz;
        if (!group_centre(&cx, &cz))
            say("The group's statues aren't there any more. Clear the group and pick again.");
        else
            duplicate_group(mouse_x - cx, mouse_z - cz);
        return 1;
    }
    /* g with a group: Snap Group to Gradient (s is the game's save, and the game's handler has no g). */
    if ((key == 'g' || key == 'G') && !ctrl && g_group_n) {
        snap_group();
        return 1;
    }
    return 0;
}

/* ── each frame ── */

void statuetool_frame(void)
{
    if (game_editor_mode() != MODE_STATUES || !game_statues_known()) {
        g_box.on = g_move.on = g_move.wrote = 0;
        return;
    }
    statuelinks_frame();
    int n = game_statue_count();
    if (g_list_n >= 0 && n != g_list_n) {
        /* A statue was added or deleted: the indices no longer mean the same statues. */
        if (g_group_n)
            say("The statue list changed, so the group was cleared.");
        g_group_n = 0;
        g_move.on = g_move.wrote = 0;
        undo_clear();
    }
    g_list_n = n;
    if (g_pick.on) {
        /* The game has had the press: say what it picked (only a statue by the press, not an older selection). */
        g_pick.on = 0;
        int sel = game_statue_selected();
        statue_pose p;
        if (sel >= 0 && game_statue_pose(sel, &p) && hypot(p.x - g_pick.x, p.z - g_pick.z) <= g_pick.r * 1.01) {
            const char *name = game_statue_texture(sel);
            say("Statue %d of %d: %s", sel + 1, n, name ? name : "no texture");
        }
    }
}

/* ── the panel ── */

int statuetool_rows(panel_row *rows, int max, int *kinds, int *args)
{
    if (game_editor_mode() != MODE_STATUES || curvetool_mode() != CURVE_MODE_OFF)
        return 0;
    int n = 0;
    panel_row *r = NULL;
#define ROW(kind_, rowk, label_) \
    (n < max ? (memset(r = &rows[n], 0, sizeof *r), kinds[n] = (kind_), args[n] = 0, r->kind = (rowk), r->label = (label_), n++, 1) \
             : (r = NULL, 0))
    ROW(ST_HEADER, ROWK_HEADER, "STATUE GROUP");
    if (!game_statues_known()) {
        if (ROW(ST_HINT, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "Not ported to this game build yet");
        return n;
    }
    if (ROW(ST_LABEL, ROWK_LABEL, ""))
        snprintf(r->text, sizeof r->text, g_group_n == 1 ? "1 statue in the group" : "%d statues in the group", g_group_n);
    int linked = statuelinks_count();
    if (linked && ROW(ST_HINT, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, linked == 1 ? "1 linked piece on this track" : "%d linked pieces on this track", linked);
    if (g_group_n) {
        ROW(ST_SNAP, ROWK_BUTTON, "Snap Group to Gradient");
        if (game_statue_copies_known())
            ROW(ST_COPY, ROWK_BUTTON, "Duplicate Group");
        ROW(ST_CLEAR, ROWK_BUTTON, "Clear Group");
    }
    if (g_undo_n) {
        static char label[48];
        const char *what = g_undo[g_undo_n - 1].what;
        if (g_undo_n > 1)
            snprintf(label, sizeof label, "Undo Statue %s (%d)", what, g_undo_n);
        else
            snprintf(label, sizeof label, "Undo Statue %s", what);
        ROW(ST_UNDO, ROWK_BUTTON, label);
    }
    if (ROW(ST_HINT, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, linked ? "Ctrl+click: add or drop a statue (or piece)" : "Ctrl+click: add or drop a statue");
    if (ROW(ST_HINT, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "Ctrl+drag: box around statues");
    if (g_group_n && ROW(ST_HINT, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "Drag a gold statue: move the group");
    if (g_group_n && ROW(ST_HINT, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "g: snap the group to the gradient");
    if (g_group_n && game_statue_copies_known() && ROW(ST_HINT, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "d: copy the group to the mouse");
    if (g_group_n && game_selected_gradient() < 0 && ROW(ST_HINT, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "Pick a gradient in Placement first");

    /* Resizing: the group, or the game's selected statue without one. */
    const int *list = NULL;
    int targets = game_statue_copies_known() ? scale_targets(&list) : 0;
    ROW(ST_HEADER, ROWK_HEADER, "STATUE SIZE");
    if (!targets) {
        if (ROW(ST_HINT, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, game_statue_copies_known() ? "Click a statue or make a group" : "Not ported to this game build yet");
    } else {
        /* The first one's size against its original; read from disk only when its mesh changes. */
        static char seen[FILE_CAP], shape[FILE_CAP], mesh[FILE_CAP];
        static double factor = 1;
        if (game_statue_files(list[0], mesh, shape, FILE_CAP) && strcmp(mesh, seen) != 0) {
            memcpy(seen, mesh, sizeof seen);
            factor = mesh[0] ? statuescale_factor(mesh) : 1;
        }
        if (ROW(ST_LABEL, ROWK_LABEL, ""))
            snprintf(r->text, sizeof r->text, "%s%.4g%% of real size", targets > 1 ? "First: " : "Now ", factor * 100);
        if (ROW(ST_SCALE_BY, ROWK_SLIDER, "Scale by (%)"))
            r->value = g_scale_pct, r->min = 10, r->max = 300, r->step = 5;
        ROW(ST_SCALE, ROWK_BUTTON, targets > 1 ? "Scale Group" : "Scale Statue");
        ROW(ST_REAL_SIZE, ROWK_BUTTON, "Real Size");
        if (ROW(ST_HINT, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "Each about its own centre");
    }
#undef ROW
    return n;
}

void statuetool_press(int kind, int arg)
{
    (void)arg;
    switch (kind) {
    case ST_SNAP: snap_group(); break;
    case ST_COPY: duplicate_group(COPY_SIDE_FT, 0); break; /* the mouse is on the panel: beside the group instead */
    case ST_CLEAR:
        if (statuetool_dragging())
            break;
        g_group_n = 0;
        say("Group cleared.");
        break;
    case ST_UNDO: undo(); break;
    case ST_SCALE: scale_statues(g_scale_pct / 100); break;
    case ST_REAL_SIZE: scale_statues(0); break;
    }
}

void statuetool_slide(int kind, double value)
{
    if (kind == ST_SCALE_BY && value >= 10 && value <= 300)
        g_scale_pct = value;
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

void statuetool_scene(scene *out, double mouse_x, double mouse_z, double ft_per_px)
{
    (void)mouse_x, (void)mouse_z;
    if (!statuetool_active() || !(ft_per_px > 0))
        return;
    static const float gold[4] = {1.0f, 0.824f, 0.290f, 0.95f}, box[4] = {1.0f, 1.0f, 1.0f, 0.8f};
    double view_ft = 0;
    int h = 0;
    if (!ingame_view(&view_ft, &h))
        return;
    double u = view_ft * 0.01 > 1 ? view_ft * 0.01 : 1;
    for (int k = 0; k < g_group_n; k++) {
        statue_pose p;
        if (!game_statue_pose(g_group[k], &p))
            continue;
        scene_item *ring = add(out, SCENE_RING, gold, 0);
        if (ring)
            ring->x = p.x, ring->z = p.z, ring->size = (float)(9 * ft_per_px / u), ring->width = (float)(2.5 * ft_per_px / u);
    }
    if (g_box.on) {
        scene_item *it = add(out, SCENE_LINE, box, 4);
        if (it) {
            it->closed = 1;
            float (*pt)[2] = &out->pt[it->first];
            pt[0][0] = (float)g_box.x0, pt[0][1] = (float)g_box.z0;
            pt[1][0] = (float)g_box.x1, pt[1][1] = (float)g_box.z0;
            pt[2][0] = (float)g_box.x1, pt[2][1] = (float)g_box.z1;
            pt[3][0] = (float)g_box.x0, pt[3][1] = (float)g_box.z1;
        }
    }
}
