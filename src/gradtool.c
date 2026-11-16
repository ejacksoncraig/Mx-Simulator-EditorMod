/* Tools for the game's own gradients, in our panel while the game's tools are in use (game thread only):
   - Gradient Placement and Edit: Delete (or Del) with Undo Delete, Export to a file, the Saved Gradients list, Place On
     Map (the next click puts its A there, same direction and length) and Put Back Where It Was. Gradient Length
     (Locked / Extend / Stretch) lives here too, so every row about the picked gradient is in one group. Berm Preview's
     button is too; bermpreview.c has the rest of it.
   - Gradient Placement: Shift+drag moves a gradient or curve whole, and the gradient group (Ctrl+click, Ctrl+drag a
     box) holds gradients and our curves together so Shift+drag on any of them moves them all as one.
   - Gradient Radial Paint and Add: how far the brush sweeps around A (the game's own is 180 degrees, centred on B,
     running straight past the edges), drawn on the map as a wedge. ingame.c swaps the game's radial helper for one
     that follows it.
   Saved gradients are one JSON file each in %LOCALAPPDATA%\MX Simulator\mxtools\gradients, newest first. */
#include "curvetool.h"
#include "json.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODE_RADIAL_PAINT 7
#define MODE_RADIAL_ADD 8
#define MODE_PLACEMENT 9
#define MODE_EDIT 10

enum {
    GT_HEADER = GRADTOOL_ROW, GT_HINT, GT_LENGTH, GT_DELETE, GT_UNDO_DELETE, GT_EXPORT, GT_SAVED, GT_SAVED_ITEM, GT_PLACE,
    GT_CANCEL_PLACE, GT_PUT_BACK, GT_FOLDER, GT_SWEEP, GT_SWEEP_SIDE, GT_BERM_PREVIEW, GT_UNDO_MOVE, GT_CLEAR_GROUP
};

static char g_say[192];
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_say, sizeof g_say, fmt, ap);
    va_end(ap);
    game_status(g_say);
    mx_log("gradients: %s", g_say);
}

/* ── Gradient Length (dragging B of a shaped gradient, game.c) ── */
static const char *const LENGTH_OPTIONS[] = {"Locked", "Extend", "Stretch"};
static int g_gradient_length = GRADIENT_LENGTH_LOCKED;
int curvetool_gradient_length(void) { return g_gradient_length; }

/* ── Delete and Undo Delete ──
   After a delete the next gradient is selected (the game needs one selected to pick at all, game.c), so Del twice
   deletes two. Undo Delete therefore keeps the last UNDO_MAX deletions on this track, newest last. */
enum { UNDO_MAX = 16 };
static gradient_copy g_deleted[UNDO_MAX];
static int g_deleted_at[UNDO_MAX];
static int g_undo_count;
static char g_deleted_track[MAX_PATH];

static int same_track(const char *track)
{
    char now[MAX_PATH];
    return game_track(now, sizeof now) && !strcmp(now, track);
}

static void delete_selected(void)
{
    int index = game_selected_gradient();
    if (index < 0) {
        say("Pick a gradient first: click its line in Gradient Placement.");
        return;
    }
    static gradient_copy copy;
    if (!game_gradient_copy(index, &copy)) {
        say("That gradient couldn't be read.");
        return;
    }
    const char *why = game_gradient_delete(index);
    if (why) {
        say("%s", why);
        return;
    }
    char track[MAX_PATH];
    if (!game_track(track, sizeof track))
        track[0] = 0;
    if (strcmp(track, g_deleted_track))
        g_undo_count = 0; /* another track's deletions can't come back here */
    snprintf(g_deleted_track, sizeof g_deleted_track, "%s", track);
    if (g_undo_count == UNDO_MAX) {
        memmove(&g_deleted[0], &g_deleted[1], (UNDO_MAX - 1) * sizeof g_deleted[0]);
        memmove(&g_deleted_at[0], &g_deleted_at[1], (UNDO_MAX - 1) * sizeof g_deleted_at[0]);
        g_undo_count--;
    }
    g_deleted[g_undo_count] = copy, g_deleted_at[g_undo_count] = index, g_undo_count++;
    curvetool_gradients_changed();
    int now = game_selected_gradient();
    if (now >= 0)
        say("Deleted gradient %d (the ground stays). Gradient %d is picked now. Undo Delete brings it back.", index + 1,
            now + 1);
    else
        say("Deleted gradient %d (the ground stays). Undo Delete brings it back.", index + 1);
}

static void undo_delete(void)
{
    if (!g_undo_count || !same_track(g_deleted_track))
        return;
    int k = g_undo_count - 1;
    const char *why = game_gradient_insert(g_deleted_at[k], &g_deleted[k]);
    if (why) {
        say("%s", why);
        return;
    }
    g_undo_count--;
    curvetool_gradients_changed();
    say("Gradient %d is back.%s", g_deleted_at[k] + 1, g_undo_count ? " Undo Delete again for the one before." : "");
}

/* ── saved gradient files ── */
enum { SAVED_MAX = 64 };
typedef struct {
    wchar_t path[MAX_PATH];
    char name[64];
    FILETIME time;
} saved_file;
static saved_file g_saved[SAVED_MAX];
static int g_saved_count, g_saved_choice, g_list_open;
static int g_frames;

static int saved_dir(wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH];
    if (!mx_data_dir(dir, MAX_PATH) || swprintf(out, cap, L"%ls\\gradients", dir) <= 0)
        return 0;
    CreateDirectoryW(out, NULL);
    return 1;
}

static int newer_first(const void *a, const void *b)
{
    return -CompareFileTime(&((const saved_file *)a)->time, &((const saved_file *)b)->time);
}

