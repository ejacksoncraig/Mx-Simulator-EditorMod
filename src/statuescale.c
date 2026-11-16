/* Resized copies of a statue's files, the same way MX Simulator Tools makes them (its mxtools/core/statue_scale.py and
   track_editor.py _scaled_paths / _write_scaled), so either tool can pick up the other's copies:

       <track>/statue/scaled/<mesh>-150pct/
           <mesh>.jm              scaled about its origin (normals, UVs and triangles as they were)
           <mesh>_lodN.jm         its LODs, scaled (the game finds them by the mesh's name)
           <mesh>_norm.*, _spec.* copied as they were (the game's fallback when the texture has none)
           <shape>.shp            each sphere's x y z radius scaled
           mx-tools-scale.json    {"source": "@pack/x.jm", "factor": 1.5, "shapes": {"x.shp": "@pack/x.shp"}}

   A resize always starts again from the original (the json's source), so sizes don't drift, and 100% names the original
   files again. The files are written straight away (the game must load them now), not on Save; the game opens a loose
   file under an @ path directly from disk, before its .saf index (1684 0x8d450), so a file made while it runs is found.
   Only loose sources can be read here: a mesh inside a .saf pack or gzipped is refused (the app can do those). */
#include "curvetool.h"
#include "json.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SCALED_FOLDER "statue/scaled"
#define SCALE_FILE "mx-tools-scale.json"
#define MAX_SHAPES 16
#define NAME_CAP 1024

/* ── paths ── */

/* "@pack/a\b.jm" -> "pack/a/b.jm" */
static void rel_of(const char *at, char *out, size_t cap)
{
    if (*at == '@')
        at++;
    snprintf(out, cap, "%s", at);
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
}

static int content_root(wchar_t *out, size_t cap)
{
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return 0;
    swprintf(out, cap, L"%ls\\MX Simulator", local);
    return 1;
}

/* root\rel with backslashes. which 0 = the content folder, 1 = the game's own folder. */
static int disk_path(int which, const char *rel, wchar_t *out, size_t cap)
{
    wchar_t root[MAX_PATH], wrel[NAME_CAP];
    if (which == 0) {
        if (!content_root(root, MAX_PATH))
            return 0;
    } else {
        DWORD n = GetModuleFileNameW(NULL, root, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return 0;
        wchar_t *slash = wcsrchr(root, L'\\');
        if (!slash)
            return 0;
        *slash = 0;
    }
    if (!MultiByteToWideChar(CP_ACP, 0, rel, -1, wrel, NAME_CAP))
        return 0;
    for (wchar_t *p = wrel; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    return swprintf(out, cap, L"%ls\\%ls", root, wrel) > 0;
}

static int is_file(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int is_dir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Where an @ file is on disk (the content folder first, as the game looks), or 0. */
static int find_file(const char *at, wchar_t *out, size_t cap)
{
    char rel[NAME_CAP];
    rel_of(at, rel, sizeof rel);
    for (int which = 0; which < 2; which++)
        if (disk_path(which, rel, out, cap) && is_file(out))
            return 1;
    return 0;
}

static char *read_all(const wchar_t *path, size_t *len)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 && n < (256L << 20) ? malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
        free(buf), buf = NULL;
    fclose(f);
    if (buf)
        buf[n] = 0, *len = (size_t)n;
    return buf;
}

static int write_all(const wchar_t *path, const char *data, size_t len)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f)
        return 0;
    int ok = fwrite(data, 1, len, f) == len;
    ok &= fclose(f) == 0;
    if (!ok)
        DeleteFileW(path);
    return ok;
}

/* ── a growing text buffer ── */

typedef struct {
    char *p;
    size_t n, cap;
    int bad;
} text;

static void put(text *t, const char *fmt, ...)
{
    if (t->bad)
        return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int got = vsnprintf(t->p ? t->p + t->n : NULL, t->p ? t->cap - t->n : 0, fmt, ap);
        va_end(ap);
        if (got < 0) {
            t->bad = 1;
            return;
        }
        if (t->p && t->n + (size_t)got < t->cap) {
            t->n += (size_t)got;
            return;
        }
        size_t want = (t->cap ? t->cap * 2 : 1 << 16) + (size_t)got + 1;
        char *p = realloc(t->p, want);
        if (!p) {
            t->bad = 1;
            return;
        }
        t->p = p, t->cap = want;
    }
}

