/* Rut stamps: a height image (a rut drawn in Photoshop, say) put on the ground like a layer. Click the map to place it,
   drag to move it, Ctrl+drag (or [ and ]) to turn it; the ground shows it live, and Apply makes it one undo step.

   The image is added to whatever ground is under it, so the same rut sits on a flat, a berm or a jump face. Its grey
   is the height: with Flat Is = Mid Grey, mid grey changes nothing, white raises by Depth and black lowers by Depth
   (White: white is flat and black is -Depth; Black: black is flat and white is +Depth). Transparent pixels change
   nothing. A 16-bit greyscale PNG keeps a deep rut smooth; an 8-bit one has only 256 steps.

   Images come from %LOCALAPPDATA%\MX Simulator\mxtools\stamps (PNG, TIFF or BMP). One rut is built in, so the tool
   works with an empty folder. The preview is the curve tool's Live (curvetool.c): the ground under it is kept, each
   change is painted from that copy, and whatever the last preview covered is put back. Game thread only. */
#include "curvetool.h"

#define COBJMACROS
#include <initguid.h> /* the WIC pixel format GUIDs, defined here rather than taken from a library */
#include <wincodec.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979
#define STAMP_MAX 24
#define STAMP_MAX_PIXELS (4096u * 4096u)
#define LIST_SHOWN 12

/* ── the image ── */

typedef struct {
    int w, h, bits;
    uint16_t *v, *a; /* grey and alpha, 0..65535, row-major, row 0 at the top */
} stamp_img;

static stamp_img g_img;
static char g_names[STAMP_MAX][64]; /* [0] is the built-in rut */
static wchar_t g_files[STAMP_MAX][MAX_PATH];
static int g_count, g_choice, g_list_open, g_listed;
static char g_info[64];

static char g_said[200];
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_said, sizeof g_said, fmt, ap);
    va_end(ap);
    game_status(g_said);
    mx_log("stamp tool: %s", g_said);
}

static void img_free(stamp_img *im)
{
    free(im->v), free(im->a);
    memset(im, 0, sizeof *im);
}

/* The built-in rut, across its width: a trough that is -1 in the middle and comes up to 0 at 55% out, then a low berm
   (+0.35) pushed up either side, back to 0 at the edges. The same all along its length. */
static int img_builtin(stamp_img *out)
{
    enum { W = 16, H = 129 };
    out->v = malloc(W * H * sizeof *out->v), out->a = malloc(W * H * sizeof *out->a);
    if (!out->v || !out->a) {
        img_free(out);
        return 0;
    }
    out->w = W, out->h = H, out->bits = 16;
    for (int y = 0; y < H; y++) {
        double r = fabs(y / (double)(H - 1) * 2 - 1), t;
        if (r < 0.55)
            t = -(cos(PI * r / 0.55) * 0.5 + 0.5);
        else
            t = 0.35 * sin(PI * (r - 0.55) / 0.45);
        uint16_t g = (uint16_t)floor((t * 0.5 + 0.5) * 65535 + 0.5);
        for (int x = 0; x < W; x++)
            out->v[y * W + x] = g, out->a[y * W + x] = 65535;
    }
    return 1;
}

/* COM from ole32, looked up when first needed, so the DLL gains no import the game didn't already have. */
typedef HRESULT(WINAPI *co_init_fn)(LPVOID, DWORD);
typedef HRESULT(WINAPI *co_create_fn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef void(WINAPI *co_uninit_fn)(void);
static const GUID WIC_FACTORY = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
static const GUID WIC_FACTORY_IID = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};

/* An image file to grey and alpha. 16-bit sources stay 16-bit; anything else is read as 8-bit RGBA (WIC's 16-bit
   formats are linear, so turning an 8-bit image into one would bend its greys). Grey is the RGB average. */