/* The folder's .json files, newest first; the chosen one stays chosen if it's still there. */
static void scan(void)
{
    wchar_t dir[MAX_PATH], pattern[MAX_PATH], chosen[MAX_PATH] = L"";
    if (g_saved_choice >= 0 && g_saved_choice < g_saved_count)
        wcscpy_s(chosen, MAX_PATH, g_saved[g_saved_choice].path);
    g_saved_count = 0;
    if (!saved_dir(dir, MAX_PATH) || swprintf(pattern, MAX_PATH, L"%ls\\*.json", dir) <= 0)
        return;
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(pattern, &found);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (g_saved_count >= SAVED_MAX)
            break;
        saved_file *f = &g_saved[g_saved_count];
        if (swprintf(f->path, MAX_PATH, L"%ls\\%ls", dir, found.cFileName) <= 0)
            continue;
        wchar_t stem[MAX_PATH];
        wcscpy_s(stem, MAX_PATH, found.cFileName);
        wchar_t *dot = wcsrchr(stem, L'.');
        if (dot)
            *dot = 0;
        if (!WideCharToMultiByte(CP_ACP, 0, stem, -1, f->name, sizeof f->name, NULL, NULL))
            snprintf(f->name, sizeof f->name, "(a gradient)");
        f->time = found.ftLastWriteTime;
        g_saved_count++;
    } while (FindNextFileW(h, &found));
    FindClose(h);
    qsort(g_saved, (size_t)g_saved_count, sizeof g_saved[0], newer_first);
    g_saved_choice = 0;
    for (int i = 0; i < g_saved_count; i++)
        if (!wcscmp(g_saved[i].path, chosen))
            g_saved_choice = i;
}

static int write_file(const wchar_t *path, const char *text, size_t n)
{
    wchar_t tmp[MAX_PATH];
    if (swprintf(tmp, MAX_PATH, L"%ls.tmp", path) <= 0)
        return 0;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, (DWORD)n, &put, NULL) && put == n;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    ok = ok && MoveFileExW(tmp, path, MOVEFILE_WRITE_THROUGH); /* never over another file */
    if (!ok)
        DeleteFileW(tmp);
    return ok;
}

/* {"format": "mxtools-gradient", "version": 1, "track", "gradient" (1-based), "a": [x, z], "b": [x, z],
    "points": [{"type", "s", "h"}]}: a profile point as in profile-sections.json. */
static size_t gradient_json(const gradient_copy *g, const char *track, int number, char *out, size_t cap)
{
    size_t n = 0;
#define PUT(...)                                                        \
    do {                                                                \
        int wrote_ = snprintf(out + n, cap - n, __VA_ARGS__);           \
        if (wrote_ < 0 || n + (size_t)wrote_ >= cap)                    \
            return 0;                                                   \
        n += (size_t)wrote_;                                            \
    } while (0)
    PUT("{\"format\": \"mxtools-gradient\", \"version\": 1, \"track\": \"");
    for (const char *c = track; *c; c++)
        PUT(*c == '"' || *c == '\\' ? "\\%c" : "%c", *c);
    PUT("\", \"gradient\": %d,\n \"a\": [%.9g, %.9g], \"b\": [%.9g, %.9g],\n \"points\": [", number, g->ax, g->az, g->bx,
        g->bz);
    for (int i = 0; i < g->points; i++)
        PUT("%s{\"type\": %d, \"s\": %.9g, \"h\": %.9g}", i ? ",\n  " : "\n  ", g->p[i].type, g->p[i].s, g->p[i].h);
    PUT("]}\n");
#undef PUT
    return n;
}

static void export_selected(void)
{
    int index = game_selected_gradient();
    static gradient_copy g;
    if (index < 0 || !game_gradient_copy(index, &g)) {
        say("Pick a gradient first: click its line in Gradient Placement.");
        return;
    }
    char track[MAX_PATH];
    if (!game_track(track, sizeof track))
        snprintf(track, sizeof track, "track");
    static char text[CURVE_MAX_POINTS * 80 + 1024];
    size_t n = gradient_json(&g, track, index + 1, text, sizeof text);
    wchar_t dir[MAX_PATH], wtrack[MAX_PATH], path[MAX_PATH];
    if (!n || !saved_dir(dir, MAX_PATH) || !MultiByteToWideChar(CP_ACP, 0, track, -1, wtrack, MAX_PATH)) {
        say("Couldn't export the gradient.");
        return;
    }
    for (wchar_t *c = wtrack; *c; c++)
        if (wcschr(L"\\/:*?\"<>|", *c))
            *c = L'-';
    /* Named so it's easy to find: the track, the gradient's number, its length. Never over an older export. */
    double length = g.p[g.points - 1].s;
    int written = 0;
    for (int copy = 1; copy < 100 && !written; copy++) {
        wchar_t extra[16] = L"";
        if (copy > 1)
            swprintf(extra, 16, L" (%d)", copy);
        if (swprintf(path, MAX_PATH, L"%ls\\%ls #%d, %.0f ft%ls.json", dir, wtrack, index + 1, length, extra) <= 0)
            break;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            written = write_file(path, text, n);
    }
    if (!written) {
        say("Couldn't write the gradient's file.");
        return;
    }
    scan();
    for (int i = 0; i < g_saved_count; i++)
        if (!wcscmp(g_saved[i].path, path))
            g_saved_choice = i;
    say("Exported as \"%s\" (mxtools\\gradients). Place it on any track from Saved Gradients.",
        g_saved_choice < g_saved_count ? g_saved[g_saved_choice].name : "");
}

static int read_gradient_file(const wchar_t *path, gradient_copy *g, const char **why)
{
    *why = "That file isn't a gradient this mod can read.";
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        *why = "That gradient's file is gone (deleted or renamed?).";
        return 0;
    }
    static char text[1 << 20];
    DWORD got = 0;
    ReadFile(f, text, sizeof text, &got, NULL);
    CloseHandle(f);
    memset(g, 0, sizeof *g);
    json_reader r = {text, text + got, NULL};
    char key[16];
    int first = 1, have_a = 0, have_b = 0;
    while (json_next_key(&r, key, sizeof key, &first)) {
        if (!strcmp(key, "a") || !strcmp(key, "b")) {
            float xz[2];
            int count = 0;
            if (!json_numbers(&r, xz, 2, &count) || count != 2)
                return 0;
            if (key[0] == 'a')
                g->ax = xz[0], g->az = xz[1], have_a = 1;
            else
                g->bx = xz[0], g->bz = xz[1], have_b = 1;
        } else if (!strcmp(key, "points")) {
            int each = 1;
            while (json_next_item(&r, &each)) {
                profile_point p = {0, 0, 0};
                double type = 0;
                char k[8];
                int fk = 1;
                while (json_next_key(&r, k, sizeof k, &fk)) {
                    double *at = !strcmp(k, "type") ? &type : !strcmp(k, "s") ? &p.s : !strcmp(k, "h") ? &p.h : NULL;
                    if (at ? !json_number(&r, at) : !json_skip(&r, 1))
                        return 0;
                }
                if (r.error)
                    return 0;
                p.type = type == 1;
                if (g->points < CURVE_MAX_POINTS)
                    g->p[g->points++] = p;
            }
        } else if (!json_skip(&r, 1)) {
            return 0;
        }
        if (r.error)
            return 0;
    }
    return !r.error && have_a && have_b && g->points >= 2;
}

