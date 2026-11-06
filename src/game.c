/* What the mod knows about mx.exe, and the game-thread side of every request.

   v1 is pinned to one build (SHA-256 below). Addresses are RVAs (offsets from the exe's load address), so ASLR
   doesn't matter. Every address and offset is from docs/design.md ("What the mod touches").
   Reads (HELLO, GET_HEIGHTS) and, from M2, height writes (SET_HEIGHTS). Every write happens here, on the game's own
   thread inside the SwapBuffers hook, and copies what the editor's brush does (mx.exe 0x140066bf0 case 4).
   MXMOD_READONLY=1 in the environment turns every write into a log line. */
#include "mod.h"
#include "curvetool.h"
#include "protocol.h"

#include <bcrypt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── what the game has, for whichever known build is running (builds.c) ── */
#define RVA_EDITOR_TERRAIN (g_build->rva_editor_terrain) /* void* editor terrain object, null without a track */
#define RVA_EDITOR_MODE (g_build->rva_editor_mode)    /* int, 0..15 */
#define RVA_MOUSE_DOWN (g_build->rva_mouse_down)     /* int, nonzero while a mouse button is held in the editor */
#define RVA_GRADIENT_COUNT (g_build->rva_gradient_count) /* int */
#define RVA_GRADIENTS (g_build->rva_gradients)      /* game_gradient* array */
#define RVA_GRADIENT_CAP (g_build->rva_gradient_cap)   /* int, room in the array */
#define RVA_GRADIENT_STRIDE (g_build->rva_gradient_stride) /* int, bytes per record (64) */
#define RVA_SELECTED_POINT (g_build->rva_selected_point) /* int, profile point picked in Gradient Edit, -1 for none */
#define RVA_SELECTED_GRADIENT (g_build->rva_selected_gradient) /* int, -1 for none */
#define RVA_UNDO_PENDING (g_build->rva_undo_pending)   /* int, nonzero = an edit is waiting for its undo snapshot */
#define RVA_STATUS_TEXT (g_build->rva_status_text)    /* const char*, the line shown in the editor panel */
#define RVA_FN_MARK_DIRTY (g_build->rva_fn_mark_dirty)    /* void(terrain*, col0, row0, col1, row1): flag 128-cell blocks for rebuild */
#define RVA_FN_UNDO_SNAPSHOT (g_build->rva_fn_undo_snapshot) /* void(void): if UNDO_PENDING, push current heights into the undo ring */

#define T_HEIGHTS 0x0u        /* float* */
#define T_SIZE_FT 0x10c8u     /* float, map side in feet */
#define T_CELL_FT 0x10ccu     /* float */
#define T_MAX_FT 0x10f4u      /* float */
#define T_MIN_FT 0x10f8u      /* float */
#define T_WIDTH 0x49508u      /* int, heightmap is width x width */
#define T_CHANGES 0x4953cu    /* int, bumped on every edit */
#define T_LOCKED 0x49574u     /* int, nonzero = editing locked */
#define T_FLAGS 0x49570u      /* int, every editor edit sets 7 */

/* One editor gradient (docs/game/editor-gradients-ui.md section 7). Checked against ModTest's edinfo, all 93 records. */
typedef struct {
    void *data;
    int32_t capacity, count, stride;
} game_vector;

typedef struct {
    float ax, az, bx, bz;
    game_vector points; /* float s, h; stride 8 */
    game_vector types;  /* uint8, 1 = Curve to the next point; stride 1 */
} game_gradient;

/* Compile-time layout checks (a negative array size fails the build). */
typedef char game_vector_is_24_bytes[sizeof(game_vector) == 24 ? 1 : -1];
typedef char game_gradient_is_64_bytes[sizeof(game_gradient) == 64 ? 1 : -1];

typedef void (*mark_dirty_fn)(void *terrain, int col0, int row0, int col1, int row1);
typedef void (*undo_snapshot_fn)(void);

static const char STATUS_APPLIED[] = "Applied from MX Simulator Tools";
static const char STATUS_GRADIENT[] = "Gradient changed by MX Simulator Tools";

/* ── state ── */
static uintptr_t g_base;
static int g_editor;       /* --editor on the command line */
static int g_started;
static int g_enabled;      /* editor + known build: the pipe runs */
static int g_readonly;     /* MXMOD_READONLY=1: writes are logged, not done */
static char g_sha[65];
static LONG g_frames, g_clears, g_swaps;

/* One request at a time from the pipe thread to the game thread. */
enum { REQ_IDLE, REQ_PENDING, REQ_BUSY, REQ_DONE };
static volatile LONG g_req_state = REQ_IDLE;
static uint32_t g_req_type, g_req_len;
static const unsigned char *g_req_payload; /* owned by the pipe thread, valid until the request is DONE */
static uint32_t g_reply_type, g_reply_len;
static void *g_reply;
static HANDLE g_req_done;

