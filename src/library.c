/* A library of profile sections (a 3 ft jump, a table top...), saved from one gradient's or curve's profile and pasted
   onto another's. One file for every track: %LOCALAPPDATA%\MX Simulator\mxtools\profile-sections.json,
   {"sections": [{"name": "...", "points": [{"type", "s", "h"}]}]}. Sections are kept relative to their first point
   (s and h start at 0), so a paste sits on the profile wherever it goes. Game thread only. */
#include "curvetool.h"
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static section g_sections[SECTION_MAX];
static int g_count, g_loaded;
static const char *g_names[SECTION_MAX];

static int file_path(wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH];
    return mx_data_dir(dir, MAX_PATH) && swprintf(out, cap, L"%ls\\profile-sections.json", dir) > 0;
}

static int read_section(json_reader *r, section *s)
{
    memset(s, 0, sizeof *s);
    char key[16];
    int first = 1;
    while (json_next_key(r, key, sizeof key, &first)) {
        if (!strcmp(key, "name")) {
            if (!json_string(r, s->name, sizeof s->name))
                return 0;
        } else if (!strcmp(key, "points")) {
            int each = 1;
            while (json_next_item(r, &each)) {
                profile_point p = {0, 0, 0};
                double type = 0;
                char k[8];
                int f = 1;
                while (json_next_key(r, k, sizeof k, &f)) {
                    double *at = !strcmp(k, "type") ? &type : !strcmp(k, "s") ? &p.s : !strcmp(k, "h") ? &p.h : NULL;
                    if (at ? !json_number(r, at) : !json_skip(r, 1))
                        return 0;
                }
                if (r->error)
                    return 0;
                p.type = type == 1;
                if (s->points < SECTION_MAX_POINTS)
                    s->p[s->points++] = p;
            }
        } else if (!json_skip(r, 1)) {
            return 0;
        }
        if (r->error)
            return 0;
    }
    return !r->error && s->points >= 2;
}

static void load(void)
{
    g_loaded = 1;
    g_count = 0;
    wchar_t path[MAX_PATH];
    if (!file_path(path, MAX_PATH))
        return;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    static char text[1 << 20];
    DWORD got = 0;
    ReadFile(f, text, sizeof text, &got, NULL);
    CloseHandle(f);
    json_reader r = {text, text + got, NULL};
    char key[16];
    int first = 1;
    while (json_next_key(&r, key, sizeof key, &first)) {
        if (!strcmp(key, "sections")) {
            int each = 1;
            while (json_next_item(&r, &each))
                if (g_count < SECTION_MAX && read_section(&r, &g_sections[g_count]))
                    g_count++;
                else if (r.error)
                    break;
        } else if (!json_skip(&r, 1)) {
            break;
        }
        if (r.error)
            break;
    }
    if (r.error)
        mx_log("profile sections: %s (kept %d)", r.error, g_count);
    else
        mx_log("profile sections: %d", g_count);
}

static int save(void)
{
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    if (!file_path(path, MAX_PATH))
        return 0;
    swprintf(tmp, MAX_PATH, L"%ls.tmp", path);
    static char text[1 << 20];
    size_t n = 0;
#define PUT(...)                                                                     \
    do {                                                                             \
        int wrote_ = snprintf(text + n, sizeof text - n, __VA_ARGS__);              \
        if (wrote_ < 0 || n + (size_t)wrote_ >= sizeof text)                         \
            return 0;                                                                \
        n += (size_t)wrote_;                                                         \
    } while (0)
    PUT("{\"sections\": [");
    for (int i = 0; i < g_count; i++) {
        PUT("%s\n {\"name\": \"", i ? "," : "");
        for (const char *c = g_sections[i].name; *c; c++)
            PUT(*c == '"' || *c == '\\' ? "\\%c" : "%c", *c);
        PUT("\", \"points\": [");
        for (int k = 0; k < g_sections[i].points; k++)
            PUT("%s{\"type\": %d, \"s\": %.17g, \"h\": %.17g}", k ? ", " : "", g_sections[i].p[k].type, g_sections[i].p[k].s,
                g_sections[i].p[k].h);
        PUT("]}");
    }
    PUT("\n]}\n");
#undef PUT
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, (DWORD)n, &put, NULL) && put == n;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    return ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

int library_count(void)
{
    if (!g_loaded)
        load();
    return g_count;
}