/* ── placing a saved gradient ── */
static int g_placing;
static gradient_copy g_place;

int gradtool_placing(void) { return g_placing; }

static int load_chosen(gradient_copy *g)
{
    const char *why;
    if (g_saved_choice < 0 || g_saved_choice >= g_saved_count) {
        say("Export a gradient first; it shows up here.");
        return 0;
    }
    if (!read_gradient_file(g_saved[g_saved_choice].path, g, &why)) {
        say("%s", why);
        return 0;
    }
    return 1;
}

static void add_gradient(const gradient_copy *g, const char *how)
{
    int count = game_gradient_count();
    const char *why = count < 0 ? "The game's gradient list couldn't be read." : game_gradient_insert(count, g);
    if (why) {
        say("%s", why);
        return;
    }
    curvetool_gradients_changed();
    say("%s as gradient %d. Drag its ends to adjust; s saves the track.", how, count + 1);
}

/* A gradient already on the track with the same ends and profile (to the file's 9 digits), or -1. Put Back would
   only stack an invisible copy on it. */
static int same_spot(const gradient_copy *g)
{
    static gradient_copy have;
    int count = game_gradient_count();
    for (int i = 0; i < count; i++) {
        if (!game_gradient_copy(i, &have) || have.points != g->points || fabsf(have.ax - g->ax) > 1e-3f ||
            fabsf(have.az - g->az) > 1e-3f || fabsf(have.bx - g->bx) > 1e-3f || fabsf(have.bz - g->bz) > 1e-3f)
            continue;
        int k = 0;
        while (k < g->points && have.p[k].type == g->p[k].type && fabs(have.p[k].s - g->p[k].s) < 1e-3 &&
               fabs(have.p[k].h - g->p[k].h) < 1e-3)
            k++;
        if (k == g->points)
            return i;
    }
    return -1;
}

void gradtool_place(double x, double z)
{
    if (!g_placing)
        return;
    g_placing = 0;
    gradient_copy g = g_place;
    float dx = g.bx - g.ax, dz = g.bz - g.az;
    g.ax = (float)x, g.az = (float)z, g.bx = (float)x + dx, g.bz = (float)z + dz;
    add_gradient(&g, "Placed");
}


/* ── moving gradients and curves, one or a group (Placement) ──
   The game's own drags move A or B alone. Shift+press on a line (a gradient's, or one of our curves', the nearest
   within 8 px) grabs it whole: both ends (every point of a curve) move by the mouse's offset from the press, from where
   they were then, so nothing drifts. Profiles aren't touched, and the ground doesn't change.
   The group, like the statue group: Ctrl+press on a line puts it in or takes it out (with an empty group the game's
   selected gradient joins too), Ctrl+press on empty ground draws a box and everything whose middle is inside joins.
   Shift+press on a line in the group moves the whole group. Gradients and curves are indices, so the gradients leave
   the group when the game's list changes length (a gradient added or deleted) and the curves when ours are renumbered.
   Ctrl+Z in Placement puts the last 16 moves back, newest first, each one whole, for whatever is still where it left. */
enum { MEMBER_GRADIENT, MEMBER_CURVE };
enum { GROUP_MAX = 512 };
static struct {
    int n, kind[GROUP_MAX], index[GROUP_MAX];
    int gradients, curves_gen; /* what the indices were taken against */
    char track[MAX_PATH];
} g_group;
static struct {
    int on, n, kind[GROUP_MAX], index[GROUP_MAX];
    float ends[GROUP_MAX][4]; /* a gradient's A and B at the press; a curve's first point (x, z) */
    double px, pz, cdx, cdz;  /* the press, and how far the curves have been shifted so far */
} g_move;
static struct {
    int on;
    double x0, z0, x1, z1;
} g_box;
/* One move, undone whole: per member, where it was and where it went (a curve: its first point). */
typedef struct {
    int n, curves_gen, *kind;
    float (*from)[4], (*to)[4];
} gradient_move;
static gradient_move g_moves[UNDO_MAX];
static int g_move_n;
static char g_moves_track[MAX_PATH];

static void move_free(gradient_move *m)
{
    free(m->kind), free(m->from), free(m->to);
    memset(m, 0, sizeof *m);
}

static double seg_dist(double x, double z, double ax, double az, double bx, double bz)
{
    double dx = bx - ax, dz = bz - az, len2 = dx * dx + dz * dz;
    double t = len2 > 0 ? ((x - ax) * dx + (z - az) * dz) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypot(x - (ax + t * dx), z - (az + t * dz));
}

/* The gradient whose line is nearest (x, z) within reach ft, the selected one winning a tie. -1 if none. */
static int line_at(double x, double z, double reach, gradient_copy *g)
{
    int count = game_gradient_count(), selected = game_selected_gradient(), pick = -1;
    double best = reach;
    for (int i = 0; i < count; i++) {
        if (!game_gradient_copy(i, g))
            continue;
        double d = seg_dist(x, z, g->ax, g->az, g->bx, g->bz);
        if (d < best || (d == best && i == selected))
            best = d, pick = i;
    }
    return pick;
}

static void moves_follow_track(void)
{
    char track[MAX_PATH] = "";
    game_track(track, sizeof track);
    if (strcmp(track, g_moves_track)) {
        while (g_move_n)
            move_free(&g_moves[--g_move_n]);
        snprintf(g_moves_track, sizeof g_moves_track, "%s", track);
    }
}

/* The group's indices still mean the same things: another track empties it, a changed gradient count drops its
   gradients, renumbered curves drop its curves. */