int mx_readable(const void *p, size_t n)
{
    const unsigned char *at = (const unsigned char *)p, *end = at + n;
    if (!p || end < at)
        return 0;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(at, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
            return 0;
        DWORD prot = mbi.Protect & 0xff;
        if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || prot == PAGE_EXECUTE)
            return 0;
        at = (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

static int read_i32(uintptr_t addr, int *out)
{
    if (!mx_readable((void *)addr, 4))
        return 0;
    *out = *(volatile int *)addr;
    return 1;
}

static int read_f32(uintptr_t addr, float *out)
{
    if (!mx_readable((void *)addr, 4))
        return 0;
    *out = *(volatile float *)addr;
    return 1;
}

static int read_ptr(uintptr_t addr, uintptr_t *out)
{
    if (!mx_readable((void *)addr, sizeof(void *)))
        return 0;
    *out = *(volatile uintptr_t *)addr;
    return 1;
}

static int file_sha256(const wchar_t *path, char hex[65])
{
    int ok = 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        static unsigned char buf[1 << 16];
        DWORD got;
        ok = 1;
        while (ReadFile(f, buf, sizeof buf, &got, NULL) && got > 0)
            if (BCryptHashData(hash, buf, got, 0) != 0) {
                ok = 0;
                break;
            }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
            for (int i = 0; i < 32; i++)
                sprintf(hex + 2 * i, "%02x", digest[i]);
            hex[64] = 0;
        } else {
            ok = 0;
        }
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

void mod_on_attach(void)
{
    /* Inside DllMain: only cheap reads. Files, hashing and threads wait for the first frame. */
    g_base = (uintptr_t)GetModuleHandleW(NULL);
    g_editor = wcsstr(GetCommandLineW(), L"--editor") != NULL;
}

/* Tell the person once, in an editor session, that this game version isn't one the mod knows, so they aren't left
   wondering where the tools went. From its own thread, so the game keeps drawing behind the box. */
static DWORD WINAPI say_unsupported(LPVOID unused)
{
    (void)unused;
    MessageBoxA(NULL,
                "This version of MX Simulator isn't supported by the MX Simulator Tools editor mod yet (the mod was made "
                "for a different game build).\n\nThe editor works normally, just without the mod's tools. Look for an "
                "updated version of the mod.",
                "MX Simulator Tools", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

static void start(void)
{
    mx_log_open();
    mx_log("mxmod %s (protocol %d) loaded, real GL: %ls", MXMOD_VERSION, MXMOD_PROTOCOL_VERSION, mx_real_gl_path());
    mx_log("command line: %ls", GetCommandLineW());
    if (!tripwire_armed())
        tripwire_arm(1); /* ws2_32 wasn't loaded yet when the DLL attached */
    /* MXMOD_PRETEND_UNARMED=1: act as if it couldn't be armed, to see the fail-closed path (it really is armed). */
    char unarmed[8];
    DWORD un = GetEnvironmentVariableA("MXMOD_PRETEND_UNARMED", unarmed, sizeof unarmed);
    int pretend_unarmed = un > 0 && un < sizeof unarmed && strcmp(unarmed, "0") != 0;
    mx_log("online tripwire %s%s", tripwire_note(),
           pretend_unarmed ? " (MXMOD_PRETEND_UNARMED: treated as not armed)"
                           : tripwire_armed() ? ": the game can't send or connect to anything while this DLL is loaded" : "");
    if (!tripwire_armed() || pretend_unarmed)
        tripwire_fail_closed(); /* never runs on with the network open */
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || !file_sha256(exe, g_sha)) {
        mx_log("could not hash %ls: pass-through only", exe);
        return;
    }
    const mx_build *build = mx_build_find(g_sha);
    /* MXMOD_PRETEND_UNKNOWN=1: behave as on a game version the mod doesn't know, to see what someone else would. */
    char pretend[8];
    DWORD pn = GetEnvironmentVariableA("MXMOD_PRETEND_UNKNOWN", pretend, sizeof pretend);
    if (pn > 0 && pn < sizeof pretend && strcmp(pretend, "0") != 0)
        build = NULL;
    int known = build != NULL;
    mx_log("exe %ls sha256 %s (%s%s)", exe, g_sha, known ? "known build: " : "UNKNOWN build", known ? build->name : "");
    if (!g_editor) {
        mx_log("no --editor on the command line: pass-through only (the mod never runs outside the editor)");
        return;
    }
    if (!known) {
        mx_log("unknown mx.exe build: pass-through only. Add its table to builds.c (tools/port_build.py) first.");
        HANDLE t = CreateThread(NULL, 0, say_unsupported, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        return;
    }
    char ro[8];
    DWORD rn = GetEnvironmentVariableA("MXMOD_READONLY", ro, sizeof ro);
    g_readonly = rn > 0 && rn < sizeof ro && strcmp(ro, "0") != 0;
    mx_log("writes %s", g_readonly ? "OFF (MXMOD_READONLY is set): every write is only logged" : "on");
    g_build = build;
    g_req_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_enabled = 1;
    pipe_start();
    ingame_start(g_base); /* on the game thread, between frames: neither hooked function is running */
}

/* ── reading the editor's state (game thread only) ── */

typedef struct {
    int present, width, locked, mode, gradients, changes, mouse_down;
    float size_ft, cell_ft, min_ft, max_ft;
    uintptr_t terrain, heights;
    char problem[160];
} track_info;

static void read_track(track_info *t)
{
    memset(t, 0, sizeof *t);
    read_i32(g_base + RVA_EDITOR_MODE, &t->mode);
    read_i32(g_base + RVA_GRADIENT_COUNT, &t->gradients);
    read_i32(g_base + RVA_MOUSE_DOWN, &t->mouse_down);
    if (!read_ptr(g_base + RVA_EDITOR_TERRAIN, &t->terrain) || !t->terrain) {
        snprintf(t->problem, sizeof t->problem, "no track loaded in the editor");
        return;
    }
    uintptr_t b = t->terrain;
    if (!read_ptr(b + T_HEIGHTS, &t->heights) || !read_i32(b + T_WIDTH, &t->width) ||
        !read_f32(b + T_SIZE_FT, &t->size_ft) || !read_f32(b + T_CELL_FT, &t->cell_ft) ||
        !read_f32(b + T_MIN_FT, &t->min_ft) || !read_f32(b + T_MAX_FT, &t->max_ft) ||
        !read_i32(b + T_CHANGES, &t->changes) || !read_i32(b + T_LOCKED, &t->locked)) {
        snprintf(t->problem, sizeof t->problem, "terrain object at %p is not readable", (void *)b);
        return;
    }
    /* Sanity: width is 2^k + 1, cell > 0, size = (width - 1) * cell. If not, the addresses are wrong. */
    int w = t->width - 1;
    int pow2 = w >= 128 && w <= 16384 && (w & (w - 1)) == 0;
    float expect = (float)w * t->cell_ft;
    if (!pow2 || !(t->cell_ft > 0.0f && t->cell_ft < 100.0f) || !(expect > 0) ||
        (t->size_ft - expect) * (t->size_ft - expect) > (0.01f * expect) * (0.01f * expect) ||
        !mx_readable((void *)t->heights, (size_t)t->width * t->width * sizeof(float))) {
        snprintf(t->problem, sizeof t->problem, "terrain values look wrong (width %d, cell %g, size %g)",
                 t->width, t->cell_ft, t->size_ft);
        return;
    }
    t->present = 1;
}

static void reply_error(const char *msg)
{
    char *json = malloc(256);
    if (!json)
        return;
    snprintf(json, 256, "{\"error\": \"%s\"}", msg);
    g_reply_type = MXMOD_MSG_ERROR;
    g_reply = json;
    g_reply_len = (uint32_t)strlen(json);
}

/* The game's command line as a JSON string body (UTF-8, quotes and backslashes escaped), so the app can tell which
   track it was started with. Cut short rather than overflow. */
static void command_line_json(char *out, size_t cap)
{
    char utf8[2048];
    if (!WideCharToMultiByte(CP_UTF8, 0, GetCommandLineW(), -1, utf8, sizeof utf8, NULL, NULL))
        utf8[0] = 0;
    utf8[sizeof utf8 - 1] = 0;
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)utf8; *c && n + 7 < cap; c++) {
        if (*c == '"' || *c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)*c;
        } else if (*c < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", *c);
        } else {
            out[n++] = (char)*c;
        }
    }
    out[n] = 0;
}

static void handle_hello(void)
{
    track_info t;
    read_track(&t);
    enum { CAP = 8192 };
    char *json = malloc(CAP);
    if (!json)
        return;
    char cmd[4096], dbg[512];
    command_line_json(cmd, sizeof cmd);
    ingame_debug_json(dbg, sizeof dbg);
    int n = snprintf(json, CAP,
        "{\"protocol\": %d, \"mod\": \"%s\", \"exeSha256\": \"%s\", \"exeKnown\": true, \"editor\": true, \"commandLine\": \"%s\", "
        "\"readonly\": %s, \"hooks\": \"%s\", \"toolPresses\": %ld, \"ingame\": %s, "
        "\"frames\": %ld, \"swaps\": %ld, \"clears\": %ld, \"mode\": %d, \"gradients\": %d, \"mouseDown\": %s, ",
        MXMOD_PROTOCOL_VERSION, MXMOD_VERSION, g_sha, cmd, g_readonly ? "true" : "false", ingame_status(), ingame_presses(), dbg,
        g_frames, g_swaps, g_clears, t.mode, t.gradients,
        t.mouse_down ? "true" : "false");
    char name[260] = "", name_json[600] = "";
    if (game_track(name, sizeof name))
        for (size_t i = 0, k = 0; name[i] && k + 3 < sizeof name_json; i++) {
            if (name[i] == '"' || name[i] == '\\')
                name_json[k++] = '\\';
            name_json[k++] = name[i] >= 32 ? name[i] : '?';
            name_json[k] = 0;
        }
    if (t.present)
        n += snprintf(json + n, CAP - n,
            "\"track\": {\"name\": \"%s\", \"width\": %d, \"cellFt\": %.9g, \"sizeFt\": %.9g, \"minFt\": %.9g, \"maxFt\": %.9g, "
            "\"locked\": %s, \"changes\": %d}}",
            name_json, t.width, t.cell_ft, t.size_ft, t.min_ft, t.max_ft, t.locked ? "true" : "false", t.changes);
    else
        n += snprintf(json + n, CAP - n, "\"track\": null, \"problem\": \"%s\"}", t.problem);
    if (n >= CAP)
        n = CAP - 1;
    g_reply_type = MXMOD_MSG_HELLO;
    g_reply = json;
    g_reply_len = (uint32_t)n;
}

static void handle_get_heights(void)
{
    if (g_req_len != 16) {
        reply_error("GET_HEIGHTS needs 16 bytes: col0, row0, cols, rows");
        return;
    }
    int32_t r[4];
    memcpy(r, g_req_payload, 16);
    track_info t;
    read_track(&t);
    if (!t.present) {
        reply_error(t.problem);
        return;
    }
    int col0 = r[0], row0 = r[1], cols = r[2], rows = r[3];
    if (col0 < 0 || row0 < 0 || cols <= 0 || rows <= 0 || col0 + cols > t.width || row0 + rows > t.width) {
        reply_error("rectangle is outside the heightmap");
        return;
    }
    if ((uint64_t)cols * (uint64_t)rows > MXMOD_MAX_FLOATS_PER_REQUEST) {
        reply_error("rectangle too big for one request; ask in strips");
        return;
    }
    size_t count = (size_t)cols * rows;
    unsigned char *out = malloc(8 + count * 4);
    if (!out) {
        reply_error("out of memory");
        return;
    }
    uint32_t dims[2] = {(uint32_t)cols, (uint32_t)rows};
    memcpy(out, dims, 8);
    const float *src = (const float *)t.heights;
    float *dst = (float *)(out + 8);
    for (int row = 0; row < rows; row++)
        memcpy(dst + (size_t)row * cols, src + (size_t)(row0 + row) * t.width + col0, (size_t)cols * 4);
    g_reply_type = MXMOD_MSG_HEIGHTS;
    g_reply = out;
    g_reply_len = (uint32_t)(8 + count * 4);
}

void game_status(const char *text) { *(const char **)(g_base + RVA_STATUS_TEXT) = text; }

/* The open track's folder name, as the game's own save builds its paths ("@" + this + "/edinfo", 0x14005f243 and
   0x140018dce): the char * at +0x28 of the window struct (*(ptr*)0x14023d7e0). "@" is %LOCALAPPDATA%\MX Simulator.
   Copied into out; 0 when no track is open or it can't be read. */
#define RVA_PTR_WINDOW_STRUCT (g_build->rva_ptr_window_struct)
#define W_TRACK_NAME 0x28u
int game_track(char *out, size_t cap)
{
    uintptr_t window = 0, name = 0;
    if (!read_ptr(g_base + RVA_PTR_WINDOW_STRUCT, &window) || !window || !read_ptr(window + W_TRACK_NAME, &name) || !name)
        return 0;
    size_t n = 0;
    while (n + 1 < cap) {
        if (!mx_readable((const void *)(name + n), 1))
            return 0;
        char c = *(const char *)(name + n);
        if (!c)
            break;
        out[n++] = c;
    }
    out[n] = 0;
    return n > 0;
}

int game_heights(game_map *out)
{
    track_info t;
    read_track(&t);
    if (!t.present)
        return 0;
    out->heights = (const float *)t.heights, out->width = t.width, out->cell_ft = t.cell_ft;
    out->locked = t.locked;
    return 1;
}

/* The write sequence is the brush's: flush the user's pending undo (first), write, widen min/max, flag the edit, mark
   the blocks dirty, then one undo snapshot and a status line (final). Game thread only. NULL on success, else why.
   See docs/design.md, "Applying a height patch". */
const char *game_write_heights(int col0, int row0, int cols, int rows, const float *src, int first, int final)
{
    track_info t;
    read_track(&t);
    if (!t.present)
        return "no track loaded in the editor";
    if (t.locked)
        return "the track is locked for editing in the game";
    if (t.mouse_down)
        return "a mouse button is held in the editor; let go and try again";
    if (cols <= 0 || rows <= 0 || col0 < 0 || row0 < 0 || col0 + cols > t.width || row0 + rows > t.width)
        return "rectangle is outside the heightmap";
    size_t count = (size_t)cols * rows;
    float lo = src[0], hi = src[0];
    for (size_t i = 0; i < count; i++) {
        float v = src[i];
        if (!(v > -1e6f && v < 1e6f)) /* also catches NaN and inf */
            return "heights must be finite and within +-1e6 ft";
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    uintptr_t b = t.terrain;
    if (g_readonly) {
        mx_log("READONLY: would write %dx%d at (%d, %d), %.3f..%.3f ft, first %d final %d", cols, rows, col0, row0, lo, hi,
               first, final);
        return NULL;
    }
    int *pending = (int *)(g_base + RVA_UNDO_PENDING);
    undo_snapshot_fn snapshot = (undo_snapshot_fn)(g_base + RVA_FN_UNDO_SNAPSHOT);
    /* 1b: an unsnapshotted user edit (gradient paint modes) goes into the undo ring on its own first. */
    if (first && *pending != 0)
        snapshot();
    float *heights = (float *)t.heights;
    for (int row = 0; row < rows; row++)
        memcpy(heights + (size_t)(row0 + row) * t.width + col0, src + (size_t)row * cols, (size_t)cols * 4);
    if (hi > *(float *)(b + T_MAX_FT))
        *(float *)(b + T_MAX_FT) = hi;
    if (lo < *(float *)(b + T_MIN_FT))
        *(float *)(b + T_MIN_FT) = lo;
    *(int *)(b + T_FLAGS) = 7;
    *(int *)(b + T_CHANGES) += 1;
    /* One cell of margin: the cells around the patch get new normals too. */
    int c0 = col0 > 0 ? col0 - 1 : 0, r0 = row0 > 0 ? row0 - 1 : 0;
    int c1 = col0 + cols < t.width ? col0 + cols : t.width - 1;
    int r1 = row0 + rows < t.width ? row0 + rows : t.width - 1;
    ((mark_dirty_fn)(g_base + RVA_FN_MARK_DIRTY))((void *)b, c0, r0, c1, r1);
    if (final) {
        *pending = 1;
        snapshot();
        *(const char **)(g_base + RVA_STATUS_TEXT) = STATUS_APPLIED;
        mx_log("applied an edit, last strip %dx%d at (%d, %d)", cols, rows, col0, row0);
    }
    return NULL;
}

/* SET_HEIGHTS: i32 col0, row0, cols, rows, u32 flags, then cols*rows float32, written with game_write_heights. */
static void handle_set_heights(void)
{
    if (g_req_len < 20) {
        reply_error("SET_HEIGHTS needs col0, row0, cols, rows, flags and the heights");
        return;
    }
    int32_t r[4];
    uint32_t flags;
    memcpy(r, g_req_payload, 16);
    memcpy(&flags, g_req_payload + 16, 4);
    int col0 = r[0], row0 = r[1], cols = r[2], rows = r[3];
    if (cols <= 0 || rows <= 0 || (uint64_t)cols * (uint64_t)rows > MXMOD_MAX_SET_FLOATS) {
        reply_error("rectangle is empty or bigger than one write; send it in strips");
        return;
    }
    size_t count = (size_t)cols * rows;
    if (g_req_len != 20 + count * 4) {
        reply_error("SET_HEIGHTS payload size doesn't match cols * rows");
        return;
    }
    const char *why = game_write_heights(col0, row0, cols, rows, (const float *)(g_req_payload + 20),
                                         (flags & MXMOD_SET_FIRST) != 0, (flags & MXMOD_SET_FINAL) != 0);
    if (why) {
        reply_error(why);
        return;
    }
    track_info t;
    read_track(&t);
    uintptr_t b = t.terrain;
    float min_ft = 0, max_ft = 0;
    int changes = 0;
    read_f32(b + T_MIN_FT, &min_ft);
    read_f32(b + T_MAX_FT, &max_ft);
    read_i32(b + T_CHANGES, &changes);
    char *json = malloc(256);
    if (!json)
        return;
    int n = snprintf(json, 256, "{\"applied\": %s, \"readonly\": %s, \"changes\": %d, \"minFt\": %.9g, \"maxFt\": %.9g}",
                     g_readonly ? "false" : "true", g_readonly ? "true" : "false", changes, min_ft, max_ft);
    g_reply_type = MXMOD_MSG_APPLIED;
    g_reply = json;
    g_reply_len = (uint32_t)(n < 256 ? n : 255);
}

/* ── gradients ── */

/* The game's realloc (0x14000bbc0): the CRT's realloc plus the game's own allocation tracking. Gradient vectors must
   grow through it, never the mod's allocator, since the game frees and grows them itself. */
#define RVA_GAME_REALLOC (g_build->rva_game_realloc)
typedef void *(*game_realloc_fn)(void *old, size_t size);

/* Room for `need` items, grown the way the game grows a full gradient vector (0x14005d0c0): capacity doubled,
   realloc to capacity * stride. 0 if the game's allocator failed (the vector is left as it was). */
static int grow_vector(void *vector, int need)
{
    struct {
        void *data;
        int32_t capacity, count, stride;
    } *v = vector;
    if (v->capacity >= need)
        return 1;
    int capacity = v->capacity > 0 ? v->capacity : 16;
    while (capacity < need)
        capacity *= 2;
    void *grown = ((game_realloc_fn)(g_base + RVA_GAME_REALLOC))(v->data, (size_t)capacity * (size_t)v->stride);
    if (!grown)
        return 0;
    v->data = grown, v->capacity = capacity;
    return 1;
}

typedef struct {
    game_gradient *list;
    int count, selected, selected_point;
    char problem[160];
} gradient_info;

/* The gradient array, checked record by record. If anything doesn't look like the layout, nothing is trusted. */
static int read_gradients(gradient_info *gi)
{
    memset(gi, 0, sizeof *gi);
    uintptr_t arr;
    int capacity, stride;
    if (!read_ptr(g_base + RVA_GRADIENTS, &arr) || !read_i32(g_base + RVA_GRADIENT_COUNT, &gi->count) ||
        !read_i32(g_base + RVA_GRADIENT_CAP, &capacity) || !read_i32(g_base + RVA_GRADIENT_STRIDE, &stride) ||
        !read_i32(g_base + RVA_SELECTED_GRADIENT, &gi->selected) || !read_i32(g_base + RVA_SELECTED_POINT, &gi->selected_point)) {
        snprintf(gi->problem, sizeof gi->problem, "the gradient globals are not readable");
        return 0;
    }
    if (gi->count < 0 || gi->count > capacity || capacity > 65536) {
        snprintf(gi->problem, sizeof gi->problem, "gradient count %d / room %d look wrong", gi->count, capacity);
        return 0;
    }
    if (gi->count == 0)
        return 1;
    if (stride != (int)sizeof(game_gradient) || !mx_readable((void *)arr, (size_t)gi->count * sizeof(game_gradient))) {
        snprintf(gi->problem, sizeof gi->problem, "gradient records are %d bytes or unreadable, expected 64", stride);
        return 0;
    }
    gi->list = (game_gradient *)arr;
    for (int i = 0; i < gi->count; i++) {
        const game_gradient *g = &gi->list[i];
        if (g->points.stride != 8 || g->types.stride != 1 || g->points.count != g->types.count ||
            g->points.count < 0 || g->points.count > g->points.capacity || g->types.count > g->types.capacity ||
            (g->points.count && (!mx_readable(g->points.data, (size_t)g->points.count * 8) ||
                                 !mx_readable(g->types.data, (size_t)g->types.count)))) {
            snprintf(gi->problem, sizeof gi->problem, "gradient %d doesn't look like a gradient record", i);
            gi->list = NULL;
            return 0;
        }
    }
    return 1;
}

/* GRADIENTS: i32 count, selected, selected point; then per gradient f32 ax, az, bx, bz, u32 points, u32 room,
   points * (f32 s, h), points * u8 type. */
static void handle_get_gradients(void)
{
    gradient_info gi;
    if (!read_gradients(&gi)) {
        reply_error(gi.problem);
        return;
    }
    size_t size = 12;
    for (int i = 0; i < gi.count; i++)
        size += 24 + (size_t)gi.list[i].points.count * 9;
    unsigned char *out = malloc(size), *at = out;
    if (!out) {
        reply_error("out of memory");
        return;
    }
    int32_t head[3] = {gi.count, gi.selected, gi.selected_point};
    memcpy(at, head, 12);
    at += 12;
    for (int i = 0; i < gi.count; i++) {
        const game_gradient *g = &gi.list[i];
        int room = g->points.capacity < g->types.capacity ? g->points.capacity : g->types.capacity;
        float line[4] = {g->ax, g->az, g->bx, g->bz};
        uint32_t n[2] = {(uint32_t)g->points.count, (uint32_t)room};
        memcpy(at, line, 16);
        memcpy(at + 16, n, 8);
        at += 24;
        memcpy(at, g->points.data, (size_t)g->points.count * 8);
        at += (size_t)g->points.count * 8;
        memcpy(at, g->types.data, (size_t)g->types.count);
        at += g->types.count;
    }
    g_reply_type = MXMOD_MSG_GRADIENTS;
    g_reply = out;
    g_reply_len = (uint32_t)size;
}

/* ── Gradient Length: extend or stretch a shaped gradient while its B is dragged in Placement ──
   The game locks a shaped gradient's length to its last point's s, so B only swings around A (docs/game/editor-gradients-ui.md
   section 6). With Extend or Stretch on, B goes under the mouse and the profile is refitted to the new length every
   frame, from the profile as it was when the drag began (core/curved.py extended_points / stretched_points, the app's
   Extend and Stretch). Untouched gradients are left to the game, which already lets B follow and resets the profile. */
#define RVA_DRAG_STATE (g_build->rva_drag_state) /* int: 1 = A or a profile point, 2 = B */
#define MODE_PLACEMENT 9

static struct {
    int active, index, untouched;
    curve start; /* the profile when the drag began (only p and points are used) */
} g_length_drag;

int game_editor_mode(void)
{
    int mode = -1;
    read_i32(g_base + RVA_EDITOR_MODE, &mode);
    return mode;
}

void game_gradient_length_frame(double mouse_x, double mouse_z, int resize)
{
    int mode = -1, drag = 0, held = 0, selected = -1;
    read_i32(g_base + RVA_EDITOR_MODE, &mode);
    read_i32(g_base + RVA_DRAG_STATE, &drag);
    read_i32(g_base + RVA_MOUSE_DOWN, &held);
    read_i32(g_base + RVA_SELECTED_GRADIENT, &selected);
    if (resize == GRADIENT_LENGTH_LOCKED || g_readonly || mode != MODE_PLACEMENT || drag != 2 || held != 1 || selected < 0) {
        g_length_drag.active = 0;
        return;
    }
    gradient_info gi;
    if (!read_gradients(&gi) || selected >= gi.count)
        return;
    game_gradient *g = &gi.list[selected];
    if (!g_length_drag.active || g_length_drag.index != selected) {
        /* The drag began: keep the profile as it is now, to refit from each frame. */
        int n = g->points.count < CURVE_MAX_POINTS ? g->points.count : CURVE_MAX_POINTS;
        const float *pts = (const float *)g->points.data;
        const unsigned char *types = (const unsigned char *)g->types.data;
        g_length_drag.start.points = n;
        int untouched = n <= 2;
        for (int i = 0; i < n; i++) {
            profile_point p = {types[i] == 1 ? 1 : 0, pts[2 * i], pts[2 * i + 1]};
            g_length_drag.start.p[i] = p;
            if (p.h != 0)
                untouched = 0;
        }
        g_length_drag.active = 1, g_length_drag.index = selected, g_length_drag.untouched = untouched;
    }
    if (g_length_drag.untouched)
        return;
    double length = hypot(mouse_x - g->ax, mouse_z - g->az);
    if (length < 0.5)
        return;
    static curve fitted;
    fitted.points = g_length_drag.start.points;
    memcpy(fitted.p, g_length_drag.start.p, (size_t)fitted.points * sizeof fitted.p[0]);
    profile_fit(&fitted, length, resize == GRADIENT_LENGTH_STRETCH);
    if (!grow_vector(&g->points, fitted.points) || !grow_vector(&g->types, fitted.points)) {
        game_status("Gradient Length: the game couldn't make room for another profile point.");
        return;
    }
    float *dst = (float *)g->points.data;
    unsigned char *dst_types = (unsigned char *)g->types.data;
    for (int i = 0; i < fitted.points; i++) {
        dst[2 * i] = (float)fitted.p[i].s;
        dst[2 * i + 1] = (float)fitted.p[i].h;
        dst_types[i] = (unsigned char)(fitted.p[i].type == 1);
    }
    g->points.count = g->types.count = fitted.points;
    g->bx = (float)mouse_x, g->bz = (float)mouse_z;
    int picked = -1;
    if (read_i32(g_base + RVA_SELECTED_POINT, &picked) && picked >= fitted.points)
        *(int *)(g_base + RVA_SELECTED_POINT) = -1;
}

/* The selected gradient's profile, for the section library: 1 with its index, points, room and picked point. */
int game_selected_profile(int *index, profile_point *out, int max, int *n, int *room, int *picked)
{
    gradient_info gi;
    if (!read_gradients(&gi) || gi.selected < 0 || gi.selected >= gi.count)
        return 0;
    const game_gradient *g = &gi.list[gi.selected];
    const float *pts = (const float *)g->points.data;
    const unsigned char *types = (const unsigned char *)g->types.data;
    *n = g->points.count < max ? g->points.count : max;
    for (int i = 0; i < *n; i++) {
        profile_point p = {types[i] == 1, pts[2 * i], pts[2 * i + 1]};
        out[i] = p;
    }
    *index = gi.selected;
    *room = max; /* the game's vectors grow as needed (grow_vector) */
    *picked = gi.selected_point;
    return 1;
}

/* Rewrites a gradient's profile in place (its ends don't move), within the room it has. NULL on success, else why. */
const char *game_set_profile(int index, const profile_point *p, int n, int picked)
{
    gradient_info gi;
    if (!read_gradients(&gi) || index < 0 || index >= gi.count)
        return "That gradient isn't there any more.";
    game_gradient *g = &gi.list[index];
    if (n < 2)
        return "A profile needs at least two points.";
    if (!g_readonly && (!grow_vector(&g->points, n) || !grow_vector(&g->types, n)))
        return "The game couldn't make room for that many points.";
    if (g_readonly) {
        mx_log("READONLY: would set gradient %d's profile to %d points", index, n);
        return NULL;
    }
    float *dst = (float *)g->points.data;
    unsigned char *dst_types = (unsigned char *)g->types.data;
    for (int i = 0; i < n; i++) {
        dst[2 * i] = (float)p[i].s, dst[2 * i + 1] = (float)p[i].h;
        dst_types[i] = (unsigned char)(p[i].type == 1);
    }
    g->points.count = g->types.count = n;
    if (gi.selected == index)
        *(int *)(g_base + RVA_SELECTED_POINT) = picked < n ? picked : -1;
    return NULL;
}

/* ── whole gradients: copy, delete, insert ──
   New Gradient (0x14005d160) mallocs a fresh points and types vector for every gradient and copies the record onto
   array[count++], growing the array by doubling through the game's realloc. Nothing shares or reuses vectors, so
   records can slide along the array. Vectors are made and freed with the game's own malloc and free (0x14000b9c0,
   0x14000bce0), which keep its allocation tracking right. */
#define RVA_GAME_MALLOC (g_build->rva_game_malloc)
#define RVA_GAME_FREE (g_build->rva_game_free)
typedef void *(*game_malloc_fn)(size_t size);
typedef void (*game_free_fn)(void *p);

int game_gradient_count(void)
{
    gradient_info gi;
    return read_gradients(&gi) ? gi.count : -1;
}

int game_selected_gradient(void)
{
    gradient_info gi;
    return read_gradients(&gi) && gi.selected >= 0 && gi.selected < gi.count ? gi.selected : -1;
}

int game_gradient_copy(int index, gradient_copy *out)
{
    gradient_info gi;
    if (!read_gradients(&gi) || index < 0 || index >= gi.count)
        return 0;
    const game_gradient *g = &gi.list[index];
    const float *pts = (const float *)g->points.data;
    const unsigned char *types = (const unsigned char *)g->types.data;
    out->ax = g->ax, out->az = g->az, out->bx = g->bx, out->bz = g->bz;
    out->points = g->points.count < CURVE_MAX_POINTS ? g->points.count : CURVE_MAX_POINTS;
    for (int i = 0; i < out->points; i++) {
        profile_point p = {types[i] == 1, pts[2 * i], pts[2 * i + 1]};
        out->p[i] = p;
    }
    return 1;
}

/* Edits to the list are refused while a button is held (a drag is going on) or the track is locked, as for heights. */
static const char *list_editable(gradient_info *gi)
{
    track_info t;
    read_track(&t);
    if (!t.present)
        return "No track is open in the editor.";
    if (t.locked)
        return "The editor has this track locked for editing.";
    if (t.mouse_down)
        return "Let go of the mouse button first.";
    if (!read_gradients(gi))
        return "The game's gradient list couldn't be read.";
    return NULL;
}

const char *game_gradient_delete(int index)
{
    gradient_info gi;
    const char *why = list_editable(&gi);
    if (why)
        return why;
    if (index < 0 || index >= gi.count)
        return "That gradient isn't there any more.";
    if (g_readonly) {
        mx_log("READONLY: would delete gradient %d of %d", index, gi.count);
        return NULL;
    }
    game_gradient removed = gi.list[index];
    memmove(&gi.list[index], &gi.list[index + 1], (size_t)(gi.count - index - 1) * sizeof gi.list[0]);
    memset(&gi.list[gi.count - 1], 0, sizeof gi.list[0]); /* the slot past the end holds no pointers of a live one */
    *(int *)(g_base + RVA_GRADIENT_COUNT) = gi.count - 1;
    /* Something stays selected while any gradient is left: Placement's click starts from the selected gradient and
       does nothing at all when there's none (0x1400eb079 on 1684: js to the end), so -1 would stop every pick. The
       game never leaves it -1 itself. The one that took its place, or the new last one. */
    int left = gi.count - 1, selected = gi.selected;
    if (selected == index)
        selected = index < left ? index : left - 1;
    else if (selected > index)
        selected--;
    *(int *)(g_base + RVA_SELECTED_GRADIENT) = selected;
    *(int *)(g_base + RVA_SELECTED_POINT) = -1;
    game_free_fn game_free = (game_free_fn)(g_base + RVA_GAME_FREE);
    if (removed.points.data)
        game_free(removed.points.data);
    if (removed.types.data)
        game_free(removed.types.data);
    g_length_drag.active = 0;
    mx_log("deleted gradient %d (%d left)", index, gi.count - 1);
    return NULL;
}

const char *game_gradient_insert(int index, const gradient_copy *src)
{
    gradient_info gi;
    const char *why = list_editable(&gi);
    if (why)
        return why;
    if (src->points < 2)
        return "A gradient needs at least two profile points.";
    float line[4] = {src->ax, src->az, src->bx, src->bz};
    for (int i = 0; i < 4; i++)
        if (!(line[i] > -1e6f && line[i] < 1e6f))
            return "That gradient's ends aren't usable numbers.";
    for (int i = 0; i < src->points; i++)
        if (!(fabs(src->p[i].s) < 1e6 && fabs(src->p[i].h) < 1e6) || (i == 0 ? fabs(src->p[0].s) > 1e-4 : !(src->p[i].s > src->p[i - 1].s)))
            return "That gradient's profile doesn't start at 0 and go forward.";
    index = index < 0 ? 0 : index > gi.count ? gi.count : index;
    if (g_readonly) {
        mx_log("READONLY: would add a gradient of %d points at %d", src->points, index);
        return NULL;
    }
    int capacity, stride;
    uintptr_t arr;
    read_i32(g_base + RVA_GRADIENT_CAP, &capacity);
    read_i32(g_base + RVA_GRADIENT_STRIDE, &stride);
    read_ptr(g_base + RVA_GRADIENTS, &arr);
    if (stride != (int)sizeof(game_gradient))
        return "The game's gradient records aren't the size this mod knows.";
    if (gi.count >= capacity) {
        /* Full: double it, as the game does (0x14005d220). */
        int grown_cap = capacity > 0 ? capacity * 2 : 16;
        void *grown = ((game_realloc_fn)(g_base + RVA_GAME_REALLOC))((void *)arr, (size_t)grown_cap * (size_t)stride);
        if (!grown)
            return "The game couldn't make room for another gradient.";
        *(void **)(g_base + RVA_GRADIENTS) = grown;
        *(int *)(g_base + RVA_GRADIENT_CAP) = grown_cap;
        arr = (uintptr_t)grown;
    }
    int room = 16;
    while (room < src->points)
        room *= 2;
    game_malloc_fn game_malloc = (game_malloc_fn)(g_base + RVA_GAME_MALLOC);
    float *pts = game_malloc((size_t)room * 8);
    unsigned char *types = pts ? game_malloc((size_t)room) : NULL;
    if (!types) {
        if (pts)
            ((game_free_fn)(g_base + RVA_GAME_FREE))(pts);
        return "The game couldn't make room for the gradient's points.";
    }
    for (int i = 0; i < src->points; i++) {
        pts[2 * i] = (float)src->p[i].s, pts[2 * i + 1] = (float)src->p[i].h;
        types[i] = (unsigned char)(src->p[i].type == 1);
    }
    game_gradient g;
    memset(&g, 0, sizeof g);
    g.ax = src->ax, g.az = src->az, g.bx = src->bx, g.bz = src->bz;
    g.points.data = pts, g.points.capacity = room, g.points.count = src->points, g.points.stride = 8;
    g.types.data = types, g.types.capacity = room, g.types.count = src->points, g.types.stride = 1;
    game_gradient *list = (game_gradient *)arr;
    memmove(&list[index + 1], &list[index], (size_t)(gi.count - index) * sizeof list[0]);
    list[index] = g;
    *(int *)(g_base + RVA_GRADIENT_COUNT) = gi.count + 1;
    *(int *)(g_base + RVA_SELECTED_GRADIENT) = index;
    *(int *)(g_base + RVA_SELECTED_POINT) = -1;
    g_length_drag.active = 0;
    mx_log("added gradient %d (%d points, %d now)", index, src->points, gi.count + 1);
    return NULL;
}

/* SET_GRADIENT: i32 index, f32 ax, az, bx, bz, u32 n, n * (f32 s, h), n * u8 type. Rewrites one gradient in place;
   refused if the points don't fit in the room it already has (growing it needs the game's allocator, not done yet). */
static void handle_set_gradient(void)
{
    if (g_req_len < 24) {
        reply_error("SET_GRADIENT needs index, line, point count, points and types");
        return;
    }
    int32_t index;
    float line[4];
    uint32_t n;
    memcpy(&index, g_req_payload, 4);
    memcpy(line, g_req_payload + 4, 16);
    memcpy(&n, g_req_payload + 20, 4);
    if (n < 2 || n > 65536 || g_req_len != 24 + (size_t)n * 9) {
        reply_error("SET_GRADIENT payload size doesn't match its point count (at least 2)");
        return;
    }
    const float *pts = (const float *)(g_req_payload + 24);
    const unsigned char *types = g_req_payload + 24 + (size_t)n * 8;
    for (int i = 0; i < 4; i++)
        if (!(line[i] > -1e6f && line[i] < 1e6f)) {
            reply_error("the gradient's ends must be finite");
            return;
        }
    for (uint32_t i = 0; i < n; i++) {
        float s = pts[2 * i], h = pts[2 * i + 1];
        if (!(s > -1e6f && s < 1e6f && h > -1e6f && h < 1e6f) || (i == 0 ? (s < -1e-4f || s > 1e-4f) : !(s > pts[2 * i - 2]))) {
            reply_error("profile points must be finite, start at 0 and go strictly forward");
            return;
        }
    }
    track_info t;
    read_track(&t);
    if (!t.present) {
        reply_error(t.problem);
        return;
    }
    if (t.mouse_down) {
        reply_error("a mouse button is held in the editor; let go and try again");
        return;
    }
    gradient_info gi;
    if (!read_gradients(&gi)) {
        reply_error(gi.problem);
        return;
    }
    if (index < 0 || index >= gi.count) {
        reply_error("the game has no gradient with that number");
        return;
    }
    game_gradient *g = &gi.list[index];
    if (!g_readonly && (!grow_vector(&g->points, (int)n) || !grow_vector(&g->types, (int)n))) {
        reply_error("the game couldn't make room for that many points");
        return;
    }
    if (g_readonly) {
        mx_log("READONLY: would set gradient %d to %u points", index, n);
    } else {
        g->ax = line[0], g->az = line[1], g->bx = line[2], g->bz = line[3];
        float *dst = (float *)g->points.data;
        unsigned char *dst_types = (unsigned char *)g->types.data;
        for (uint32_t i = 0; i < n; i++) {
            dst[2 * i] = pts[2 * i];
            dst[2 * i + 1] = pts[2 * i + 1];
            dst_types[i] = types[i] == 1 ? 1 : 0;
        }
        g->points.count = (int32_t)n;
        g->types.count = (int32_t)n;
        /* The editor's picked point must still exist. */
        if (gi.selected == index && gi.selected_point >= (int)n)
            *(int *)(g_base + RVA_SELECTED_POINT) = -1;
        *(const char **)(g_base + RVA_STATUS_TEXT) = STATUS_GRADIENT;
        mx_log("set gradient %d: %u points", index, n);
    }
    char *json = malloc(128);
    if (!json)
        return;
    int len = snprintf(json, 128, "{\"applied\": %s, \"readonly\": %s, \"index\": %d, \"points\": %u}",
                       g_readonly ? "false" : "true", g_readonly ? "true" : "false", index, n);
    g_reply_type = MXMOD_MSG_APPLIED;
    g_reply = json;
    g_reply_len = (uint32_t)len;
}

/* SET_SCENE: JSON, what to draw over the editor (docs/ingame-tools.md section 4). Replaces the last one. */
static void handle_set_scene(void)
{
    const char *why = scene_set((const char *)g_req_payload, g_req_len);
    if (why) {
        reply_error(why);
        return;
    }
    const scene *s = scene_current();
    char *json = malloc(128);
    if (!json)
        return;
    int len = snprintf(json, 128, "{\"applied\": true, \"items\": %d, \"points\": %d, \"tool\": \"%s\"}", s->items, s->points,
                       s->tool[0] ? "on" : "");
    g_reply_type = MXMOD_MSG_APPLIED;
    g_reply = json;
    g_reply_len = (uint32_t)len;
}

/* GET_EVENTS: the game's mouse and keys while our tool is on, and the view, as JSON (empties the mod's queue). */
static void handle_get_events(void)
{
    enum { CAP = 65536 };
    char *json = malloc(CAP);
    if (!json)
        return;
    g_reply_len = (uint32_t)ingame_events_json(json, CAP);
    g_reply_type = MXMOD_MSG_EVENTS;
    g_reply = json;
}

static void service_request(void)
{
    if (InterlockedCompareExchange(&g_req_state, REQ_BUSY, REQ_PENDING) != REQ_PENDING)
        return;
    g_reply = NULL;
    g_reply_len = 0;
    switch (g_req_type) {
    case MXMOD_MSG_HELLO: handle_hello(); break;
    case MXMOD_MSG_GET_HEIGHTS: handle_get_heights(); break;
    case MXMOD_MSG_SET_HEIGHTS: handle_set_heights(); break;
    case MXMOD_MSG_GET_GRADIENTS: handle_get_gradients(); break;
    case MXMOD_MSG_SET_GRADIENT: handle_set_gradient(); break;
    case MXMOD_MSG_SET_SCENE: handle_set_scene(); break;
    case MXMOD_MSG_GET_EVENTS: handle_get_events(); break;
    default: reply_error("unknown message type"); break;
    }
    InterlockedExchange(&g_req_state, REQ_DONE);
    SetEvent(g_req_done);
}

void mod_on_frame(void)
{
    InterlockedIncrement(&g_swaps);
    InterlockedIncrement(&g_frames);
    if (!g_started) {
        g_started = 1;
        start();
    }
    if (g_enabled) {
        scene_frame();
        ingame_frame();
        service_request();
    }
}

void mod_on_clear(void)
{
    /* Fallback in case the game's GDI SwapBuffers never reaches wglSwapBuffers: drive frames from glClear. */
    LONG clears = InterlockedIncrement(&g_clears);
    if (g_swaps == 0 && clears > 240) {
        if (!g_started) {
            g_started = 1;
            start();
            mx_log("wglSwapBuffers not seen after 240 glClear calls: using glClear as the frame hook");
        }
        InterlockedIncrement(&g_frames);
        if (g_enabled) {
            scene_frame();
            ingame_frame();
            service_request();
        }
    }
}

int mod_request(uint32_t type, const void *payload, uint32_t length,
                uint32_t *out_type, void **out, uint32_t *out_length, DWORD timeout_ms)
{
    if (!g_enabled)
        return 0;
    if (InterlockedCompareExchange(&g_req_state, REQ_IDLE, REQ_IDLE) != REQ_IDLE)
        return 0;
    g_req_type = type;
    g_req_len = length;
    g_req_payload = payload; /* the caller keeps it alive until we return */
    ResetEvent(g_req_done);
    InterlockedExchange(&g_req_state, REQ_PENDING);
    if (WaitForSingleObject(g_req_done, timeout_ms) != WAIT_OBJECT_0) {
        /* Game not drawing (minimized, loading). Take the request back unless the game thread already took it. */
        if (InterlockedCompareExchange(&g_req_state, REQ_IDLE, REQ_PENDING) == REQ_PENDING)
            return 0;
        WaitForSingleObject(g_req_done, INFINITE);
    }
    *out_type = g_reply_type;
    *out = g_reply;
    *out_length = g_reply_len;
    g_reply = NULL;
    InterlockedExchange(&g_req_state, REQ_IDLE);
    return *out != NULL;
}