const char *const *library_names(void)
{
    library_count();
    for (int i = 0; i < g_count; i++)
        g_names[i] = g_sections[i].name;
    return g_names;
}

const section *library_get(int i) { return i >= 0 && i < library_count() ? &g_sections[i] : NULL; }

/* Points first..last of a profile, made relative to the first, kept under a name from its shape. */
const char *library_add(const profile_point *p, int first, int last)
{
    if (library_count() >= SECTION_MAX)
        return "The section library is full; delete one first.";
    if (first < 0 || last <= first)
        return "Mark a start point, then an end point further along.";
    if (last - first + 1 > SECTION_MAX_POINTS)
        return "That section has too many points to keep.";
    section *s = &g_sections[g_count];
    memset(s, 0, sizeof *s);
    double lo = 0, hi = 0;
    for (int i = first; i <= last; i++) {
        profile_point q = {p[i].type, p[i].s - p[first].s, p[i].h - p[first].h};
        s->p[s->points++] = q;
        lo = q.h < lo ? q.h : lo, hi = q.h > hi ? q.h : hi;
    }
    /* Short enough to fit a panel row; Rename Section gives it a real name. */
    double length = s->p[s->points - 1].s, end = s->p[s->points - 1].h;
    if (fabs(end) > 0.05)
        snprintf(s->name, sizeof s->name, "%.1f ft %s, %.0f ft", fabs(end), end > 0 ? "up" : "down", length);
    else if (hi - lo < 0.05)
        snprintf(s->name, sizeof s->name, "Flat, %.0f ft", length);
    else
        snprintf(s->name, sizeof s->name, "%.1f ft %s, %.0f ft", hi > -lo ? hi : -lo, hi > -lo ? "high" : "deep", length);
    g_count++;
    if (!save()) {
        g_count--;
        return "Couldn't write the section library.";
    }
    return NULL;
}

const char *library_rename(int i, const char *name)
{
    if (i < 0 || i >= library_count())
        return "No section to rename.";
    if (!name[0])
        return "A name can't be empty.";
    char old[sizeof g_sections[0].name];
    memcpy(old, g_sections[i].name, sizeof old);
    snprintf(g_sections[i].name, sizeof g_sections[i].name, "%s", name);
    if (!save()) {
        memcpy(g_sections[i].name, old, sizeof old);
        return "Couldn't write the section library.";
    }
    return NULL;
}

const char *library_delete(int i)
{
    if (i < 0 || i >= library_count())
        return "No section to delete.";
    section removed = g_sections[i];
    memmove(&g_sections[i], &g_sections[i + 1], (size_t)(g_count - i - 1) * sizeof g_sections[0]);
    g_count--;
    if (!save()) {
        memmove(&g_sections[i + 1], &g_sections[i], (size_t)(g_count - i) * sizeof g_sections[0]);
        g_sections[i] = removed;
        g_count++;
        return "Couldn't write the section library.";
    }
    return NULL;
}

/* The section laid onto a profile from point `at`: its first point there (at that point's height), the points it
   covers replaced, the rest kept. The profile's length doesn't change. The new count, or -1 and *why. */
int section_paste(const profile_point *p, int n, int at, const section *s, int room, profile_point *out, const char **why)
{
    if (at < 0 || at >= n) {
        *why = "Pick the point to paste at first.";
        return -1;
    }
    double s0 = p[at].s, h0 = p[at].h, end = s0 + s->p[s->points - 1].s;
    if (end > p[n - 1].s + 1e-6) {
        *why = "The section runs past the end of the profile; pick a point earlier on.";
        return -1;
    }
    int count = 0;
    for (int i = 0; i < at; i++)
        out[count++] = p[i];
    for (int k = 0; k < s->points; k++) {
        profile_point q = {s->p[k].type, s0 + s->p[k].s, h0 + s->p[k].h};
        out[count++] = q;
    }
    /* The profile's own end stays its end: if the section reaches it, the section's last point is the end. */
    for (int i = at + 1; i < n; i++)
        if (p[i].s > end + 1e-6)
            out[count++] = p[i];
    if (count > room) {
        *why = "Not enough room in this profile for the section (the game gives each gradient a fixed number of points).";
        return -1;
    }
    out[count - 1].s = p[n - 1].s; /* exactly the same length as before */
    return count;
}