static void group_follow(void)
{
    char track[MAX_PATH] = "";
    game_track(track, sizeof track);
    int gradients = game_gradient_count(), gen = curvetool_curves_gen();
    int new_track = strcmp(track, g_group.track) != 0;
    int drop_g = new_track || gradients != g_group.gradients, drop_c = new_track || gen != g_group.curves_gen;
    if (g_group.n && (drop_g || drop_c)) {
        int keep = 0, before = g_group.n;
        for (int k = 0; k < g_group.n; k++)
            if (!(g_group.kind[k] == MEMBER_GRADIENT ? drop_g : drop_c))
                g_group.kind[keep] = g_group.kind[k], g_group.index[keep] = g_group.index[k], keep++;
        g_group.n = keep;
        if (keep < before && !new_track)
            say(keep ? "The %s list changed, so %s left the group." : "The %s list changed, so the group was cleared.",
                drop_g ? "gradient" : "curve", drop_g ? "its gradients" : "its curves");
    }
    snprintf(g_group.track, sizeof g_group.track, "%s", track);
    g_group.gradients = gradients, g_group.curves_gen = gen;
}

static int group_has(int kind, int index)
{
    for (int k = 0; k < g_group.n; k++)
        if (g_group.kind[k] == kind && g_group.index[k] == index)
            return k;
    return -1;
}

static void group_add(int kind, int index)
{
    if (index >= 0 && group_has(kind, index) < 0 && g_group.n < GROUP_MAX)
        g_group.kind[g_group.n] = kind, g_group.index[g_group.n] = index, g_group.n++;
}

static void group_counts(int *gradients, int *curves)
{
    *gradients = *curves = 0;
    for (int k = 0; k < g_group.n; k++)
        (*(g_group.kind[k] == MEMBER_GRADIENT ? gradients : curves))++;
}

/* The line nearest (x, z) within reach ft, a gradient's or a curve's: 1 and *kind, *index, or 0. */
static int member_at(double x, double z, double reach, int *kind, int *index)
{
    static gradient_copy g;
    int gi = line_at(x, z, reach, &g);
    double gd = gi >= 0 && game_gradient_copy(gi, &g) ? seg_dist(x, z, g.ax, g.az, g.bx, g.bz) : 1e300, cd = 1e300;
    int ci = curvetool_curve_near(x, z, reach, &cd);
    if (gi < 0 && ci < 0)
        return 0;
    if (ci >= 0 && (gi < 0 || cd < gd))
        *kind = MEMBER_CURVE, *index = ci;
    else
        *kind = MEMBER_GRADIENT, *index = gi;
    return 1;
}

static const char *member_name(int kind, int index)
{
    static char name[48];
    snprintf(name, sizeof name, "%s %d", kind == MEMBER_GRADIENT ? "Gradient" : "Curve", index + 1);
    return name;
}

static void say_group(const char *what)
{
    int gradients, curves;
    group_counts(&gradients, &curves);
    say("%s: %d gradient%s and %d curve%s in the group.", what, gradients, gradients == 1 ? "" : "s", curves,
        curves == 1 ? "" : "s");
}

/* Where each mover is now, so the drag offsets them all from there. 0 (and why said) if one can't be moved. */
static int move_begin(int n, const int *kind, const int *index, double x, double z)
{
    static gradient_copy g;
    for (int k = 0; k < n; k++) {
        if (kind[k] == MEMBER_GRADIENT) {
            /* Writing the ends as they are checks the edit is allowed before anything moves. */
            const char *why = game_gradient_copy(index[k], &g) ? game_gradient_set_ends(index[k], g.ax, g.az, g.bx, g.bz)
                                                               : "A gradient in the group isn't there any more.";
            if (why) {
                say("%s", why);
                return 0;
            }
            g_move.ends[k][0] = g.ax, g_move.ends[k][1] = g.az, g_move.ends[k][2] = g.bx, g_move.ends[k][3] = g.bz;
        } else {
            double fx, fz;
            if (!curvetool_curve_first(index[k], &fx, &fz)) {
                say("A curve in the group isn't there any more.");
                return 0;
            }
            g_move.ends[k][0] = (float)fx, g_move.ends[k][1] = (float)fz, g_move.ends[k][2] = g_move.ends[k][3] = 0;
        }
        g_move.kind[k] = kind[k], g_move.index[k] = index[k];
    }
    moves_follow_track();
    g_move.on = 1, g_move.n = n, g_move.px = x, g_move.pz = z, g_move.cdx = g_move.cdz = 0;
    for (int k = 0; k < n; k++)
        if (kind[k] == MEMBER_CURVE) {
            curvetool_group_move_begin(); /* one curve undo step for the whole drag */
            break;
        }
    return 1;
}

int gradtool_down(double x, double z, double ft_per_px, int shift, int ctrl)
{
    if ((!shift && !ctrl) || game_editor_mode() != MODE_PLACEMENT || g_placing)
        return 0;
    group_follow();
    int kind = 0, index = -1, hit = member_at(x, z, 8 * ft_per_px, &kind, &index);
    if (ctrl) {
        if (!hit) {
            g_box.on = 1, g_box.x0 = g_box.x1 = x, g_box.z0 = g_box.z1 = z;
            return 1;
        }
        int k = group_has(kind, index);
        if (k >= 0) {
            memmove(&g_group.kind[k], &g_group.kind[k + 1], (size_t)(g_group.n - k - 1) * sizeof g_group.kind[0]);
            memmove(&g_group.index[k], &g_group.index[k + 1], (size_t)(g_group.n - k - 1) * sizeof g_group.index[0]);
            g_group.n--;
            say_group(member_name(kind, index));
            return 1;
        }
        int sel = game_selected_gradient();
        if (g_group.n == 0 && sel >= 0 && !(kind == MEMBER_GRADIENT && index == sel))
            group_add(MEMBER_GRADIENT, sel); /* "click one, Ctrl+click the rest", as with statues */
        group_add(kind, index);
        say_group(member_name(kind, index));
        return 1;
    }
    if (!hit)
        return 0;
    /* Shift: the whole group if the line is in it, else just that line. */
    int one_kind[1] = {kind}, one_index[1] = {index};
    int in = group_has(kind, index) >= 0;
    if (!move_begin(in ? g_group.n : 1, in ? g_group.kind : one_kind, in ? g_group.index : one_index, x, z))
        return 1; /* still ours: a Shift+click shouldn't fall through to the game's pick-and-drag of one end */
    if (kind == MEMBER_GRADIENT)
        game_gradient_select(index);
    if (in)
        say("Moving the group (%d): let go to drop it.", g_group.n);
    else
        say("Moving %s: let go to drop it.", member_name(kind, index));
    return 1;
}