/* ── the two formats ── */

static char *next_token(char **at)
{
    char *p = *at;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (!*p)
        return NULL;
    char *start = p;
    while (*p && !isspace((unsigned char)*p))
        p++;
    if (*p)
        *p++ = 0;
    *at = p;
    return start;
}

/* A JM1 mesh resized about its origin: scale_jm's output, byte for byte where the numbers allow. */
static const char *scale_jm(char *data, double f, text *out)
{
    char *at = data, *tok = next_token(&at);
    if (!tok || strcmp(tok, "JM1") != 0)
        return "Only JM1 meshes can be resized.";
    char *head[5];
    for (int k = 0; k < 5; k++)
        if (!(head[k] = next_token(&at)))
            return "The mesh is cut short or damaged, so it can't be resized.";
    char *vt = next_token(&at), *tt = next_token(&at);
    long verts = vt ? strtol(vt, NULL, 10) : -1, tris = tt ? strtol(tt, NULL, 10) : -1;
    if (verts < 0 || tris < 0 || verts > 10000000 || tris > 10000000)
        return "The mesh is cut short or damaged, so it can't be resized.";
    put(out, "JM1\n%s %s %s %s %s\n%ld %ld\n", head[0], head[1], head[2], head[3], head[4], verts, tris);
    for (long v = 0; v < verts; v++) {
        for (int k = 0; k < 8; k++) {
            char *s = next_token(&at), *end;
            if (!s)
                return "The mesh is cut short or damaged, so it can't be resized.";
            double x = strtod(s, &end);
            if (end == s)
                return "The mesh is cut short or damaged, so it can't be resized.";
            put(out, k ? " %.6f" : "%.6f", k < 3 ? x * f : x);
        }
        put(out, "\n");
    }
    for (long k = 0; k < tris * 3; k++) {
        char *s = next_token(&at);
        if (!s)
            return "The mesh is cut short or damaged, so it can't be resized.";
        put(out, "%ld\n", strtol(s, NULL, 10));
    }
    return out->bad ? "Out of memory." : NULL;
}

/* Collision spheres resized: each `x y z radius friction stiffness damping` line's first four numbers. */
static void scale_shp(char *data, double f, text *out)
{
    char *line = data;
    while (*line) {
        char *eol = line;
        while (*eol && *eol != '\n')
            eol++;
        int more = *eol == '\n';
        *eol = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[--len] = 0;
        char copy[512];
        char *parts[16];
        int n = 0;
        if (len < sizeof copy) {
            memcpy(copy, line, len + 1);
            char *at = copy, *t;
            while (n < 16 && (t = next_token(&at)))
                parts[n++] = t;
        }
        double v[4];
        int nums = n >= 7;
        for (int k = 0; nums && k < 4; k++) {
            char *end;
            v[k] = strtod(parts[k], &end);
            nums = end != parts[k] && *end == 0;
        }
        if (nums) {
            put(out, "%.6f %.6f %.6f %.6f", v[0] * f, v[1] * f, v[2] * f, v[3] * f);
            for (int k = 4; k < n; k++)
                put(out, " %s", parts[k]);
            put(out, "\n");
        } else {
            put(out, "%s\n", line);
        }
        line = more ? eol + 1 : eol;
    }
}

/* ── the record beside a scaled mesh ── */

typedef struct {
    char source[NAME_CAP];
    double factor;
    int shapes;
    char shape_name[MAX_SHAPES][256], shape_source[MAX_SHAPES][NAME_CAP];
} scale_record;

