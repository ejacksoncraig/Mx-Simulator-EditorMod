/* opengl32.dll proxy: the game loads this from its own folder, and every GL call is forwarded to the real
   opengl32.dll. If opengl32_mxs.dll sits next to it (MXS Shaders, renamed), that is loaded instead of
   System32's, so both mods run.

   The real opengl32 is loaded at the first GL call (mx_load_real, reached through mx_lazy in stubs_masm.asm), not
   inside DllMain: loading a DLL while the loader lock is held is what Microsoft says never to do.

   Nothing here touches mx.exe's code: the game calls these exports like any GL driver (it hashes its own code). */
#include "mod.h"
#include "protocol.h"
#include "exports_gen.h"

void *real_table[MXMOD_EXPORT_COUNT]; /* used by the jump stubs in stubs_masm.asm; mx_lazy until loaded */
void mx_lazy(void);                   /* stubs_masm.asm */

typedef BOOL(WINAPI *swap_fn)(HDC);
typedef void(WINAPI *clear_fn)(unsigned int);
static swap_fn real_wglSwapBuffers;
static clear_fn real_glClear;
static HMODULE g_real;
static wchar_t g_real_path[MAX_PATH];
static HMODULE g_self;
static INIT_ONCE g_load_once = INIT_ONCE_STATIC_INIT;

static int export_index(const char *name)
{
    for (int i = 0; i < MXMOD_EXPORT_COUNT; i++)
        if (strcmp(mxmod_export_names[i], name) == 0)
            return i;
    return -1;
}

static HMODULE load_real(HMODULE self)
{
    /* opengl32_mxs.dll next to us first (chained mod), else the system one by full path. */
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) {
            slash[1] = 0;
            wcscpy_s(g_real_path, MAX_PATH, dir);
            wcscat_s(g_real_path, MAX_PATH, L"opengl32_mxs.dll");
            if (GetFileAttributesW(g_real_path) != INVALID_FILE_ATTRIBUTES) {
                HMODULE m = LoadLibraryW(g_real_path);
                if (m)
                    return m;
            }
        }
    }
    UINT len = GetSystemDirectoryW(g_real_path, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 20)
        return NULL;
    wcscat_s(g_real_path, MAX_PATH, L"\\opengl32.dll");
    return LoadLibraryW(g_real_path);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH && reserved == NULL) {
        ingame_stop();     /* unloaded while the game runs on: give it its own code back. At exit there's no need. */
        tripwire_disarm(); /* its import entries point into this DLL, which is going away */
    }
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(self);
    tripwire_arm(0); /* first, before the game can send anything: every session, editor or not */
    g_self = self;
    for (int i = 0; i < MXMOD_EXPORT_COUNT; i++)
        real_table[i] = (void *)mx_lazy; /* the first call to any export loads the real opengl32 */
    mod_on_attach();
    return TRUE;
}

static BOOL CALLBACK load_once(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once, (void)param, (void)context;
    g_real = load_real(g_self);
    if (!g_real) {
        /* No GL at all: the game can't run. Say so rather than crash on a null call. */
        MessageBoxA(NULL, "The MX Simulator Tools editor mod couldn't load the real opengl32.dll, so the game can't start.",
                    "MX Simulator Tools", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
        ExitProcess(1);
    }
    void *found[MXMOD_EXPORT_COUNT];
    for (int i = 0; i < MXMOD_EXPORT_COUNT; i++)
        found[i] = (void *)GetProcAddress(g_real, mxmod_export_names[i]);
    int s = export_index("wglSwapBuffers"), c = export_index("glClear");
    real_wglSwapBuffers = s >= 0 ? (swap_fn)found[s] : NULL;
    real_glClear = c >= 0 ? (clear_fn)found[c] : NULL;
    for (int i = 0; i < MXMOD_EXPORT_COUNT; i++)
        real_table[i] = found[i]; /* each entry goes from mx_lazy to its final value in one pointer write */
    return TRUE;
}

/* Called from mx_lazy (any thread) and the two hooks. Loads once; other threads wait until it's done. */
void mx_load_real(void) { InitOnceExecuteOnce(&g_load_once, load_once, NULL, NULL); }

const wchar_t *mx_real_gl_path(void) { return g_real_path; }

BOOL WINAPI hook_wglSwapBuffers(HDC dc)
{
    mx_load_real();
    mod_on_frame();
    return real_wglSwapBuffers ? real_wglSwapBuffers(dc) : FALSE;
}

void WINAPI hook_glClear(unsigned int mask)
{
    mx_load_real();
    mod_on_clear();
    if (real_glClear)
        real_glClear(mask);
}

const char *mxmod_version(void) { return MXMOD_VERSION; }