int gradtool_dragging(void) { return g_move.on || g_box.on; }

void gradtool_move(double x, double z)
{
    if (g_box.on)
        g_box.x1 = x, g_box.z1 = z;
    if (!g_move.on)
        return;
    double dx = x - g_move.px, dz = z - g_move.pz;
    for (int k = 0; k < g_move.n; k++) {
        if (g_move.kind[k] != MEMBER_GRADIENT)
            continue;
        const float *e = g_move.ends[k];
        const char *why = game_gradient_set_ends(g_move.index[k], (float)(e[0] + dx), (float)(e[1] + dz), (float)(e[2] + dx),
                                                 (float)(e[3] + dz));
        if (why) {
            say("%s", why);
            g_move.on = 0; /* stop where it is; the release still records what moved, so it can be undone */
            return;
        }
    }
    /* Curves shift by what's left to go, so they end exactly at the mouse's offset. */
    double ddx = dx - g_move.cdx, ddz = dz - g_move.cdz;
    for (int k = 0; k < g_move.n; k++)
        if (g_move.kind[k] == MEMBER_CURVE)
            curvetool_curve_shift(g_move.index[k], ddx, ddz);
    g_move.cdx = dx, g_move.cdz = dz;
}

static void box_end(void)
{
    g_box.on = 0;
    double x0 = fmin(g_box.x0, g_box.x1), x1 = fmax(g_box.x0, g_box.x1);
    double z0 = fmin(g_box.z0, g_box.z1), z1 = fmax(g_box.z0, g_box.z1);
    int before = g_group.n;
    static gradient_copy g;
    for (int i = 0, n = game_gradient_count(); i < n; i++) {
        if (!game_gradient_copy(i, &g))
            continue;
        double mx = (g.ax + g.bx) / 2, mz = (g.az + g.bz) / 2;
        if (mx >= x0 && mx <= x1 && mz >= z0 && mz <= z1)
            group_add(MEMBER_GRADIENT, i);
    }
    for (int i = 0, n = curvetool_curve_count(); i < n; i++) {
        double mx, mz;
        if (curvetool_curve_middle(i, &mx, &mz) && mx >= x0 && mx <= x1 && mz >= z0 && mz <= z1)
            group_add(MEMBER_CURVE, i);
    }
    if (g_group.n == before)
        say("Nothing in that box. Ctrl+drag around the middles of the lines.");
    else
        say_group("Added");
}

void gradtool_up(void)
{
    if (g_box.on) {
        box_end();
        return;
    }
    if (!g_move.n)
        return;
    int curves = 0;
    for (int k = 0; k < g_move.n; k++)
        curves |= g_move.kind[k] == MEMBER_CURVE;
    if (curves)
        curvetool_group_move_end();
    g_move.on = 0;
    /* One undo step for everything that ended up somewhere else. */
    gradient_move m = {0};
    m.curves_gen = curvetool_curves_gen();
    m.kind = malloc(sizeof(int) * g_move.n);
    m.from = malloc(sizeof m.from[0] * g_move.n), m.to = malloc(sizeof m.to[0] * g_move.n);
    int n = g_move.n;
    g_move.n = 0;
    if (!m.kind || !m.from || !m.to) {
        move_free(&m);
        say("Moved, but there was no memory to keep its undo.");
        return;
    }
    static gradient_copy g;
    double moved_ft = 0;
    for (int k = 0; k < n; k++) {
        float now[4] = {0, 0, 0, 0};
        if (g_move.kind[k] == MEMBER_GRADIENT) {
            if (!game_gradient_copy(g_move.index[k], &g))
                continue;
            now[0] = g.ax, now[1] = g.az, now[2] = g.bx, now[3] = g.bz;
        } else {
            double fx, fz;
            if (!curvetool_curve_first(g_move.index[k], &fx, &fz))
                continue;
            now[0] = (float)fx, now[1] = (float)fz;
        }
        if (!memcmp(now, g_move.ends[k], sizeof now))
            continue;
        m.kind[m.n] = g_move.kind[k];
        memcpy(m.from[m.n], g_move.ends[k], sizeof now), memcpy(m.to[m.n], now, sizeof now);
        moved_ft = hypot(now[0] - g_move.ends[k][0], now[1] - g_move.ends[k][1]);
        m.n++;
    }
    if (!m.n) {
        move_free(&m); /* pressed and let go without moving */
        return;
    }
    if (g_move_n == UNDO_MAX) {
        move_free(&g_moves[0]);
        memmove(&g_moves[0], &g_moves[1], (UNDO_MAX - 1) * sizeof g_moves[0]), g_move_n--;
    }
    g_moves[g_move_n++] = m;
    if (n == 1)
        say("Moved %s %.1f ft. Ctrl+Z puts it back.", member_name(g_move.kind[0], g_move.index[0]), moved_ft);
    else
        say("Moved %d lines %.1f ft. Ctrl+Z puts them back.", m.n, moved_ft);
}

/* The last move, put back. Each gradient is found by where the move left it (its number may have changed since); each
   curve by its first point, if the curves weren't renumbered. Anything changed since is left where it is. */
