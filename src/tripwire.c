/* The online tripwire (docs/design.md rule 2): while this DLL is loaded, nothing the game sends reaches
   anything, this PC included, editor session or not. Armed from DllMain, before the game has sent anything.

   It changes no code: mx.exe's import table holds a pointer per Windows function it calls, and the entries for the
   network ones (ws2_32 sendto, WSASendTo, connect, WSAConnect; the game's own network is UDP sendto) are pointed
   here. They're found by value, not by a fixed address, so this works on any game build, also one the editor tools
   don't support. Every call is refused, this PC (127.x, ::1) included, so a server running on the same machine
   can't be reached either; a box says why and offers to close the game. */
#include "mod.h"

#include <stdio.h>
#include <string.h>

typedef int(WINAPI *sendto_fn)(UINT_PTR s, const char *buf, int len, int flags, const void *to, int tolen);
typedef int(WINAPI *wsasendto_fn)(UINT_PTR s, void *bufs, DWORD count, DWORD *sent, DWORD flags, const void *to,
                                  int tolen, void *overlapped, void *routine);
typedef int(WINAPI *connect_fn)(UINT_PTR s, const void *to, int tolen);
typedef int(WINAPI *wsaconnect_fn)(UINT_PTR s, const void *to, int tolen, void *caller, void *callee, void *sqos,
                                   void *gqos);
typedef void(WINAPI *set_error_fn)(int);

static sendto_fn g_sendto;
static wsasendto_fn g_wsasendto;
static connect_fn g_connect;
static wsaconnect_fn g_wsaconnect;
static set_error_fn g_set_wsa_error;

enum { MAX_SLOTS = 16 };
static void **g_slot[MAX_SLOTS];
static void *g_slot_real[MAX_SLOTS];
static int g_slots, g_sendto_slots;
static volatile LONG g_blocked, g_told;
static volatile ULONGLONG g_told_until; /* no new box before this tick: the game retries every 2 s */
static char g_note[160] = "not armed yet";

static BOOL CALLBACK find_game_window(HWND w, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER))
        return TRUE;
    *(HWND *)out = w;
    return FALSE;
}

/* A box over a fullscreen game window stays hidden behind it (seen 0.21.0), so the game is minimized while the
   box is up. ShowWindowAsync: the game's thread owns the window, and must never wait on us. */
static DWORD WINAPI say_online_blocked(LPVOID unused)
{
    (void)unused;
    HWND game = NULL;
    EnumWindows(find_game_window, (LPARAM)&game);
    if (game)
        ShowWindowAsync(game, SW_MINIMIZE);
    int answer = MessageBoxA(NULL,
                "Online play is blocked while the MX Simulator Tools editor mod is installed (its opengl32.dll is in the "
                "game folder). Nothing was sent to the server.\n\n"
                "To play online, close the game and run uninstall.bat from the mod's folder. Run install.bat again "
                "when you want the editor tools back.\n\n"
                "Close the game now?\n"
                "Yes: close MX Simulator (anything you haven't saved in the editor is lost).\n"
                "No: go back to the game so you can save first.",
                "MX Simulator Tools", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING | MB_SYSTEMMODAL | MB_TOPMOST |
                MB_SETFOREGROUND);
    if (answer == IDYES) {
        mx_log("online tripwire: closing the game, as asked from the box");
        if (game)
            PostMessageA(game, WM_CLOSE, 0, 0);
        Sleep(5000);
        mx_log("online tripwire: the game didn't close itself, ending it");
        ExitProcess(0);
    }
    if (game)
        ShowWindowAsync(game, SW_RESTORE);
    g_told_until = GetTickCount64() + 15000;
    InterlockedExchange(&g_told, 0);
    return 0;
}

/* Every blocked call ends here: log the first few, show the box (not while one is up), and fail like a dead network. */
static int blocked(const char *what, const void *to, int tolen)
{
    LONG n = InterlockedIncrement(&g_blocked);
    if (n <= 3) {
        const unsigned char *a = to;
        if (a && tolen >= 8 && (a[0] | a[1] << 8) == 2)
            mx_log("online tripwire: blocked %s to %u.%u.%u.%u:%u", what, a[4], a[5], a[6], a[7], a[2] << 8 | a[3]);
        else
            mx_log("online tripwire: blocked %s", what);
    }
    if (GetTickCount64() >= g_told_until && !InterlockedExchange(&g_told, 1)) {
        HANDLE t = CreateThread(NULL, 0, say_online_blocked, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            InterlockedExchange(&g_told, 0);
    }
    if (g_set_wsa_error)
        g_set_wsa_error(10051); /* WSAENETUNREACH */
    return -1;                  /* SOCKET_ERROR: the game sees the network as unreachable */
}

/* All four refuse every call. The real functions are kept only so the import table can be put back on unload. */
static int WINAPI mx_sendto(UINT_PTR s, const char *buf, int len, int flags, const void *to, int tolen)
{
    (void)s, (void)buf, (void)len, (void)flags;
    return blocked("a packet", to, tolen);
}

static int WINAPI mx_wsasendto(UINT_PTR s, void *bufs, DWORD count, DWORD *sent, DWORD flags, const void *to, int tolen,
                               void *overlapped, void *routine)
{
    (void)s, (void)bufs, (void)count, (void)flags, (void)overlapped, (void)routine;
    if (sent)
        *sent = 0;
    return blocked("a packet", to, tolen);
}

static int WINAPI mx_connect(UINT_PTR s, const void *to, int tolen)
{
    (void)s;
    return blocked("a connection", to, tolen);
}

static int WINAPI mx_wsaconnect(UINT_PTR s, const void *to, int tolen, void *caller, void *callee, void *sqos,
                                void *gqos)
{
    (void)s, (void)caller, (void)callee, (void)sqos, (void)gqos;
    return blocked("a connection", to, tolen);
}

static int write_pointer(void **slot, void *value)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old))
        return 0;
    *slot = value;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

