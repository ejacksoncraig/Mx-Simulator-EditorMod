/* The app's linked pieces in the game's Statues mode. MX Simulator Tools keeps which lines belong together in
   mx-tools-groups.json beside the track: [{id, name, lines: {statues: [exact line text], billboards: [...], ...}}]. The
   game knows nothing of it. Here each piece's statues lines are found in the game's list, so the statue group can take a
   whole piece at once, Duplicate keeps each copy linked and Snap moves a piece as one (statuetool.c).

   - The file names lines by their text and the game's list holds no text, so its lines are looked up in the statues file
     on disk, and only while that file is what the game holds: each statue agrees with a line, in order. That is so when
     the track has just been loaded and right after the game saves it.
   - In between, each statue's piece is followed through the list by its mark (mesh, texture, place, turn, height): when
     the list's length changes (the game's d or Del, our copies, an undo), the old list is lined up against the new one.
   - When the game has saved (the statues file changed and agrees with the list), each piece's statues lines are written
     again from that file, so a piece moved or copied in the game is still that piece in the app. Its other lines
     (billboards, flares, flaggers) are kept as they were, and so are statues lines the game didn't load.
   - When the app has saved the groups file, it's read again (pieces made here and not written yet are kept).
   - The app matches a line whose numbers only went through the game's 32-bit floats ("loose"), and so does this.
   Game thread only. */
#include "curvetool.h"
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define POLL_FRAMES 30 /* how often the two files' times are looked at */
#define LINE_CAP 2048
#define NAME_CAP 1024
#define MAX_KINDS 16
#define GROUPS_FILE L"mx-tools-groups.json"

typedef struct {
    char kind[32];
    int n;
    char **text;
} kind_lines;

typedef struct {
    char id[64], name[256];
    int kinds_n;
    kind_lines kinds[MAX_KINDS]; /* as read; for statues, only the lines no statue in the list took */
    int fresh;                   /* made in the game this session and not in the file yet */
} piece;

static piece *g_pieces;
static int g_pieces_n, g_pieces_cap;
static int *g_piece_of;     /* each statue's piece, -1 for none */
static statue_mark *g_marks; /* the list as it was last frame */
static int g_n = -1, g_cap;
static wchar_t g_dir[MAX_PATH];
static FILETIME g_statues_seen, g_groups_seen;
static int g_poll, g_resync, g_no_write, g_loose;
static unsigned g_made;

/* ── small things ── */

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void free_piece(piece *p)
{
    for (int k = 0; k < p->kinds_n; k++) {
        for (int t = 0; t < p->kinds[k].n; t++)
            free(p->kinds[k].text[t]);
        free(p->kinds[k].text);
    }
    memset(p, 0, sizeof *p);
}

static piece *new_piece(void)
{
    if (g_pieces_n == g_pieces_cap) {
        int cap = g_pieces_cap ? g_pieces_cap * 2 : 64;
        piece *grown = realloc(g_pieces, sizeof(piece) * (size_t)cap);
        if (!grown)
            return NULL;
        g_pieces = grown, g_pieces_cap = cap;
    }
    piece *p = &g_pieces[g_pieces_n++];
    memset(p, 0, sizeof *p);
    return p;
}

static kind_lines *kind_of(piece *p, const char *kind, int make)
{
    for (int k = 0; k < p->kinds_n; k++)
        if (strcmp(p->kinds[k].kind, kind) == 0)
            return &p->kinds[k];
    if (!make || p->kinds_n == MAX_KINDS)
        return NULL;
    kind_lines *k = &p->kinds[p->kinds_n++];
    memset(k, 0, sizeof *k);
    snprintf(k->kind, sizeof k->kind, "%s", kind);
    return k;
}

static int add_text(kind_lines *k, const char *text)
{
    char **grown = realloc(k->text, sizeof(char *) * (size_t)(k->n + 1));
    if (!grown)
        return 0;
    k->text = grown;
    return (k->text[k->n] = dup_str(text)) != NULL ? (k->n++, 1) : 0;
}