static void undo_move(void)
{
    moves_follow_track();
    if (!g_move_n) {
        say("No gradient move to undo.");
        return;
    }
    if (g_move.on || g_box.on) {
        say("Let go of the mouse button first.");
        return;
    }
    gradient_move *m = &g_moves[g_move_n - 1];
    static gradient_copy g;
    int count = game_gradient_count(), done = 0, left = 0, curves = 0, last = -1;
    for (int k = 0; k < m->n; k++)
        curves |= m->kind[k] == MEMBER_CURVE;
    if (curves)
        curvetool_group_move_begin();
    for (int k = 0; k < m->n; k++) {
        const float *to = m->to[k], *from = m->from[k];
        if (m->kind[k] == MEMBER_GRADIENT) {
            int found = -1;
            for (int i = 0; i < count && found < 0; i++)
                if (game_gradient_copy(i, &g) && g.ax == to[0] && g.az == to[1] && g.bx == to[2] && g.bz == to[3])
                    found = i;
            if (found < 0 || game_gradient_set_ends(found, from[0], from[1], from[2], from[3])) {
                left++;
                continue;
            }
            last = found, done++;
        } else {
            /* A curve: the one whose first point is where the move left it. */
            int found = -1;
            if (m->curves_gen == curvetool_curves_gen())
                for (int i = 0, n = curvetool_curve_count(); i < n && found < 0; i++) {
                    double fx, fz;
                    if (curvetool_curve_first(i, &fx, &fz) && (float)fx == to[0] && (float)fz == to[1])
                        found = i;
                }
            if (found < 0) {
                left++;
                continue;
            }
            double fx, fz;
            curvetool_curve_first(found, &fx, &fz);
            curvetool_curve_shift(found, from[0] - fx, from[1] - fz);
            done++;
        }
    }
    if (curves)
        curvetool_group_move_end();
    move_free(m);
    g_move_n--;
    if (last >= 0)
        game_gradient_select(last);
    if (left)
        say("Put back %d; %d had changed since, so they were left.", done, left);
    else if (done == 1)
        say("Put back.%s", g_move_n ? " Ctrl+Z again for the move before." : "");
    else
        say("Put back %d.%s", done, g_move_n ? " Ctrl+Z again for the move before." : "");
}

int gradtool_key(int key)
{
    if (g_placing && key == 27) {
        g_placing = 0;
        say("Placing cancelled.");
        return 1;
    }
    int mode = game_editor_mode();
    int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (mode == MODE_PLACEMENT && (key == 26 || (ctrl && key == 'z'))) {
        moves_follow_track();
        if (g_move_n) {
            undo_move();
            return 1;
        }
        return 0;
    }
    if (key == 127 && (mode == MODE_PLACEMENT || mode == MODE_EDIT) && game_selected_gradient() >= 0) {
        delete_selected();
        return 1;
    }
    return 0;
}

static void open_folder(void)
{
    wchar_t dir[MAX_PATH], cmd[MAX_PATH + 32];
    if (!saved_dir(dir, MAX_PATH) || swprintf(cmd, MAX_PATH + 32, L"explorer.exe \"%ls\"", dir) <= 0)
        return;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
        say("Opened the gradients folder in Explorer (Alt+Tab to it).");
    } else {
        say("Couldn't open Explorer (%lu).", GetLastError());
    }
}

/* ── the radial brushes' sweep ── */
static double g_sweep = 180;
static int g_side = SWEEP_BOTH;
static const char *const SIDE_OPTIONS[] = {"Both Sides", "Clockwise", "Counter-CW"};

int gradtool_sweep(double *degrees, int *side)
{
    *degrees = g_sweep, *side = g_side;
    return !(g_sweep == 180 && g_side == SWEEP_BOTH);
}

void gradtool_set_sweep(double degrees, int side)
{
    if (degrees == degrees) /* not NAN */
        g_sweep = degrees < 30 ? 30 : degrees > 360 ? 360 : floor(degrees / 15 + 0.5) * 15; /* the slider's steps */
    if (side >= SWEEP_BOTH && side <= SWEEP_COUNTER)
        g_side = side;
}

/* ── the panel ── */

void gradtool_frame(void)
{
    int mode = game_editor_mode(), gradient_mode = mode == MODE_PLACEMENT || mode == MODE_EDIT;
    if (!gradient_mode)
        g_placing = 0, g_list_open = 0;
    if (mode != MODE_PLACEMENT)
        g_box.on = 0;
    if (gradient_mode)
        group_follow();
    /* The folder is read again now and then, so files dropped in from elsewhere show up. */
    if (gradient_mode && (g_frames++ % 120) == 0)
        scan();
    bermpreview_frame();
}

