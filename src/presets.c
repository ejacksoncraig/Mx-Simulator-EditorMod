/* Named presets kept across tracks: the curve tool's settings (Inner, Outer, Bank, Strength, Bake As, SubMode) and the
   game brush's (its outer and inner radius in feet, our Brush Shape, Angle, Oval Width, Triangle Width and Lean, and the radial Sweep). One
   file: %LOCALAPPDATA%\MX Simulator\mxtools\presets.json, {"curve": [{"name": "...", "inner": 5, ...}], "brush": [...]}.
   A field the file doesn't have (an older file, or one written by hand) stays NAN, and using the preset leaves that
   setting as it is. Game thread only. */
#include "curvetool.h"
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const KIND_KEYS[PRESET_KINDS] = {"curve", "brush"};
static const char *const FIELDS[PRESET_KINDS][PRESET_FIELDS] = {
    {"inner", "outer", "bank", "strength", "add", "limit", "bakeSide"},
    {"outerFt", "innerFt", "shape", "angle", "oval", "sweep", "side", "triWidth", "triLean"},
};
static preset g_presets[PRESET_KINDS][PRESET_MAX];
static int g_count[PRESET_KINDS], g_loaded;
static const char *g_names[PRESET_KINDS][PRESET_MAX];

static int file_path(wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH];
    return mx_data_dir(dir, MAX_PATH) && swprintf(out, cap, L"%ls\\presets.json", dir) > 0;
}

static int read_preset(json_reader *r, int kind, preset *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < PRESET_FIELDS; i++)
        p->v[i] = NAN;
    char key[24];
    int first = 1;
    while (json_next_key(r, key, sizeof key, &first)) {
        int field = -1;
        for (int i = 0; i < PRESET_FIELDS; i++)
            if (FIELDS[kind][i] && !strcmp(key, FIELDS[kind][i]))
                field = i;
        if (!strcmp(key, "name")) {
            if (!json_string(r, p->name, sizeof p->name))
                return 0;
        } else if (field >= 0) {
            if (!json_number(r, &p->v[field]))
                return 0;
        } else if (!json_skip(r, 1)) {
            return 0;
        }
        if (r->error)
            return 0;
    }
    return !r->error && p->name[0];
}

static void load(void)
{
    g_loaded = 1;
    g_count[0] = g_count[1] = 0;
    wchar_t path[MAX_PATH];
    if (!file_path(path, MAX_PATH))
        return;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    static char text[1 << 18];
    DWORD got = 0;
    ReadFile(f, text, sizeof text, &got, NULL);
    CloseHandle(f);
    json_reader r = {text, text + got, NULL};
    char key[16];
    int first = 1;
    while (json_next_key(&r, key, sizeof key, &first)) {
        int kind = !strcmp(key, KIND_KEYS[0]) ? 0 : !strcmp(key, KIND_KEYS[1]) ? 1 : -1;
        if (kind < 0) {
            if (!json_skip(&r, 1))
                break;
            continue;
        }
        int each = 1;
        while (json_next_item(&r, &each))
            if (g_count[kind] < PRESET_MAX && read_preset(&r, kind, &g_presets[kind][g_count[kind]]))
                g_count[kind]++;
            else if (r.error)
                break;
        if (r.error)
            break;
    }
    mx_log("presets: %d curve, %d brush%s%s", g_count[0], g_count[1], r.error ? "; " : "", r.error ? r.error : "");
}

static int save(void)
{
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    if (!file_path(path, MAX_PATH))
        return 0;
    swprintf(tmp, MAX_PATH, L"%ls.tmp", path);
    static char text[1 << 18];
    size_t n = 0;
#define PUT(...)                                                                     \
    do {                                                                             \
        int wrote_ = snprintf(text + n, sizeof text - n, __VA_ARGS__);              \
        if (wrote_ < 0 || n + (size_t)wrote_ >= sizeof text)                         \
            return 0;                                                                \
        n += (size_t)wrote_;                                                         \
    } while (0)
    PUT("{");
    for (int kind = 0; kind < PRESET_KINDS; kind++) {
        PUT("%s\"%s\": [", kind ? ",\n" : "", KIND_KEYS[kind]);
        for (int i = 0; i < g_count[kind]; i++) {
            const preset *p = &g_presets[kind][i];
            PUT("%s\n {\"name\": \"", i ? "," : "");
            for (const char *c = p->name; *c; c++)
                PUT(*c == '"' || *c == '\\' ? "\\%c" : "%c", *c);
            PUT("\"");
            for (int k = 0; k < PRESET_FIELDS; k++)
                if (FIELDS[kind][k] && !isnan(p->v[k]))
                    PUT(", \"%s\": %.17g", FIELDS[kind][k], p->v[k]);
            PUT("}");
        }
        PUT("\n]");
    }
    PUT("}\n");
#undef PUT
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, (DWORD)n, &put, NULL) && put == n;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    return ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static int valid_kind(int kind) { return kind >= 0 && kind < PRESET_KINDS; }