/* Paths the way the game finds them: no @, either slash, any case. */
static int same_path(const char *a, const char *b)
{
    if (*a == '@')
        a++;
    if (*b == '@')
        b++;
    for (; *a && *b; a++, b++) {
        char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

typedef struct {
    float x, y, z, rot;
    char mesh[NAME_CAP], tex[NAME_CAP], shape[NAME_CAP];
} parsed_line;

/* "[x y offset z] rot mesh texture shape", as the game reads a statues line. 0 for anything else: the game stops there. */
static int parse_line(const char *text, parsed_line *out)
{
    return sscanf(text, " [%f %f %f] %f %1023s %1023s %1023s", &out->x, &out->y, &out->z, &out->rot, out->mesh, out->tex,
                  out->shape) == 7;
}

/* The same object to the game: each number as a 32-bit float, the paths as the game finds them. */
static int loose_same(const char *a, const char *b)
{
    static parsed_line p, q;
    return parse_line(a, &p) && parse_line(b, &q) && p.x == q.x && p.y == q.y && p.z == q.z && p.rot == q.rot &&
           same_path(p.mesh, q.mesh) && same_path(p.tex, q.tex) && same_path(p.shape, q.shape);
}

static char *read_all(const wchar_t *path, size_t *len)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
        free(buf), buf = NULL;
    fclose(f);
    if (buf)
        buf[n] = 0, *len = (size_t)n;
    return buf;
}

static FILETIME time_of(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME none = {0, 0};
    return GetFileAttributesExW(path, GetFileExInfoStandard, &a) ? a.ftLastWriteTime : none;
}

static int same_time(FILETIME a, FILETIME b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

static void path_in(wchar_t *out, size_t cap, const wchar_t *name) { swprintf(out, cap, L"%ls\\%ls", g_dir, name); }

/* ── following the list ── */

static int mark_same(const statue_mark *a, const statue_mark *b)
{
    return a->mesh == b->mesh && a->tex == b->tex && a->x == b->x && a->z == b->z && a->rot == b->rot && a->offset == b->offset;
}

/* Last frame's list against this one's, in order: a statue gone from the old one was deleted, and new ones come at the
   end (the game's add appends). When nothing at all lines up, the track was loaded again: read the file again. */
static void follow_list(void)
{
    int n = game_statue_count();
    if (n < 0)
        n = 0;
    if (n > g_cap) {
        int cap = n * 2 > 1024 ? n * 2 : 1024;
        statue_mark *marks = realloc(g_marks, sizeof(statue_mark) * (size_t)cap);
        if (!marks)
            return;
        g_marks = marks;
        int *of = realloc(g_piece_of, sizeof(int) * (size_t)cap);
        if (!of)
            return;
        g_piece_of = of, g_cap = cap;
    }
    static statue_mark *now;
    static int now_cap;
    if (n > now_cap) {
        statue_mark *grown = realloc(now, sizeof(statue_mark) * (size_t)g_cap);
        if (!grown)
            return;
        now = grown, now_cap = g_cap;
    }
    n = n ? game_statue_marks(now, now_cap) : 0;
    if (n > now_cap)
        return; /* the list grew between the two reads; next frame */
    if (g_n < 0) {
        for (int i = 0; i < n; i++)
            g_piece_of[i] = -1;
        g_resync = 1;
    } else if (n != g_n) {
        static int *was;
        static int was_cap;
        if (g_n > was_cap) {
            int *grown = realloc(was, sizeof(int) * (size_t)g_cap);
            if (!grown)
                return;
            was = grown, was_cap = g_cap;
        }
        memcpy(was, g_piece_of, sizeof(int) * (size_t)g_n);
        int j = 0, lined = 0;
        for (int i = 0; i < n; i++) {
            int k = j;
            while (k < g_n && !mark_same(&g_marks[k], &now[i]))
                k++;
            if (k < g_n)
                g_piece_of[i] = was[k], j = k + 1, lined++;
            else
                g_piece_of[i] = -1;
        }
        if (n && g_n && !lined)
            g_resync = 1;
    } else if (n > 1) {
        int kept = 0;
        for (int i = 0; i < n && !kept; i++)
            kept = g_marks[i].mesh == now[i].mesh;
        if (!kept) { /* the same count, but every mesh loaded again: the track was reloaded */
            for (int i = 0; i < n; i++)
                g_piece_of[i] = -1;
            g_resync = 1;
        }
    }
    memcpy(g_marks, now, sizeof(statue_mark) * (size_t)n);
    g_n = n;
}

/* ── the files ── */

typedef struct {
    char *buf;
    char **line; /* each statues line the game reads, up to the first it can't */
    int n;
    int *statue; /* the statue each line is in the game's list, -1 for one the game didn't take */
} statues_file;

static void free_statues(statues_file *f)
{
    free(f->buf), free(f->line), free(f->statue);
    memset(f, 0, sizeof *f);
}

static int read_statues(statues_file *f)
{
    memset(f, 0, sizeof *f);
    wchar_t path[MAX_PATH];
    path_in(path, MAX_PATH, L"statues");
    size_t len = 0;
    if (!(f->buf = read_all(path, &len))) {
        f->buf = dup_str(""); /* no statues file: no statues */
        return f->buf != NULL;
    }
    int cap = 1;
    for (size_t k = 0; k < len; k++)
        cap += f->buf[k] == '\n';
    f->line = malloc(sizeof(char *) * (size_t)cap);
    f->statue = malloc(sizeof(int) * (size_t)cap);
    if (!f->line || !f->statue)
        return 0;
    static parsed_line p;
    for (char *at = f->buf; *at;) {
        char *end = strchr(at, '\n'), *next = end ? end + 1 : at + strlen(at);
        if (end) {
            *end = 0;
            if (end > at && end[-1] == '\r')
                end[-1] = 0;
        }
        if (!parse_line(at, &p))
            break;
        f->statue[f->n] = -1;
        f->line[f->n++] = at;
        at = next;
    }
    return 1;
}

/* Does statue i hold this line? Its numbers as the game read them, and its mesh and texture when they can be read. */
static int statue_holds(int i, const char *text)
{
    static parsed_line p;
    static char mesh[NAME_CAP], shape[NAME_CAP];
    if (!parse_line(text, &p))
        return 0;
    const statue_mark *m = &g_marks[i];
    if (fabsf(p.x - m->x) > 1e-3f || fabsf(p.z - m->z) > 1e-3f || fabsf(p.y - m->offset) > 1e-3f || fabsf(p.rot - m->rot) > 1e-4f)
        return 0;
    if (game_statue_files(i, mesh, shape, NAME_CAP) && mesh[0] && !same_path(mesh, p.mesh))
        return 0;
    const char *tex = game_statue_texture(i);
    return !tex || same_path(tex, p.tex);
}

/* Each statue in the list to its line, in order. 0 if the file isn't what the game holds (edits not saved yet). */
static int line_up(statues_file *f)
{
    int j = 0;
    for (int i = 0; i < g_n; i++) {
        while (j < f->n && !statue_holds(i, f->line[j]))
            j++;
        if (j == f->n)
            return 0;
        f->statue[j++] = i;
    }
    return 1;
}

/* One group of the app's file into a new piece. */
static int read_group(json_reader *r)
{
    piece *p = new_piece();
    if (!p)
        return json_fail(r, "out of memory");
    static char key[64], kind[64], text[LINE_CAP];
    int first = 1;
    while (json_next_key(r, key, sizeof key, &first)) {
        if (strcmp(key, "id") == 0) {
            if (!json_string(r, p->id, sizeof p->id))
                return 0;
        } else if (strcmp(key, "name") == 0) {
            if (!json_string(r, p->name, sizeof p->name))
                return 0;
        } else if (strcmp(key, "lines") == 0) {
            int each_kind = 1;
            while (json_next_key(r, kind, sizeof kind, &each_kind)) {
                kind_lines *k = kind_of(p, kind, 1);
                if (!k)
                    return json_fail(r, "more kinds of line than the mod keeps");
                int each = 1;
                while (json_next_item(r, &each)) {
                    if (!json_string(r, text, sizeof text))
                        return 0;
                    if (strlen(text) + 1 >= sizeof text)
                        return json_fail(r, "a line longer than the mod keeps");
                    if (!add_text(k, text))
                        return json_fail(r, "out of memory");
                }
                if (r->error)
                    return 0;
            }
            if (r->error)
                return 0;
        } else {
            return json_fail(r, "a key the mod doesn't know, so it won't write the file");
        }
    }
    return !r->error;
}

/* The app's file read again: pieces made here stay, the rest come from the file, each statues line found in the list. */
static void read_groups(statues_file *f)
{
    wchar_t path[MAX_PATH];
    path_in(path, MAX_PATH, GROUPS_FILE);
    size_t len = 0;
    char *text = read_all(path, &len);

    /* Keep only the fresh pieces, numbered again from 0. */
    int *moved = malloc(sizeof(int) * (size_t)(g_pieces_n + 1)), kept = 0;
    if (!moved) {
        free(text);
        return;
    }
    for (int k = 0; k < g_pieces_n; k++) {
        if (g_pieces[k].fresh) {
            moved[k] = kept;
            if (kept != k)
                g_pieces[kept] = g_pieces[k];
            kept++;
        } else {
            moved[k] = -1;
            free_piece(&g_pieces[k]);
        }
    }
    for (int i = 0; i < g_n; i++)
        g_piece_of[i] = g_piece_of[i] >= 0 ? moved[g_piece_of[i]] : -1;
    free(moved);
    g_pieces_n = kept;
    g_no_write = 0;
    g_loose = 0;
    if (!text)
        return; /* no file: nothing linked yet */

    json_reader r = {text, text + len, NULL};
    int first = 1, from = g_pieces_n;
    while (json_next_item(&r, &first))
        if (!read_group(&r))
            break;
    if (r.error) {
        mx_log("statue links: %ls couldn't be read (%s), so linked pieces aren't known and it won't be written", path, r.error);
        for (int k = from; k < g_pieces_n; k++)
            free_piece(&g_pieces[k]);
        g_pieces_n = from;
        g_no_write = 1;
        free(text);
        return;
    }
    free(text);

    /* Exact text first, across every piece, so a loose match never takes a line another piece names exactly. */
    char *taken = calloc((size_t)f->n + 1, 1);
    if (!taken)
        return;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = from; k < g_pieces_n; k++) {
            kind_lines *s = kind_of(&g_pieces[k], "statues", 0);
            if (!s)
                continue;
            int left = 0;
            for (int t = 0; t < s->n; t++) {
                int found = -1;
                for (int j = 0; j < f->n && found < 0; j++)
                    if (!taken[j] && f->statue[j] >= 0 && g_piece_of[f->statue[j]] < 0 &&
                        (pass == 0 ? strcmp(s->text[t], f->line[j]) == 0 : loose_same(s->text[t], f->line[j])))
                        found = j;
                if (found >= 0) {
                    taken[found] = 1;
                    g_piece_of[f->statue[found]] = k;
                    g_loose += pass;
                    free(s->text[t]);
                } else {
                    s->text[left++] = s->text[t]; /* not found (yet): kept, so it's written back as it was */
                }
            }
            s->n = left;
        }
    }
    free(taken);
    int linked = 0;
    for (int i = 0; i < g_n; i++)
        linked += g_piece_of[i] >= from;
    mx_log("statue links: %d pieces read, %d statues in them found in the list", g_pieces_n - from, linked);
}