static int in_scaled_folder(const char *at)
{
    char rel[NAME_CAP];
    rel_of(at, rel, sizeof rel);
    for (char *p = rel; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return strstr(rel, "/statue/scaled/") != NULL;
}

/* The folder part of a path, keeping its slash style (rel form). */
static void folder_of(const char *rel, char *out, size_t cap)
{
    snprintf(out, cap, "%s", rel);
    char *slash = strrchr(out, '/');
    if (slash)
        *slash = 0;
    else
        out[0] = 0;
}

static const char *base_of(const char *path)
{
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : path;
}

static int read_record(const char *folder_rel, scale_record *rec)
{
    memset(rec, 0, sizeof *rec);
    char rel[NAME_CAP];
    snprintf(rel, sizeof rel, "%s/%s", folder_rel, SCALE_FILE);
    wchar_t path[MAX_PATH * 2];
    size_t len;
    char *data;
    if (!disk_path(0, rel, path, MAX_PATH * 2) || !(data = read_all(path, &len)))
        return 0;
    json_reader r = {data, data + len, NULL};
    char key[32];
    int first = 1, got_source = 0, got_factor = 0;
    while (json_next_key(&r, key, sizeof key, &first)) {
        if (!strcmp(key, "source")) {
            got_source = json_string(&r, rec->source, sizeof rec->source);
        } else if (!strcmp(key, "factor")) {
            got_factor = json_number(&r, &rec->factor);
        } else if (!strcmp(key, "shapes")) {
            int each = 1;
            char name[256];
            while (json_next_key(&r, name, sizeof name, &each)) {
                if (rec->shapes < MAX_SHAPES) {
                    snprintf(rec->shape_name[rec->shapes], sizeof rec->shape_name[0], "%s", name);
                    if (!json_string(&r, rec->shape_source[rec->shapes], sizeof rec->shape_source[0]))
                        break;
                    rec->shapes++;
                } else if (!json_skip(&r, 1)) {
                    break;
                }
            }
        } else if (!json_skip(&r, 1)) {
            break;
        }
    }
    free(data);
    return !r.error && got_source && got_factor && rec->factor > 0;
}

static void put_json_string(text *t, const char *s)
{
    put(t, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            put(t, "\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            put(t, "\\u%04x", (unsigned char)*s);
        else
            put(t, "%c", *s);
    }
    put(t, "\"");
}

static int write_record(const wchar_t *folder, const scale_record *rec)
{
    text t = {0};
    put(&t, "{\n \"source\": ");
    put_json_string(&t, rec->source);
    put(&t, ",\n \"factor\": %.17g,\n \"shapes\": {", rec->factor);
    for (int k = 0; k < rec->shapes; k++) {
        put(&t, k ? ",\n  " : "\n  ");
        put_json_string(&t, rec->shape_name[k]);
        put(&t, ": ");
        put_json_string(&t, rec->shape_source[k]);
    }
    put(&t, rec->shapes ? "\n }\n}" : "}\n}");
    wchar_t path[MAX_PATH * 2];
    swprintf(path, MAX_PATH * 2, L"%ls\\mx-tools-scale.json", folder);
    int ok = !t.bad && write_all(path, t.p, t.n);
    free(t.p);
    return ok;
}

/* The original a mesh was resized from, and how many times its size it is. Shape likewise (NULL when none). */
static void origin_of(const char *mesh, const char *shape, char *src_mesh, char *src_shape, double *factor)
{
    snprintf(src_mesh, NAME_CAP, "%s", mesh);
    snprintf(src_shape, NAME_CAP, "%s", shape);
    for (char *p = src_mesh; *p; p++)
        if (*p == '\\')
            *p = '/'; /* as the app writes paths */
    for (char *p = src_shape; *p; p++)
        if (*p == '\\')
            *p = '/';
    *factor = 1;
    if (!in_scaled_folder(mesh))
        return;
    char rel[NAME_CAP], folder[NAME_CAP];
    rel_of(mesh, rel, sizeof rel);
    folder_of(rel, folder, sizeof folder);
    static scale_record rec;
    wchar_t found[MAX_PATH * 2];
    if (!read_record(folder, &rec) || !find_file(rec.source, found, MAX_PATH * 2))
        return;
    snprintf(src_mesh, NAME_CAP, "%s", rec.source);
    *factor = rec.factor;
    if (shape[0]) {
        char srel[NAME_CAP], sfolder[NAME_CAP];
        rel_of(shape, srel, sizeof srel);
        folder_of(srel, sfolder, sizeof sfolder);
        if (_stricmp(sfolder, folder) == 0)
            for (int k = 0; k < rec.shapes; k++)
                if (_stricmp(rec.shape_name[k], base_of(srel)) == 0)
                    snprintf(src_shape, NAME_CAP, "%s", rec.shape_source[k]);
    }
}

double statuescale_factor(const char *mesh)
{
    char m[NAME_CAP], s[NAME_CAP];
    double f;
    origin_of(mesh, "", m, s, &f);
    return f;
}

/* ── writing a copy ── */

static int make_dirs(const wchar_t *path)
{
    wchar_t buf[MAX_PATH * 2];
    swprintf(buf, MAX_PATH * 2, L"%ls", path);
    for (wchar_t *p = buf + 3; *p; p++)
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(buf, NULL);
            *p = L'\\';
        }
    CreateDirectoryW(buf, NULL);
    return is_dir(buf);
}

static const char *scale_file(const wchar_t *from, const wchar_t *to, double f, int jm)
{
    size_t len;
    char *data = read_all(from, &len);
    if (!data)
        return "A source file couldn't be read.";
    text out = {0};
    const char *why = NULL;
    if (jm)
        why = scale_jm(data, f, &out);
    else
        scale_shp(data, f, &out);
    if (!why && out.bad)
        why = "Out of memory.";
    if (!why && !write_all(to, out.p, out.n))
        why = "A resized file couldn't be written into the track's folder.";
    free(data), free(out.p);
    return why;
}

static int starts_with_ci(const wchar_t *s, const wchar_t *prefix)
{
    return _wcsnicmp(s, prefix, wcslen(prefix)) == 0;
}

/* The mesh's LODs (scaled) and its _norm / _spec maps (copied), from its folder into the copy's. */
static void siblings(const wchar_t *src_mesh, const wchar_t *dest, double f)
{
    wchar_t dir[MAX_PATH * 2], stem[MAX_PATH], pattern[MAX_PATH * 2];
    swprintf(dir, MAX_PATH * 2, L"%ls", src_mesh);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash)
        return;
    swprintf(stem, MAX_PATH, L"%ls", slash + 1);
    *slash = 0;
    wchar_t *dot = wcsrchr(stem, L'.');
    if (dot)
        *dot = 0;
    wchar_t lod[MAX_PATH], norm[MAX_PATH], spec[MAX_PATH];
    swprintf(lod, MAX_PATH, L"%ls_lod", stem);
    swprintf(norm, MAX_PATH, L"%ls_norm", stem);
    swprintf(spec, MAX_PATH, L"%ls_spec", stem);
    swprintf(pattern, MAX_PATH * 2, L"%ls\\%ls_*", dir, stem);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        wchar_t from[MAX_PATH * 2], to[MAX_PATH * 2];
        swprintf(from, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
        swprintf(to, MAX_PATH * 2, L"%ls\\%ls", dest, fd.cFileName);
        if (is_file(to))
            continue;
        size_t n = wcslen(fd.cFileName);
        if (starts_with_ci(fd.cFileName, lod) && n > 3 && _wcsicmp(fd.cFileName + n - 3, L".jm") == 0) {
            if (scale_file(from, to, f, 1))
                mx_log("scale: a LOD couldn't be resized, so it was left out (the game uses the level before it)");
        } else if (starts_with_ci(fd.cFileName, norm) || starts_with_ci(fd.cFileName, spec)) {
            CopyFileW(from, to, TRUE);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void safe_stem(const char *path, char *out, size_t cap)
{
    const char *b = base_of(path);
    size_t n = 0;
    int last_us = 1; /* strips leading underscores */
    const char *dot = strrchr(b, '.');
    for (const char *p = b; *p && p != dot && n + 1 < cap; p++) {
        int ok = isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-';
        if (ok)
            out[n++] = *p, last_us = 0;
        else if (!last_us)
            out[n++] = '_', last_us = 1;
    }
    while (n && out[n - 1] == '_')
        n--;
    out[n] = 0;
    if (!n)
        snprintf(out, cap, "mesh");
}

const char *statuescale_files(const char *mesh, const char *shape, double by, char *out_mesh, char *out_shape, size_t cap,
                              double *total)
{
    if (!mesh[0])
        return "That statue has no mesh to resize.";
    char src_mesh[NAME_CAP], src_shape[NAME_CAP];
    double was;
    origin_of(mesh, shape, src_mesh, src_shape, &was);
    double f = was * by;
    *total = f;
    if (!(f >= 0.01 && f <= 100))
        return "That would be outside 1% to 10000% of its real size.";
    if (fabs(f - 1) < 1e-6) {
        snprintf(out_mesh, cap, "%s", src_mesh); /* back at its real size: the original files again */
        snprintf(out_shape, cap, "%s", src_shape);
        *total = 1;
        return NULL;
    }
    char track[256];
    if (!game_track(track, sizeof track))
        return "No track is open.";
    wchar_t src_path[MAX_PATH * 2], src_shp_path[MAX_PATH * 2];
    if (!find_file(src_mesh, src_path, MAX_PATH * 2)) {
        mx_log("scale: %s isn't a loose file", src_mesh);
        return "Its mesh isn't a loose file (it's in a .saf pack or gzipped): resize this one in MX Simulator Tools.";
    }
    if (src_shape[0] && !find_file(src_shape, src_shp_path, MAX_PATH * 2))
        return "Its collision shape isn't a loose file (it's in a .saf pack or gzipped): resize it in MX Simulator Tools.";

    /* The folder: <stem>-<pct>pct, or -2, -3... past ones holding something else. */
    char stem[200], base_rel[NAME_CAP], rel[NAME_CAP];
    safe_stem(src_mesh, stem, sizeof stem);
    snprintf(base_rel, sizeof base_rel, "%s/" SCALED_FOLDER "/%s-%.4gpct", track, stem, f * 100);
    static scale_record rec;
    memset(&rec, 0, sizeof rec);
    wchar_t folder[MAX_PATH * 2];
    int reuse = 0;
    for (int n = 1;; n++) {
        if (n > 99)
            return "Too many resized copies of that mesh already.";
        if (n == 1)
            snprintf(rel, sizeof rel, "%s", base_rel);
        else
            snprintf(rel, sizeof rel, "%s-%d", base_rel, n);
        if (!disk_path(0, rel, folder, MAX_PATH * 2))
            return "The track's folder couldn't be found.";
        if (!is_dir(folder))
            break; /* free */
        if (read_record(rel, &rec) && _stricmp(rec.source, src_mesh) == 0 && fabs(rec.factor - f) < 1e-9 * (f > 1 ? f : 1)) {
            reuse = 1;
            break; /* this mesh at this size, made before */
        }
    }
    if (!reuse) {
        memset(&rec, 0, sizeof rec);
        snprintf(rec.source, sizeof rec.source, "%s", src_mesh);
        rec.factor = f;
    }
    if (!make_dirs(folder))
        return "The resized copy's folder couldn't be made in the track.";

    /* The mesh and what follows it by name. */
    const char *mesh_name = base_of(src_mesh);
    wchar_t dest[MAX_PATH * 2], wname[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, mesh_name, -1, wname, MAX_PATH);
    swprintf(dest, MAX_PATH * 2, L"%ls\\%ls", folder, wname);
    if (!is_file(dest)) {
        const char *why = scale_file(src_path, dest, f, 1);
        if (why)
            return why;
        siblings(src_path, folder, f);
    }
    snprintf(out_mesh, cap, "@%s/%s", rel, mesh_name);

    /* The shape. */
    out_shape[0] = 0;
    if (src_shape[0]) {
        const char *shp_name = base_of(src_shape);
        MultiByteToWideChar(CP_ACP, 0, shp_name, -1, wname, MAX_PATH);
        swprintf(dest, MAX_PATH * 2, L"%ls\\%ls", folder, wname);
        if (!is_file(dest)) {
            const char *why = scale_file(src_shp_path, dest, f, 0);
            if (why)
                return why;
        }
        int k = 0;
        while (k < rec.shapes && _stricmp(rec.shape_name[k], shp_name) != 0)
            k++;
        if (k < MAX_SHAPES) {
            snprintf(rec.shape_name[k], sizeof rec.shape_name[0], "%s", shp_name);
            snprintf(rec.shape_source[k], sizeof rec.shape_source[0], "%s", src_shape);
            if (k == rec.shapes)
                rec.shapes++;
        }
        snprintf(out_shape, cap, "@%s/%s", rel, shp_name);
    }
    if (!write_record(folder, &rec))
        mx_log("scale: %s couldn't be written; the copy works, but a later resize can't find its original", SCALE_FILE);
    return NULL;
}