static const char *img_load(const wchar_t *path, stamp_img *out)
{
    memset(out, 0, sizeof *out);
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    co_init_fn co_init = ole ? (co_init_fn)(void *)GetProcAddress(ole, "CoInitializeEx") : NULL;
    co_create_fn co_create = ole ? (co_create_fn)(void *)GetProcAddress(ole, "CoCreateInstance") : NULL;
    co_uninit_fn co_uninit = ole ? (co_uninit_fn)(void *)GetProcAddress(ole, "CoUninitialize") : NULL;
    if (!co_init || !co_create || !co_uninit)
        return "Windows' image reader (COM) isn't available.";
    HRESULT init = co_init(NULL, COINIT_APARTMENTTHREADED);
    const char *why = NULL;
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    uint16_t *raw16 = NULL;
    uint8_t *raw8 = NULL;
    UINT w = 0, h = 0;
    WICPixelFormatGUID src;
    if (FAILED(co_create(&WIC_FACTORY, NULL, CLSCTX_INPROC_SERVER, &WIC_FACTORY_IID, (void **)&factory))) {
        why = "Windows' image reader (WIC) isn't available.";
        goto done;
    }
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame))) {
        why = "Couldn't read that image (is it a PNG, TIFF or BMP?).";
        goto done;
    }
    if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &w, &h)) || w < 2 || h < 2 || (uint64_t)w * h > STAMP_MAX_PIXELS) {
        why = "The image must be 2x2 to 4096x4096 pixels.";
        goto done;
    }
    if (FAILED(IWICBitmapFrameDecode_GetPixelFormat(frame, &src))) {
        why = "Couldn't read the image's pixel format.";
        goto done;
    }
    int gray16 = IsEqualGUID(&src, &GUID_WICPixelFormat16bppGray);
    int deep = !gray16 && (IsEqualGUID(&src, &GUID_WICPixelFormat48bppRGB) || IsEqualGUID(&src, &GUID_WICPixelFormat48bppBGR) ||
                           IsEqualGUID(&src, &GUID_WICPixelFormat64bppRGBA) || IsEqualGUID(&src, &GUID_WICPixelFormat64bppBGRA));
    size_t n = (size_t)w * h;
    out->v = malloc(n * sizeof *out->v), out->a = malloc(n * sizeof *out->a);
    if (!out->v || !out->a) {
        why = "Not enough memory for that image.";
        goto done;
    }
    out->w = (int)w, out->h = (int)h, out->bits = gray16 || deep ? 16 : 8;
    if (gray16) {
        if (FAILED(IWICBitmapFrameDecode_CopyPixels(frame, NULL, w * 2, (UINT)(n * 2), (BYTE *)out->v))) {
            why = "Couldn't read the image's pixels.";
            goto done;
        }
        for (size_t i = 0; i < n; i++)
            out->a[i] = 65535;
        goto done;
    }
    if (FAILED(IWICImagingFactory_CreateFormatConverter(factory, &conv)) ||
        FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
                                              deep ? &GUID_WICPixelFormat64bppRGBA : &GUID_WICPixelFormat32bppRGBA,
                                              WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
        why = "Couldn't convert the image.";
        goto done;
    }
    if (deep) {
        raw16 = malloc(n * 8);
        if (!raw16 || FAILED(IWICFormatConverter_CopyPixels(conv, NULL, w * 8, (UINT)(n * 8), (BYTE *)raw16))) {
            why = "Couldn't read the image's pixels.";
            goto done;
        }
        for (size_t i = 0; i < n; i++) {
            const uint16_t *p = raw16 + i * 4;
            out->v[i] = (uint16_t)(((uint32_t)p[0] + p[1] + p[2] + 1) / 3), out->a[i] = p[3];
        }
    } else {
        raw8 = malloc(n * 4);
        if (!raw8 || FAILED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, (UINT)(n * 4), raw8))) {
            why = "Couldn't read the image's pixels.";
            goto done;
        }
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p = raw8 + i * 4;
            uint32_t g = ((uint32_t)p[0] + p[1] + p[2]) * 257;
            out->v[i] = (uint16_t)((g + 1) / 3), out->a[i] = (uint16_t)(p[3] * 257);
        }
    }
done:
    free(raw16), free(raw8);
    if (conv)
        IWICFormatConverter_Release(conv);
    if (frame)
        IWICBitmapFrameDecode_Release(frame);
    if (decoder)
        IWICBitmapDecoder_Release(decoder);
    if (factory)
        IWICImagingFactory_Release(factory);
    if (SUCCEEDED(init))
        co_uninit();
    if (why)
        img_free(out);
    return why;
}

/* ── the stamps folder ── */

static int stamps_dir(wchar_t *out, size_t cap)
{
    wchar_t data[MAX_PATH];
    if (!mx_data_dir(data, MAX_PATH) || swprintf(out, cap, L"%ls\\stamps", data) < 0)
        return 0;
    CreateDirectoryW(out, NULL);
    return 1;
}