static int put_string(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\')
            fprintf(out, "\\%c", *c);
        else if (*c < 0x20 || *c >= 0x7f)
            fprintf(out, "\\u%04x", *c); /* Latin-1 bytes, as the app's json.dumps writes them */
        else
            fputc(*c, out);
    }
    return fputc('"', out) != EOF;
}

/* The app's file written again: each piece's statues lines as the statues file has them now, then the ones it didn't
   find, then its other kinds as they were. A piece of fewer than two lines is left out, as the app leaves it out. */
static void write_groups(statues_file *f)
{
    if (g_no_write)
        return;
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    path_in(path, MAX_PATH, GROUPS_FILE);
    swprintf(tmp, MAX_PATH, L"%ls.mxmod-tmp", path);
    int *count = calloc((size_t)g_pieces_n + 1, sizeof(int));
    if (!count)
        return;
    for (int j = 0; j < f->n; j++)
        if (f->statue[j] >= 0 && g_piece_of[f->statue[j]] >= 0)
            count[g_piece_of[f->statue[j]]]++;
    int wanted = 0;
    for (int k = 0; k < g_pieces_n; k++) {
        int lines = count[k];
        for (int t = 0; t < g_pieces[k].kinds_n; t++)
            lines += g_pieces[k].kinds[t].n;
        count[k] = lines >= 2 ? count[k] : -1;
        wanted += lines >= 2;
    }
    if (!wanted && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        free(count);
        return;
    }
    FILE *out = _wfopen(tmp, L"wb");
    if (!out) {
        free(count);
        mx_log("statue links: couldn't write %ls", tmp);
        return;
    }
    fputs("[", out);
    int written = 0;
    for (int k = 0; k < g_pieces_n; k++) {
        piece *p = &g_pieces[k];
        if (count[k] < 0)
            continue;
        if (!p->id[0])
            snprintf(p->id, sizeof p->id, "%08lx%04x", (unsigned long)GetTickCount(), ++g_made & 0xffff);
        fputs(written++ ? ",\n {\n  \"id\": " : "\n {\n  \"id\": ", out);
        put_string(out, p->id);
        fputs(",\n  \"name\": ", out);
        put_string(out, p->name[0] ? p->name : "Linked objects");
        fputs(",\n  \"lines\": {", out);
        int kinds = 0;
        kind_lines *s = kind_of(p, "statues", 0);
        if (count[k] + (s ? s->n : 0) > 0) {
            fputs("\n   \"statues\": [", out);
            kinds++;
            int item = 0;
            for (int j = 0; j < f->n; j++)
                if (f->statue[j] >= 0 && g_piece_of[f->statue[j]] == k) {
                    fputs(item++ ? ",\n    " : "\n    ", out);
                    put_string(out, f->line[j]);
                }
            for (int q = 0; s && q < s->n; q++) {
                fputs(item++ ? ",\n    " : "\n    ", out);
                put_string(out, s->text[q]);
            }
            fputs("\n   ]", out);
        }
        for (int t = 0; t < p->kinds_n; t++) {
            if (&p->kinds[t] == s || p->kinds[t].n == 0)
                continue;
            fputs(kinds++ ? ",\n   " : "\n   ", out);
            put_string(out, p->kinds[t].kind);
            fputs(": [", out);
            for (int q = 0; q < p->kinds[t].n; q++) {
                fputs(q ? ",\n    " : "\n    ", out);
                put_string(out, p->kinds[t].text[q]);
            }
            fputs("\n   ]", out);
        }
        fputs("\n  }\n }", out);
    }
    fputs(written ? "\n]\n" : "]\n", out);
    int ok = !ferror(out);
    ok &= fclose(out) == 0;
    free(count);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        mx_log("statue links: couldn't replace %ls", path);
        return;
    }
    for (int k = 0; k < g_pieces_n; k++)
        g_pieces[k].fresh = 0;
    g_groups_seen = time_of(path);
    mx_log("statue links: wrote %d pieces to %ls", written, path);
}