int gradtool_rows(panel_row *rows, int max, int *kinds, int *args)
{
    int n = 0, mode = game_editor_mode();
    panel_row *r = NULL;
#define ROW(kind_, rowk, label_) \
    (n < max ? (memset(r = &rows[n], 0, sizeof *r), kinds[n] = (kind_), args[n] = 0, r->kind = (rowk), r->label = (label_), n++, 1) \
             : (r = NULL, 0))
    int selected = game_selected_gradient(), count = game_gradient_count();
    if (mode == MODE_PLACEMENT || mode == MODE_EDIT) {
        if (selected >= 0) {
            if (ROW(GT_HEADER, ROWK_HEADER, ""))
                snprintf(r->text, sizeof r->text, "GRADIENT %d OF %d", selected + 1, count);
            if (mode == MODE_PLACEMENT && ROW(GT_LENGTH, ROWK_CHOICE, "Gradient Length"))
                r->options = LENGTH_OPTIONS, r->count = 3, r->selected = g_gradient_length;
            if (mode == MODE_PLACEMENT && ROW(GT_HINT, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "Shift+drag a line to move it whole");
            ROW(GT_DELETE, ROWK_BUTTON, "Delete Gradient (Del)");
            ROW(GT_EXPORT, ROWK_BUTTON, "Export Gradient");
            if (ROW(GT_BERM_PREVIEW, ROWK_BUTTON, bermpreview_on() ? "Berm Preview: On" : "Berm Preview: Off"))
                r->on = bermpreview_on();
        } else {
            ROW(GT_HEADER, ROWK_HEADER, "GRADIENTS");
            if (ROW(GT_HINT, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, mode == MODE_PLACEMENT ? "Click a gradient's line to pick it"
                                                                         : "Pick one in Gradient Placement (9)");
        }
        if (g_undo_count && same_track(g_deleted_track)) {
            static char undo_label[48];
            snprintf(undo_label, sizeof undo_label, g_undo_count > 1 ? "Undo Delete (%d)" : "Undo Delete", g_undo_count);
            ROW(GT_UNDO_DELETE, ROWK_BUTTON, undo_label);
        }
        if (mode == MODE_PLACEMENT) {
            ROW(GT_HEADER, ROWK_HEADER, "GRADIENT GROUP");
            int gradients, curves;
            group_counts(&gradients, &curves);
            if (g_group.n) {
                if (ROW(GT_HINT, ROWK_LABEL, ""))
                    snprintf(r->text, sizeof r->text, "%d gradient%s, %d curve%s", gradients, gradients == 1 ? "" : "s",
                             curves, curves == 1 ? "" : "s");
                ROW(GT_CLEAR_GROUP, ROWK_BUTTON, "Clear Group");
            }
            if (ROW(GT_HINT, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "Ctrl+click: add or drop a line");
            if (ROW(GT_HINT, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "Ctrl+drag: box around middles");
            if (g_group.n && ROW(GT_HINT, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "Shift+drag a gold line: move all");
        }
        moves_follow_track();
        if (mode == MODE_PLACEMENT && g_move_n) {
            static char move_label[48];
            snprintf(move_label, sizeof move_label, "Undo Gradient Move (Ctrl+Z)  %d", g_move_n);
            ROW(GT_UNDO_MOVE, ROWK_BUTTON, move_label);
        }

        ROW(GT_HEADER, ROWK_HEADER, "SAVED GRADIENTS");
        if (!g_saved_count) {
            if (ROW(GT_HINT, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "None yet: export one to start");
        } else {
            g_saved_choice = g_saved_choice < g_saved_count ? g_saved_choice : 0;
            /* The chosen file across the whole row; a click lists them all under it, newest first. */
            if (ROW(GT_SAVED, ROWK_BUTTON, g_saved[g_saved_choice].name))
                r->on = g_list_open;
            if (g_list_open) {
                int first, last;
                panel_list_range(PANEL_LIST_SAVED_GRADIENTS, g_saved_count, &first, &last);
                if (first > 0 && ROW(PANEL_LIST_UP, ROWK_BUTTON, panel_list_more(PANEL_LIST_SAVED_GRADIENTS, 1, first)))
                    r->list = PANEL_LIST_SAVED_GRADIENTS;
                for (int i = first; i < last; i++)
                    if (ROW(GT_SAVED_ITEM, ROWK_BUTTON, g_saved[i].name)) {
                        r->on = i == g_saved_choice;
                        r->list = PANEL_LIST_SAVED_GRADIENTS;
                        args[n - 1] = i;
                    }
                if (last < g_saved_count &&
                    ROW(PANEL_LIST_DOWN, ROWK_BUTTON, panel_list_more(PANEL_LIST_SAVED_GRADIENTS, 0, g_saved_count - last)))
                    r->list = PANEL_LIST_SAVED_GRADIENTS;
            }
            if (g_placing) {
                if (ROW(GT_HINT, ROWK_HINT, ""))
                    snprintf(r->text, sizeof r->text, "Click the map where A goes");
                if (ROW(GT_CANCEL_PLACE, ROWK_BUTTON, "Cancel Placing (Esc)"))
                    r->on = 1;
            } else {
                ROW(GT_PLACE, ROWK_BUTTON, "Place On Map");
                ROW(GT_PUT_BACK, ROWK_BUTTON, "Put Back Where It Was");
            }
        }
        ROW(GT_FOLDER, ROWK_BUTTON, "Open Gradients Folder");
    }
    if (mode == MODE_RADIAL_PAINT || mode == MODE_RADIAL_ADD || bermpreview_on()) {
        ROW(GT_HEADER, ROWK_HEADER, "RADIAL SWEEP");
        if (ROW(GT_SWEEP, ROWK_SLIDER, "Sweep"))
            r->value = g_sweep, r->min = 30, r->max = 360, r->step = 15;
        if (g_sweep < 360 && ROW(GT_SWEEP_SIDE, ROWK_CHOICE, "From B"))
            r->options = SIDE_OPTIONS, r->count = 3, r->selected = g_side;
        if (ROW(GT_HINT, ROWK_HINT, "")) {
            if (selected < 0)
                snprintf(r->text, sizeof r->text, "Pick a gradient in Placement (9)");
            else if (g_sweep >= 360)
                snprintf(r->text, sizeof r->text, "A full circle around A");
            else if (g_sweep == 180 && g_side == SWEEP_BOTH)
                snprintf(r->text, sizeof r->text, "The game's own sweep");
            else
                snprintf(r->text, sizeof r->text, "Past the edges it runs straight");
        }
    }
#undef ROW
    return n;
}

void gradtool_press(int kind, int arg)
{
    switch (kind) {
    case GT_LENGTH: g_gradient_length = (g_gradient_length + 1) % 3; break;
    case GT_DELETE: delete_selected(); break;
    case GT_UNDO_DELETE: undo_delete(); break;
    case GT_UNDO_MOVE: undo_move(); break;
    case GT_CLEAR_GROUP:
        if (!g_move.on && !g_box.on) {
            g_group.n = 0;
            say("Group cleared.");
        }
        break;
    case GT_EXPORT: export_selected(); break;
    case GT_SAVED:
        g_list_open = !g_list_open;
        if (g_list_open)
            scan(), panel_list_show(PANEL_LIST_SAVED_GRADIENTS, g_saved_choice);
        break;
    case GT_SAVED_ITEM:
        g_saved_choice = arg;
        g_list_open = 0;
        break;
    case GT_PLACE:
        g_list_open = 0;
        if (load_chosen(&g_place)) {
            g_placing = 1;
            say("Click the map where its A goes (Esc cancels).");
        }
        break;
    case GT_CANCEL_PLACE:
        g_placing = 0;
        say("Placing cancelled.");
        break;
    case GT_PUT_BACK: {
        static gradient_copy g;
        g_list_open = 0;
        if (!load_chosen(&g))
            break;
        int same = same_spot(&g);
        if (same >= 0) {
            say("It's already there: gradient %d is the same, in the same spot.", same + 1);
            break;
        }
        add_gradient(&g, "Put back where it was");
        break;
    }
    case GT_FOLDER: open_folder(); break;
    case GT_SWEEP_SIDE: g_side = (g_side + 1) % 3; break;
    case GT_BERM_PREVIEW: bermpreview_toggle(); break;
    }
}

void gradtool_slide(int kind, double value)
{
    if (kind == GT_SWEEP)
        g_sweep = value < 30 ? 30 : value > 360 ? 360 : value;
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

static void line(scene *s, double x0, double z0, double x1, double z1, const float rgba[4])
{
    scene_item *it = add(s, SCENE_LINE, rgba, 2);
    if (it) {
        s->pt[it->first][0] = (float)x0, s->pt[it->first][1] = (float)z0;
        s->pt[it->first + 1][0] = (float)x1, s->pt[it->first + 1][1] = (float)z1;
    }
}

static void dot(scene *s, double x, double z, float size, const float rgba[4])
{
    scene_item *it = add(s, SCENE_DOT, rgba, 0);
    if (it)
        it->x = (float)x, it->z = (float)z, it->size = size;
}

/* The wedge's middle direction and half width, in radians, x right and z down the screen (so a growing angle turns
   clockwise on screen). The same numbers ingame.c's radial helper uses. */
void gradtool_wedge(double ax, double az, double bx, double bz, double *mid, double *half)
{
    double toward_b = atan2(bz - az, bx - ax), sweep = g_sweep * 3.14159265358979 / 180;
    *half = sweep / 2;
    *mid = g_side == SWEEP_CLOCKWISE ? toward_b + *half : g_side == SWEEP_COUNTER ? toward_b - *half : toward_b;
}

void gradtool_scene(scene *out, double mouse_x, double mouse_z, double ft_per_px)
{
    double g_ft_per_px = ft_per_px;
    static const float gold[4] = {1.0f, 0.824f, 0.290f, 0.95f}, faint[4] = {1.0f, 0.824f, 0.290f, 0.45f};
    static const float start[4] = {0.490f, 1.0f, 0.541f, 1.0f}, end[4] = {1, 1, 1, 1};
    if (g_placing) {
        /* Where it will go: A under the mouse, B the same way and as far as it was. */
        double bx = mouse_x + (g_place.bx - g_place.ax), bz = mouse_z + (g_place.bz - g_place.az);
        line(out, mouse_x, mouse_z, bx, bz, gold);
        dot(out, mouse_x, mouse_z, 0.7f, start);
        dot(out, bx, bz, 0.55f, end);
        return;
    }
    int mode = game_editor_mode(), selected = game_selected_gradient();
    static gradient_copy g;
    if (mode == MODE_PLACEMENT) {
        /* The group in gold, always; with Shift or Ctrl held, the line a press would grab too; while moving, what moves. */
        int held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000);
        int n = g_move.on ? g_move.n : g_group.n;
        const int *kind = g_move.on ? g_move.kind : g_group.kind, *index = g_move.on ? g_move.index : g_group.index;
        int hk = 0, hi = -1;
        if (!g_move.on && held && !g_box.on && !member_at(mouse_x, mouse_z, 8 * g_ft_per_px, &hk, &hi))
            hi = -1;
        for (int k = -1; k < n; k++) {
            int mk = k < 0 ? hk : kind[k], mi = k < 0 ? hi : index[k];
            if (mi < 0)
                continue;
            if (mk == MEMBER_GRADIENT) {
                if (!game_gradient_copy(mi, &g))
                    continue;
                line(out, g.ax, g.az, g.bx, g.bz, k < 0 ? faint : gold);
                dot(out, g.ax, g.az, 0.55f, start);
                dot(out, g.bx, g.bz, 0.55f, end);
            } else {
                static double pts[CURVE_MAX_ANCHORS * 17][2];
                int count = curvetool_curve_line(mi, pts, CURVE_MAX_ANCHORS * 17);
                scene_item *it = count > 1 ? add(out, SCENE_LINE, k < 0 ? faint : gold, count) : NULL;
                if (it) {
                    it->size = 2;
                    for (int p = 0; p < count; p++)
                        out->pt[it->first + p][0] = (float)pts[p][0], out->pt[it->first + p][1] = (float)pts[p][1];
                }
            }
        }
        if (g_box.on) {
            static const float white[4] = {1, 1, 1, 0.8f};
            scene_item *it = add(out, SCENE_LINE, white, 4);
            if (it) {
                it->closed = 1;
                float (*pt)[2] = &out->pt[it->first];
                pt[0][0] = (float)g_box.x0, pt[0][1] = (float)g_box.z0, pt[1][0] = (float)g_box.x1, pt[1][1] = (float)g_box.z0;
                pt[2][0] = (float)g_box.x1, pt[2][1] = (float)g_box.z1, pt[3][0] = (float)g_box.x0, pt[3][1] = (float)g_box.z1;
            }
        }
        if (held || g_move.on || g_box.on)
            return;
    }
    if ((mode != MODE_RADIAL_PAINT && mode != MODE_RADIAL_ADD && !bermpreview_on()) || selected < 0 || !game_gradient_copy(selected, &g))
        return;
    /* The sweep: from A out to the end of the profile, an arc between its two edges. */
    double radius = g.points ? g.p[g.points - 1].s : 0;
    if (!(radius > 0))
        radius = hypot(g.bx - g.ax, g.bz - g.az);
    if (!(radius > 0))
        return;
    double mid, half;
    gradtool_wedge(g.ax, g.az, g.bx, g.bz, &mid, &half);
    int full = g_sweep >= 360, steps = 96;
    scene_item *arc = add(out, SCENE_LINE, gold, steps + 1);
    if (arc) {
        arc->closed = full;
        for (int i = 0; i <= steps; i++) {
            double a = full ? i * 6.28318530717959 / steps : mid - half + 2 * half * i / steps;
            out->pt[arc->first + i][0] = (float)(g.ax + radius * cos(a)), out->pt[arc->first + i][1] = (float)(g.az + radius * sin(a));
        }
    }
    if (!full) {
        for (int k = -1; k <= 1; k += 2) {
            double a = mid + k * half;
            line(out, g.ax, g.az, g.ax + radius * cos(a), g.az + radius * sin(a), gold);
            /* Past each edge the profile runs on straight: a short faint line along that edge's far side. */
            double ex = g.ax + radius * cos(a), ez = g.az + radius * sin(a), px = -sin(a) * k, pz = cos(a) * k;
            line(out, ex, ez, ex + px * radius * 0.35, ez + pz * radius * 0.35, faint);
        }
    }
    dot(out, g.ax, g.az, 0.6f, start);
}