int preset_count(int kind)
{
    if (!g_loaded)
        load();
    return valid_kind(kind) ? g_count[kind] : 0;
}

const char *const *preset_names(int kind)
{
    int n = preset_count(kind);
    for (int i = 0; i < n; i++)
        g_names[kind][i] = g_presets[kind][i].name;
    return valid_kind(kind) ? g_names[kind] : NULL;
}

const preset *preset_get(int kind, int i) { return i >= 0 && i < preset_count(kind) ? &g_presets[kind][i] : NULL; }

/* A name from the values, short enough for a panel row; Rename gives it a real one. */
static void name_from(int kind, const double *v, char *out, size_t cap)
{
    if (kind == PRESET_CURVE) {
        snprintf(out, cap, "Inner %g, Outer %g", v[PC_INNER], v[PC_OUTER]);
        return;
    }
    static const char *const SHAPES[] = {"Circle", "Square", "Oval", "Triangle"};
    int shape = v[PB_SHAPE] >= 0 && v[PB_SHAPE] <= 3 ? (int)v[PB_SHAPE] : 0;
    snprintf(out, cap, "%g/%g ft %s, %g sweep", v[PB_OUTER], v[PB_INNER], SHAPES[shape], v[PB_SWEEP]);
}

const char *preset_add(int kind, const double *v)
{
    if (!valid_kind(kind))
        return "No such preset list.";
    if (preset_count(kind) >= PRESET_MAX)
        return "The preset list is full; delete one first.";
    preset *p = &g_presets[kind][g_count[kind]];
    memset(p, 0, sizeof *p);
    for (int i = 0; i < PRESET_FIELDS; i++)
        p->v[i] = FIELDS[kind][i] ? v[i] : NAN;
    name_from(kind, v, p->name, sizeof p->name);
    g_count[kind]++;
    if (!save()) {
        g_count[kind]--;
        return "Couldn't write the presets file.";
    }
    return NULL;
}

const char *preset_rename(int kind, int i, const char *name)
{
    if (i < 0 || i >= preset_count(kind))
        return "No preset to rename.";
    if (!name[0])
        return "A name can't be empty.";
    preset *p = &g_presets[kind][i];
    char old[sizeof p->name];
    memcpy(old, p->name, sizeof old);
    snprintf(p->name, sizeof p->name, "%s", name);
    if (!save()) {
        memcpy(p->name, old, sizeof old);
        return "Couldn't write the presets file.";
    }
    return NULL;
}

/* A deleted preset put back at its old place (Undo Delete Preset). */
const char *preset_insert(int kind, int i, const preset *p)
{
    if (!valid_kind(kind))
        return "No such preset list.";
    if (preset_count(kind) >= PRESET_MAX)
        return "The preset list is full; delete one first.";
    preset *list = g_presets[kind];
    i = i < 0 ? 0 : i > g_count[kind] ? g_count[kind] : i;
    memmove(&list[i + 1], &list[i], (size_t)(g_count[kind] - i) * sizeof list[0]);
    list[i] = *p;
    g_count[kind]++;
    if (!save()) {
        memmove(&list[i], &list[i + 1], (size_t)(g_count[kind] - i - 1) * sizeof list[0]);
        g_count[kind]--;
        return "Couldn't write the presets file.";
    }
    return NULL;
}

const char *preset_delete(int kind, int i)
{
    if (i < 0 || i >= preset_count(kind))
        return "No preset to delete.";
    preset removed = g_presets[kind][i];
    preset *list = g_presets[kind];
    memmove(&list[i], &list[i + 1], (size_t)(g_count[kind] - i - 1) * sizeof list[0]);
    g_count[kind]--;
    if (!save()) {
        memmove(&list[i + 1], &list[i], (size_t)(g_count[kind] - i) * sizeof list[0]);
        list[i] = removed;
        g_count[kind]++;
        return "Couldn't write the presets file.";
    }
    return NULL;
}