/* The two files against the list: read the app's file when it changed (or the track is new), write it when the game
   saved. Both only while the statues file is what the game holds. */
static void sync(int statues_changed)
{
    wchar_t path[MAX_PATH];
    path_in(path, MAX_PATH, GROUPS_FILE);
    FILETIME groups_now = time_of(path);
    int groups_changed = g_resync || !same_time(groups_now, g_groups_seen);
    statues_file f;
    int ok = read_statues(&f) && line_up(&f);
    if (!ok && g_resync)
        mx_log("statue links: the statues file isn't what the game holds yet, so linked pieces wait for a save");
    g_resync = 0; /* tried; the next try is when a file changes */
    if (!ok) {
        free_statues(&f);
        return;
    }
    if (groups_changed) {
        g_groups_seen = groups_now;
        read_groups(&f);
    }
    int fresh = 0;
    for (int k = 0; k < g_pieces_n; k++)
        fresh |= g_pieces[k].fresh;
    /* After the game's save the lines' text may be new; a loose match means the file still names the old text. */
    if (fresh || g_loose || (statues_changed && !groups_changed && g_pieces_n)) {
        write_groups(&f);
        g_loose = 0;
    }
    free_statues(&f);
}

static void reset(void)
{
    for (int k = 0; k < g_pieces_n; k++)
        free_piece(&g_pieces[k]);
    g_pieces_n = 0;
    for (int i = 0; i < g_n; i++)
        g_piece_of[i] = -1;
    memset(&g_statues_seen, 0, sizeof g_statues_seen);
    memset(&g_groups_seen, 0, sizeof g_groups_seen);
    g_resync = 1;
}