static void list_stamps(void)
{
    char was[64];
    snprintf(was, sizeof was, "%s", g_choice > 0 && g_choice < g_count ? g_names[g_choice] : "");
    snprintf(g_names[0], sizeof g_names[0], "Built-in Rut");
    g_files[0][0] = 0;
    g_count = 1;
    g_listed = 1;
    wchar_t dir[MAX_PATH];
    if (!stamps_dir(dir, MAX_PATH))
        return;
    static const wchar_t *const PATTERNS[] = {L"*.png", L"*.tif", L"*.tiff", L"*.bmp"};
    for (int k = 0; k < 4; k++) {
        wchar_t pattern[MAX_PATH];
        swprintf(pattern, MAX_PATH, L"%ls\\%ls", dir, PATTERNS[k]);
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pattern, &fd);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        do {
            size_t len = wcslen(fd.cFileName);
            /* *.tif also finds .tiff on Windows: skip the second sighting */
            if (k == 2 || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || g_count >= STAMP_MAX || len < 4)
                continue;
            swprintf(g_files[g_count], MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, g_names[g_count], (int)sizeof g_names[g_count], NULL, NULL);
            char *dot = strrchr(g_names[g_count], '.');
            if (dot)
                *dot = 0;
            g_count++;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    g_choice = 0;
    for (int i = 1; i < g_count; i++)
        if (was[0] && !strcmp(g_names[i], was))
            g_choice = i;
}

/* ── where it sits and how it's painted ── */

enum { FLAT_MID, FLAT_WHITE, FLAT_BLACK };
typedef struct {
    double cx, cz, angle, length, width, depth, end_fade, side_fade;
    int flat, stamp, loads; /* loads counts image loads, so a reload previews again */
} placement;
static placement g_p = {0, 0, 0, 30, 4, 0.75, 3, 0, FLAT_MID, -1, 0};
static int g_placed, g_live = 1;
enum { DRAG_NONE, DRAG_MOVE, DRAG_TURN };
static int g_drag;
static double g_drag_dx, g_drag_dz, g_drag_da;

static void use_stamp(int i)
{
    if (i < 0 || i >= g_count)
        return;
    stamp_img fresh;
    if (i == 0) {
        if (!img_builtin(&fresh)) {
            say("Not enough memory for the built-in rut.");
            return;
        }
    } else {
        const char *why = img_load(g_files[i], &fresh);
        if (why) {
            say("%s: %s", g_names[i], why);
            return;
        }
        /* Keep the length, and the image's proportions across. */
        g_p.width = floor(g_p.length * fresh.h / fresh.w * 4 + 0.5) / 4;
        g_p.width = g_p.width < 0.5 ? 0.5 : g_p.width > 100 ? 100 : g_p.width;
    }
    img_free(&g_img);
    g_img = fresh;
    g_choice = i, g_p.stamp = i, g_p.loads++;
    if (i == 0)
        snprintf(g_info, sizeof g_info, "A rut with a berm each side");
    else
        snprintf(g_info, sizeof g_info, "%d x %d px, %d-bit%s", g_img.w, g_img.h, g_img.bits,
                 g_img.bits == 8 ? " (16-bit is smoother)" : "");
    say("Stamp: %s. Click the map to place it.", g_names[i]);
}

/* The stamp's own axes: u along its length (the image's x, left to right), v across (its y, top to bottom). */
static void axes(double *ux, double *uz, double *vx, double *vz)
{
    double a = g_p.angle * PI / 180;
    *ux = cos(a), *uz = sin(a), *vx = -sin(a), *vz = cos(a);
}

static void corners(double grow, double out[4][2])
{
    double ux, uz, vx, vz, hl = g_p.length / 2 + grow, hw = g_p.width / 2 + grow;
    axes(&ux, &uz, &vx, &vz);
    static const int sign[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int k = 0; k < 4; k++)
        out[k][0] = g_p.cx + sign[k][0] * hl * ux + sign[k][1] * hw * vx,
        out[k][1] = g_p.cz + sign[k][0] * hl * uz + sign[k][1] * hw * vz;
}

/* The cells the stamp can touch, clipped to the map: row0, row1, col0, col1. 0 if it's off the map. */
static int bounds(const game_map *m, int b[4])
{
    double c[4][2], x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
    corners(0, c);
    for (int k = 0; k < 4; k++)
        x0 = fmin(x0, c[k][0]), x1 = fmax(x1, c[k][0]), z0 = fmin(z0, c[k][1]), z1 = fmax(z1, c[k][1]);
    double cell = m->cell_ft;
    int c0 = (int)floor(x0 / cell), c1 = (int)ceil(x1 / cell), r0 = (int)floor(z0 / cell), r1 = (int)ceil(z1 / cell);
    c0 = c0 < 0 ? 0 : c0, r0 = r0 < 0 ? 0 : r0;
    c1 = c1 > m->width - 1 ? m->width - 1 : c1, r1 = r1 > m->width - 1 ? m->width - 1 : r1;
    if (c0 > c1 || r0 > r1)
        return 0;
    b[0] = r0, b[1] = r1, b[2] = c0, b[3] = c1;
    return 1;
}

/* Bilinear grey (as a height share, -1..1 by Flat Is) times alpha, at (s, t) in 0..1 across the image. */
static double sample(double s, double t)
{
    double fx = s * (g_img.w - 1), fy = t * (g_img.h - 1);
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    x0 = x0 < 0 ? 0 : x0 > g_img.w - 2 ? g_img.w - 2 : x0;
    y0 = y0 < 0 ? 0 : y0 > g_img.h - 2 ? g_img.h - 2 : y0;
    double ax = fx - x0, ay = fy - y0, sum = 0;
    for (int k = 0; k < 4; k++) {
        int x = x0 + (k & 1), y = y0 + (k >> 1);
        double wgt = (k & 1 ? ax : 1 - ax) * (k >> 1 ? ay : 1 - ay);
        size_t i = (size_t)y * g_img.w + x;
        double grey = g_img.v[i] / 65535.0, share = g_p.flat == FLAT_MID ? grey * 2 - 1 : g_p.flat == FLAT_WHITE ? grey - 1 : grey;
        sum += wgt * share * (g_img.a[i] / 65535.0);
    }
    return sum;
}

static double smooth(double e) { return e <= 0 ? 0 : e >= 1 ? 1 : e * e * (3 - 2 * e); }

/* grid is rows x cols from (at_row, at_col), already the ground before; the stamp's offset is added on it. */
static void paint(float *grid, int at_row, int at_col, int rows, int cols, double cell)
{
    double ux, uz, vx, vz;
    axes(&ux, &uz, &vx, &vz);
    /* Several samples a cell when the image is finer than the grid, so a big image isn't read as noise. */
    double per_cell = fmax((g_img.w - 1) / g_p.length, (g_img.h - 1) / g_p.width) * cell;
    int k = (int)ceil(per_cell);
    k = k < 1 ? 1 : k > 6 ? 6 : k;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            double sum = 0, wsum = 0;
            for (int i = 0; i < k; i++)
                for (int j = 0; j < k; j++) {
                    double x = (at_col + c + (j + 0.5) / k - 0.5) * cell - g_p.cx;
                    double z = (at_row + r + (i + 0.5) / k - 0.5) * cell - g_p.cz;
                    double lu = x * ux + z * uz + g_p.length / 2, lv = x * vx + z * vz + g_p.width / 2;
                    if (lu < 0 || lu > g_p.length || lv < 0 || lv > g_p.width) {
                        wsum += 1;
                        continue;
                    }
                    double fade = 1;
                    if (g_p.end_fade > 0)
                        fade *= smooth(fmin(lu, g_p.length - lu) / g_p.end_fade);
                    if (g_p.side_fade > 0)
                        fade *= smooth(fmin(lv, g_p.width - lv) / g_p.side_fade);
                    sum += fade * sample(lu / g_p.length, lv / g_p.width);
                    wsum += 1;
                }
            if (sum != 0)
                grid[(size_t)r * cols + c] += (float)(g_p.depth * sum / wsum);
        }
}

