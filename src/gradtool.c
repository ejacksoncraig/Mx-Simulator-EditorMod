/* Tools for the game's own gradients, in our panel while the game's tools are in use (game thread only):
   - Gradient Placement and Edit: Delete (or Del) with Undo Delete, Export to a file, the Saved Gradients list, Place On
     Map (the next click puts its A there, same direction and length) and Put Back Where It Was. Gradient Length
     (Locked / Extend / Stretch) lives here too, so every row about the picked gradient is in one group.
   - Gradient Radial Paint and Add: how far the brush sweeps around A (the game's own is 180 degrees, centred on B,
     running straight past the edges), drawn on the map as a wedge. ingame.c swaps the game's radial helper for one
     that follows it. The rule is in docs/ingame-tools.md, "Gradient tools".
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
    GT_CANCEL_PLACE, GT_PUT_BACK, GT_FOLDER, GT_SWEEP, GT_SWEEP_SIDE
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

int gradtool_key(int key)
{
    if (g_placing && key == 27) {
        g_placing = 0;
        say("Placing cancelled.");
        return 1;
    }
    int mode = game_editor_mode();
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

/* ── the panel ── */

void gradtool_frame(void)
{
    int mode = game_editor_mode(), gradient_mode = mode == MODE_PLACEMENT || mode == MODE_EDIT;
    if (!gradient_mode)
        g_placing = 0, g_list_open = 0;
    /* The folder is read again now and then, so files dropped in from elsewhere show up. */
    if (gradient_mode && (g_frames++ % 120) == 0)
        scan();
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
            ROW(GT_DELETE, ROWK_BUTTON, "Delete Gradient (Del)");
            ROW(GT_EXPORT, ROWK_BUTTON, "Export Gradient");
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

        ROW(GT_HEADER, ROWK_HEADER, "SAVED GRADIENTS");
        if (!g_saved_count) {
            if (ROW(GT_HINT, ROWK_HINT, ""))
                snprintf(r->text, sizeof r->text, "None yet: export one to start");
        } else {
            g_saved_choice = g_saved_choice < g_saved_count ? g_saved_choice : 0;
            /* The chosen file across the whole row; a click lists them all under it, newest first. */
            if (ROW(GT_SAVED, ROWK_BUTTON, g_saved[g_saved_choice].name))
                r->on = g_list_open;
            if (g_list_open)
                for (int i = 0; i < g_saved_count; i++)
                    if (ROW(GT_SAVED_ITEM, ROWK_BUTTON, g_saved[i].name)) {
                        r->on = i == g_saved_choice;
                        args[n - 1] = i;
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
    if (mode == MODE_RADIAL_PAINT || mode == MODE_RADIAL_ADD) {
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
    case GT_EXPORT: export_selected(); break;
    case GT_SAVED:
        g_list_open = !g_list_open;
        if (g_list_open)
            scan();
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

void gradtool_scene(scene *out, double mouse_x, double mouse_z)
{
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
    if ((mode != MODE_RADIAL_PAINT && mode != MODE_RADIAL_ADD) || selected < 0 || !game_gradient_copy(selected, &g))
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