void statuelinks_frame(void)
{
    if (!game_statues_known())
        return;
    follow_list();
    if (++g_poll < POLL_FRAMES && !g_resync)
        return;
    g_poll = 0;
    wchar_t dir[MAX_PATH];
    if (!mod_track_dir(dir, MAX_PATH))
        return;
    if (wcscmp(dir, g_dir) != 0) {
        wcscpy_s(g_dir, MAX_PATH, dir);
        reset();
    }
    wchar_t path[MAX_PATH];
    path_in(path, MAX_PATH, L"statues");
    FILETIME statues_now = time_of(path), groups_now;
    path_in(path, MAX_PATH, GROUPS_FILE);
    groups_now = time_of(path);
    int statues_changed = !same_time(statues_now, g_statues_seen);
    if (!g_resync && !statues_changed && same_time(groups_now, g_groups_seen))
        return;
    g_statues_seen = statues_now;
    sync(statues_changed);
}

/* ── for statuetool.c ── */

int statuelinks_piece(int i) { return i >= 0 && i < g_n && g_piece_of ? g_piece_of[i] : -1; }

int statuelinks_members(int p, int *out, int max)
{
    int n = 0;
    for (int i = 0; p >= 0 && i < g_n; i++)
        if (g_piece_of[i] == p && n < max)
            out[n++] = i;
    return n;
}