/* Every import-table entry of mx.exe that holds `real` now holds `mine`. Forwarders (wsock32's sendto) resolve to
   ws2_32's, so comparing values catches those too. Returns how many. */
static int swap_imports(uint8_t *base, void *real, void *mine)
{
    if (!real)
        return 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        for (void **slot = (void **)(base + d->FirstThunk); *slot; slot++) {
            if (*slot != real || g_slots >= MAX_SLOTS)
                continue;
            if (write_pointer(slot, mine)) {
                g_slot[g_slots] = slot, g_slot_real[g_slots] = real, g_slots++;
                n++;
            }
        }
    }
    return n;
}

int tripwire_arm(int may_load)
{
    if (g_sendto_slots)
        return 1;
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (!ws2 && may_load)
        ws2 = LoadLibraryW(L"ws2_32.dll"); /* not inside DllMain */
    if (!ws2) {
        snprintf(g_note, sizeof g_note, "not armed: ws2_32.dll isn't loaded");
        return 0;
    }
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    g_set_wsa_error = (set_error_fn)(void *)GetProcAddress(ws2, "WSASetLastError");
    g_sendto = (sendto_fn)(void *)GetProcAddress(ws2, "sendto");
    g_wsasendto = (wsasendto_fn)(void *)GetProcAddress(ws2, "WSASendTo");
    g_connect = (connect_fn)(void *)GetProcAddress(ws2, "connect");
    g_wsaconnect = (wsaconnect_fn)(void *)GetProcAddress(ws2, "WSAConnect");
    int s = swap_imports(base, (void *)g_sendto, (void *)mx_sendto);
    int w = swap_imports(base, (void *)g_wsasendto, (void *)mx_wsasendto);
    int c = swap_imports(base, (void *)g_connect, (void *)mx_connect);
    int a = swap_imports(base, (void *)g_wsaconnect, (void *)mx_wsaconnect);
    g_sendto_slots = s + w;
    if (!g_sendto_slots) {
        snprintf(g_note, sizeof g_note, "not armed: mx.exe doesn't import sendto or WSASendTo");
        return 0;
    }
    snprintf(g_note, sizeof g_note, "armed (sendto %d, WSASendTo %d, connect %d, WSAConnect %d)", s, w, c, a);
    return 1;
}

void tripwire_disarm(void)
{
    for (int i = 0; i < g_slots; i++)
        write_pointer(g_slot[i], g_slot_real[i]);
    g_slots = g_sendto_slots = 0;
    snprintf(g_note, sizeof g_note, "disarmed (the DLL is unloading)");
}

/* Fail closed: the tripwire couldn't be armed (a game build that calls the network some other way), so the game
   doesn't get to run with this DLL loaded. On the game thread at its first frame; the game is minimized so the box
   isn't hidden behind it, and it never comes back. */
void tripwire_fail_closed(void)
{
    mx_log("online tripwire couldn't be armed (%s): closing the game", g_note);
    HWND game = NULL;
    EnumWindows(find_game_window, (LPARAM)&game);
    if (game)
        ShowWindow(game, SW_MINIMIZE);
    MessageBoxA(NULL,
                "The MX Simulator Tools editor mod couldn't make sure this game stays offline (this version of MX "
                "Simulator talks to the network in a way the mod doesn't know), so the game will close now.\n\n"
                "To play, run uninstall.bat from the mod's folder. Look for an updated version of the mod for the "
                "editor tools.",
                "MX Simulator Tools", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_TOPMOST | MB_SETFOREGROUND);
    ExitProcess(1);
}

int tripwire_armed(void) { return g_sendto_slots > 0; }
long tripwire_blocked(void) { return g_blocked; }
const char *tripwire_note(void) { return g_note; }