/* ── the live preview (curvetool.c's Live, on floats) ── */

static struct {
    float *base; /* the ground before the preview, rows x cols from (at_row, at_col) */
    int at_row, at_col, rows, cols;
    float *shown; /* what was last written, s_rows x s_cols from (s_row, s_col); NULL when nothing is */
    int s_row, s_col, s_rows, s_cols;
    int valid; /* last is what's on the ground now (or what last failed, so it isn't retried every frame) */
    placement last;
} g_pv;
static int g_frames, g_wait;

static void pv_forget(void)
{
    free(g_pv.base), free(g_pv.shown);
    g_pv.base = g_pv.shown = NULL;
    g_pv.valid = 0;
}

static int pv_was_shown(int gr, int gc)
{
    return g_pv.shown && gr >= g_pv.s_row && gr < g_pv.s_row + g_pv.s_rows && gc >= g_pv.s_col && gc < g_pv.s_col + g_pv.s_cols;
}

/* Everything kept must still be as left: the ground from before, with the last preview on it. */
static int pv_untouched(const game_map *m)
{
    if (!g_pv.base)
        return 1;
    if (g_pv.at_row + g_pv.rows > m->width || g_pv.at_col + g_pv.cols > m->width)
        return 0;
    for (int r = 0; r < g_pv.rows; r++)
        for (int c = 0; c < g_pv.cols; c++) {
            int gr = g_pv.at_row + r, gc = g_pv.at_col + c;
            float want = pv_was_shown(gr, gc) ? g_pv.shown[(size_t)(gr - g_pv.s_row) * g_pv.s_cols + (gc - g_pv.s_col)]
                                              : g_pv.base[(size_t)r * g_pv.cols + c];
            if (m->heights[(size_t)gr * m->width + gc] != want)
                return 0;
        }
    return 1;
}