const char *statuelinks_name(int p) { return p >= 0 && p < g_pieces_n && g_pieces[p].name[0] ? g_pieces[p].name : "Linked objects"; }

int statuelinks_count(void)
{
    static int *seen;
    static int seen_cap;
    if (g_pieces_n > seen_cap) {
        int *grown = realloc(seen, sizeof(int) * (size_t)g_pieces_n);
        if (!grown)
            return 0;
        seen = grown, seen_cap = g_pieces_n;
    }
    if (!g_pieces_n)
        return 0;
    memset(seen, 0, sizeof(int) * (size_t)g_pieces_n);
    int n = 0;
    for (int i = 0; i < g_n; i++)
        if (g_piece_of[i] >= 0 && ++seen[g_piece_of[i]] == 2)
            n++;
    return n;
}

void statuelinks_copied(const int *from, const int *to, int n)
{
    follow_list(); /* the copies are in the list now: line it up first, so they're known by index */
    for (int k = 0; k < n; k++) {
        int p = statuelinks_piece(from[k]);
        if (p < 0 || statuelinks_piece(to[k]) >= 0)
            continue;
        piece *made = new_piece();
        if (!made)
            return;
        snprintf(made->name, sizeof made->name, "%s", g_pieces[p].name);
        made->fresh = 1;
        int fresh = g_pieces_n - 1;
        for (int q = k; q < n; q++)
            if (statuelinks_piece(from[q]) == p && to[q] >= 0 && to[q] < g_n)
                g_piece_of[to[q]] = fresh;
    }
}