/* Grow the kept ground to cover these cells. The new part comes from the game, which has no preview there. */
static int pv_cover(const game_map *m, int r0, int r1, int c0, int c1)
{
    if (g_pv.base) {
        int er = g_pv.at_row + g_pv.rows - 1, ec = g_pv.at_col + g_pv.cols - 1;
        r0 = r0 < g_pv.at_row ? r0 : g_pv.at_row, c0 = c0 < g_pv.at_col ? c0 : g_pv.at_col;
        r1 = r1 > er ? r1 : er, c1 = c1 > ec ? c1 : ec;
        if (r0 == g_pv.at_row && c0 == g_pv.at_col && r1 == er && c1 == ec)
            return 1;
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

/* Write what's changing: cells the stamp moved, plus wherever the last preview was (so it's put back there). */
static const char *pv_write(const float *painted, int final)
{
    int rows = g_pv.rows, cols = g_pv.cols, top = rows, bottom = -1, left = cols, right = -1;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            if (pv_was_shown(g_pv.at_row + r, g_pv.at_col + c) || painted[(size_t)r * cols + c] != g_pv.base[(size_t)r * cols + c]) {
                top = r < top ? r : top, bottom = r > bottom ? r : bottom;
                left = c < left ? c : left, right = c > right ? c : right;
            }
    if (bottom < 0)
        return final ? "The stamp doesn't change the ground here (is Depth 0?)." : NULL;
    int h = bottom - top + 1, w = right - left + 1;
    float *block = malloc((size_t)h * w * sizeof(float));
    if (!block)
        return "out of memory";
    for (int r = 0; r < h; r++)
        memcpy(block + (size_t)r * w, painted + (size_t)(top + r) * cols + left, (size_t)w * sizeof(float));
    const char *why = game_write_heights(g_pv.at_col + left, g_pv.at_row + top, w, h, block, g_pv.shown == NULL, final);
    if (why) {
        free(block);
        return why;
    }
    free(g_pv.shown);
    g_pv.shown = block, g_pv.s_row = g_pv.at_row + top, g_pv.s_col = g_pv.at_col + left, g_pv.s_rows = h, g_pv.s_cols = w;
    return NULL;
}

/* Paint the stamp on the ground as it was before the preview. final leaves it as one undo step. */
static const char *pv_show(int final)
{
    game_map m;
    if (!game_heights(&m))
        return "no track loaded";
    if (!g_img.v)
        return "No stamp is loaded.";
    if (!pv_untouched(&m)) {
        /* The game's undo (or anything else) changed the ground under it: start again from the ground as it is now. */
        pv_forget();
        say("The ground under the stamp changed, so its preview starts again from the ground as it is now.");
    }
    int b[4];
    if (!bounds(&m, b))
        return "The stamp is off the map.";
    if (!pv_cover(&m, b[0], b[1], b[2], b[3]))
        return "out of memory";
    size_t cells = (size_t)g_pv.rows * g_pv.cols;
    float *painted = malloc(cells * sizeof(float));
    if (!painted)
        return "out of memory";
    memcpy(painted, g_pv.base, cells * sizeof(float));
    /* Only the stamp's own cells are painted; the rest of the kept ground stays as it was before. */
    float *part = painted + (size_t)(b[0] - g_pv.at_row) * g_pv.cols + (b[2] - g_pv.at_col);
    int rows = b[1] - b[0] + 1, cols = b[3] - b[2] + 1;
    float *tmp = malloc((size_t)rows * cols * sizeof(float));
    if (!tmp) {
        free(painted);
        return "out of memory";
    }
    for (int r = 0; r < rows; r++)
        memcpy(tmp + (size_t)r * cols, part + (size_t)r * g_pv.cols, (size_t)cols * sizeof(float));
    paint(tmp, b[0], b[2], rows, cols, m.cell_ft);
    for (int r = 0; r < rows; r++)
        memcpy(part + (size_t)r * g_pv.cols, tmp + (size_t)r * cols, (size_t)cols * sizeof(float));
    free(tmp);
    const char *why = pv_write(painted, final);
    free(painted);
    if (final && !why)
        pv_forget();
    return why;
}

/* Put back the ground the preview covered. No undo step. */
static void pv_cancel(void)
{
    game_map m;
    if (g_pv.shown && game_heights(&m) && pv_untouched(&m))
        pv_write(g_pv.base, 0);
    pv_forget();
}

/* ── what the rest of the mod calls ── */

void stamptool_off(void)
{
    pv_cancel();
    g_drag = DRAG_NONE;
}

int stamptool_before_save(void)
{
    /* The game saves the ground as it is: take the preview off first, and wait a moment before showing it again. */
    if (!g_pv.shown)
        return 1;
    game_map m;
    if (game_heights(&m) && pv_untouched(&m) && pv_write(g_pv.base, 0))
        return 0; /* still on the ground, and still kept, so it can come off later */
    pv_forget();
    g_wait = 30;
    return 1;
}

static void apply(void)
{
    if (!g_placed) {
        say("Click the map to place the stamp first.");
        return;
    }
    const char *why = pv_show(1);
    if (why) {
        say("%s", why);
        return;
    }
    g_placed = 0;
    say("Stamp applied. Backspace takes it back; s saves. Click again to place another.");
}

static void lift(void)
{
    pv_cancel();
    g_placed = 0, g_drag = DRAG_NONE;
}

static char g_track[MAX_PATH];

void stamptool_frame(int active)
{
    /* Another track: what's kept belongs to the last one's ground, so it's dropped, not put back. */
    char track[MAX_PATH];
    if (!game_track(track, sizeof track))
        track[0] = 0;
    if (strcmp(track, g_track)) {
        snprintf(g_track, sizeof g_track, "%s", track);
        pv_forget();
        g_placed = 0, g_drag = DRAG_NONE;
    }
    if (!active) {
        if (g_pv.base)
            pv_cancel();
        return;
    }
    if (!g_listed) {
        list_stamps();
        use_stamp(0);
    }
    if (g_wait > 0) {
        g_wait--;
        return;
    }
    if (++g_frames % 2)
        return;
    if (!g_placed || !g_live) {
        if (g_pv.base)
            pv_cancel();
        return;
    }
    if (g_pv.valid && !memcmp(&g_pv.last, &g_p, sizeof g_p))
        return;
    g_pv.last = g_p, g_pv.valid = 1; /* also when it fails: tried again once something changes */
    const char *why = pv_show(0);
    if (why)
        say("%s", why);
}

static double norm_angle(double a)
{
    a = fmod(a, 360);
    return a < 0 ? a + 360 : a;
}

static int inside(double x, double z, double grow)
{
    double ux, uz, vx, vz, dx = x - g_p.cx, dz = z - g_p.cz;
    axes(&ux, &uz, &vx, &vz);
    return fabs(dx * ux + dz * uz) <= g_p.length / 2 + grow && fabs(dx * vx + dz * vz) <= g_p.width / 2 + grow;
}

void stamptool_mouse(const curve_view *v, int type, double x, double z, int ctrl)
{
    if (type == CURVE_DOWN) {
        double grab = 10.0 * v->view_ft / (v->h > 0 ? v->h : 1); /* 10 px of slack round the edge */
        if (g_placed && ctrl) {
            g_drag = DRAG_TURN;
            g_drag_da = g_p.angle - atan2(z - g_p.cz, x - g_p.cx) * 180 / PI;
        } else if (g_placed && inside(x, z, grab)) {
            g_drag = DRAG_MOVE;
            g_drag_dx = g_p.cx - x, g_drag_dz = g_p.cz - z;
        } else {
            g_p.cx = x, g_p.cz = z;
            g_placed = 1;
            g_drag = DRAG_MOVE, g_drag_dx = g_drag_dz = 0;
        }
    } else if (type == CURVE_MOVE) {
        if (g_drag == DRAG_MOVE)
            g_p.cx = x + g_drag_dx, g_p.cz = z + g_drag_dz;
        else if (g_drag == DRAG_TURN && hypot(x - g_p.cx, z - g_p.cz) > 1e-6)
            g_p.angle = norm_angle(floor(atan2(z - g_p.cz, x - g_p.cx) * 180 / PI + g_drag_da + 0.5));
    } else {
        g_drag = DRAG_NONE;
    }
}

int stamptool_dragging(void) { return g_drag != DRAG_NONE; }

int stamptool_key(int key)
{
    if (key == '[' || key == ']') {
        g_p.angle = norm_angle(g_p.angle + (key == '[' ? -5 : 5));
        return 1;
    }
    if (key == 13) {
        apply();
        return 1;
    }
    if (key == 27 || key == 127) {
        if (g_list_open)
            g_list_open = 0;
        else
            lift();
        return 1;
    }
    return 0;
}

/* ── the panel ── */

enum {
    ST_HEADER = STAMPTOOL_ROW, ST_CHOICE, ST_ITEM, ST_RELOAD, ST_FOLDER, ST_LENGTH, ST_WIDTH, ST_ANGLE, ST_DEPTH, ST_FLAT,
    ST_END_FADE, ST_SIDE_FADE, ST_LIVE, ST_APPLY, ST_LIFT
};
static const char *const FLAT_OPTIONS[] = {"Mid Grey", "White", "Black"};

int stamptool_rows(panel_row *rows, int max, int *kinds, int *args)
{
    int n = 0;
    panel_row *r = NULL;
#define SROW(kind_, rowk, label_) \
    (n < max ? (memset(r = &rows[n], 0, sizeof *r), kinds[n] = (kind_), args[n] = 0, r->kind = (rowk), r->label = (label_), n++, 1) \
             : (r = NULL, 0))
    SROW(ST_HEADER, ROWK_HEADER, "RUT STAMP");
    static char choice_label[96], item_label[STAMP_MAX][72];
    snprintf(choice_label, sizeof choice_label, "%s  %s", g_count ? g_names[g_choice] : "(none)", g_list_open ? "^" : "v");
    if (SROW(ST_CHOICE, ROWK_BUTTON, choice_label))
        r->on = g_list_open;
    if (g_list_open) {
        for (int i = 0; i < g_count && i < LIST_SHOWN; i++) {
            snprintf(item_label[i], sizeof item_label[i], "%s %s", i == g_choice ? ">" : "  ", g_names[i]);
            if (SROW(ST_ITEM, ROWK_BUTTON, item_label[i]))
                r->on = i == g_choice, args[n - 1] = i;
        }
        if (g_count > LIST_SHOWN && SROW(ST_HEADER, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "%d more in the folder not shown", g_count - LIST_SHOWN);
        SROW(ST_RELOAD, ROWK_BUTTON, "Reload Stamps Folder");
        SROW(ST_FOLDER, ROWK_BUTTON, "Open Stamps Folder");
        if (SROW(ST_HEADER, ROWK_HINT, ""))
            snprintf(r->text, sizeof r->text, "Put grey PNGs in the folder");
        return n;
    }
    if (g_info[0] && SROW(ST_HEADER, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "%s", g_info);
    if (SROW(ST_HEADER, ROWK_HINT, ""))
        snprintf(r->text, sizeof r->text, "%s", g_placed ? "Drag: move. Ctrl+drag or [ ]: turn" : "Click the map to place it");
    if (SROW(ST_LENGTH, ROWK_SLIDER, "Length (ft)"))
        r->value = g_p.length, r->min = 1, r->max = 200, r->step = 0.5;
    if (SROW(ST_WIDTH, ROWK_SLIDER, "Width (ft)"))
        r->value = g_p.width, r->min = 0.5, r->max = 100, r->step = 0.25;
    if (SROW(ST_ANGLE, ROWK_SLIDER, "Angle"))
        r->value = g_p.angle, r->min = 0, r->max = 359, r->step = 1;
    if (SROW(ST_DEPTH, ROWK_SLIDER, "Depth (ft)"))
        r->value = g_p.depth, r->min = -10, r->max = 10, r->step = 0.05;
    if (SROW(ST_FLAT, ROWK_CHOICE, "Flat Is"))
        r->options = FLAT_OPTIONS, r->count = 3, r->selected = g_p.flat;
    if (SROW(ST_END_FADE, ROWK_SLIDER, "End Fade (ft)"))
        r->value = g_p.end_fade, r->min = 0, r->max = 50, r->step = 0.25;
    if (SROW(ST_SIDE_FADE, ROWK_SLIDER, "Side Fade (ft)"))
        r->value = g_p.side_fade, r->min = 0, r->max = 25, r->step = 0.25;
    game_map m;
    if (game_heights(&m) && SROW(ST_HEADER, ROWK_LABEL, ""))
        snprintf(r->text, sizeof r->text, "Ground grid: %.2f ft a cell", m.cell_ft);
    if (SROW(ST_LIVE, ROWK_BUTTON, g_live ? "Live Preview: On" : "Live Preview: Off"))
        r->on = g_live;
    SROW(ST_APPLY, ROWK_BUTTON, "Apply To Ground (Enter)");
    if (g_placed)
        SROW(ST_LIFT, ROWK_BUTTON, "Lift Stamp (Esc)");
#undef SROW
    return n;
}

static void open_folder(void)
{
    wchar_t dir[MAX_PATH];
    if (!stamps_dir(dir, MAX_PATH)) {
        say("Couldn't make the stamps folder.");
        return;
    }
    typedef HINSTANCE(WINAPI * shell_fn)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    shell_fn open = shell ? (shell_fn)(void *)GetProcAddress(shell, "ShellExecuteW") : NULL;
    if (!open || (INT_PTR)open(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        char path[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, dir, -1, path, sizeof path, NULL, NULL);
        say("Couldn't open the folder: %s", path);
        return;
    }
    say("Stamps folder opened. Add images there, then Reload Stamps Folder.");
}

void stamptool_press(int kind, int arg)
{
    switch (kind) {
    case ST_CHOICE:
        g_list_open = !g_list_open;
        if (g_list_open)
            list_stamps(); /* a fresh look at the folder each time it opens */
        break;
    case ST_ITEM:
        g_list_open = 0;
        use_stamp(arg);
        break;
    case ST_RELOAD:
        list_stamps();
        use_stamp(g_choice);
        say("%d stamps in the folder.", g_count - 1);
        break;
    case ST_FOLDER: open_folder(); break;
    case ST_FLAT: g_p.flat = (g_p.flat + 1) % 3; break;
    case ST_LIVE: g_live = !g_live; break;
    case ST_APPLY: apply(); break;
    case ST_LIFT: lift(); break;
    }
}

void stamptool_slide(int kind, double value)
{
    switch (kind) {
    case ST_LENGTH: g_p.length = value; break;
    case ST_WIDTH: g_p.width = value; break;
    case ST_ANGLE: g_p.angle = value; break;
    case ST_DEPTH: g_p.depth = value; break;
    case ST_END_FADE: g_p.end_fade = value; break;
    case ST_SIDE_FADE: g_p.side_fade = value; break;
    }
}

/* ── drawing: the stamp's outline, where its fades start, its middle and which way its image's x runs ── */

static scene_item *item(scene *s, int kind, const float rgba[4], const double (*pts)[2], int n)
{
    if (s->items >= SCENE_MAX_ITEMS || s->points + n > SCENE_MAX_POINTS)
        return NULL;
    scene_item *it = &s->item[s->items++];
    memset(it, 0, sizeof *it);
    it->kind = kind, it->first = s->points, it->count = n, it->size = 1;
    memcpy(it->rgba, rgba, sizeof it->rgba);
    for (int i = 0; i < n; i++)
        s->pt[s->points + i][0] = (float)pts[i][0], s->pt[s->points + i][1] = (float)pts[i][1];
    s->points += n;
    return it;
}

void stamptool_scene(scene *s, double mouse_x, double mouse_z)
{
    static const float edge[4] = {1.0f, 0.824f, 0.290f, 1.0f}, fade[4] = {1.0f, 0.824f, 0.290f, 0.4f};
    static const float ghost[4] = {1, 1, 1, 0.5f};
    double keep_x = g_p.cx, keep_z = g_p.cz;
    if (!g_placed)
        g_p.cx = mouse_x, g_p.cz = mouse_z; /* where a click would put it */
    double c[4][2];
    corners(0, c);
    scene_item *it = item(s, SCENE_LINE, g_placed ? edge : ghost, (const double(*)[2])c, 4);
    if (it)
        it->closed = 1;
    if (g_placed) {
        double ux, uz, vx, vz;
        axes(&ux, &uz, &vx, &vz);
        double el = fmin(g_p.end_fade, g_p.length / 2), sw = fmin(g_p.side_fade, g_p.width / 2);
        double hl = g_p.length / 2 - el, hw = g_p.width / 2 - sw;
        if (el > 0 || sw > 0) {
            double in[4][2];
            static const int sign[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int k = 0; k < 4; k++)
                in[k][0] = g_p.cx + sign[k][0] * hl * ux + sign[k][1] * hw * vx,
                in[k][1] = g_p.cz + sign[k][0] * hl * uz + sign[k][1] * hw * vz;
            if ((it = item(s, SCENE_LINE, fade, (const double(*)[2])in, 4)))
                it->closed = 1;
        }
        /* An arrow from the middle towards the image's right-hand end. */
        double tip = g_p.length / 2, head = fmin(g_p.width / 2, g_p.length / 6);
        double arrow[3][2] = {{g_p.cx + (tip - head) * ux + head * vx, g_p.cz + (tip - head) * uz + head * vz},
                              {g_p.cx + tip * ux, g_p.cz + tip * uz},
                              {g_p.cx + (tip - head) * ux - head * vx, g_p.cz + (tip - head) * uz - head * vz}};
        double shaft[2][2] = {{g_p.cx, g_p.cz}, {g_p.cx + tip * ux, g_p.cz + tip * uz}};
        item(s, SCENE_LINE, edge, (const double(*)[2])arrow, 3);
        item(s, SCENE_LINE, fade, (const double(*)[2])shaft, 2);
        double mid[1][2] = {{g_p.cx, g_p.cz}};
        if ((it = item(s, SCENE_CROSS, edge, (const double(*)[2])mid, 0)))
            it->x = (float)g_p.cx, it->z = (float)g_p.cz, it->size = 1.2f;
    }
    g_p.cx = keep_x, g_p.cz = keep_z;
}
