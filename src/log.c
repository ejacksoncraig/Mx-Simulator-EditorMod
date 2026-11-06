/* The mod's own files (the log, the section library) live in the game's data folder, beside the tracks:
   %LOCALAPPDATA%\MX Simulator\mxtools. Always writable, unlike the game's install folder. The log is replaced each time
   the game starts. */
#include "mod.h"

#include <stdarg.h>
#include <stdio.h>

static CRITICAL_SECTION g_lock;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static LONG g_init = 0;

/* The folder, made if it isn't there. Files from before it moved (%LOCALAPPDATA%\mxtools-mod) are copied over once. */
int mx_data_dir(wchar_t *out, size_t cap)
{
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 60)
        return 0;
    swprintf(out, cap, L"%ls\\MX Simulator", local);
    CreateDirectoryW(out, NULL);
    swprintf(out, cap, L"%ls\\MX Simulator\\mxtools", local);
    if (CreateDirectoryW(out, NULL)) {
        wchar_t from[MAX_PATH], to[MAX_PATH];
        swprintf(from, MAX_PATH, L"%ls\\mxtools-mod\\profile-sections.json", local);
        swprintf(to, MAX_PATH, L"%ls\\profile-sections.json", out);
        CopyFileW(from, to, TRUE);
    }
    return 1;
}

void mx_log_open(void)
{
    if (InterlockedExchange(&g_init, 1))
        return;
    InitializeCriticalSection(&g_lock);
    wchar_t path[MAX_PATH];
    if (!mx_data_dir(path, MAX_PATH))
        return;
    wcscat_s(path, MAX_PATH, L"\\log.txt");
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void mx_log(const char *fmt, ...)
{
    if (!g_init || g_file == INVALID_HANDLE_VALUE)
        return;
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int len = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    int more = vsnprintf(line + len, sizeof line - len - 2, fmt, args);
    va_end(args);
    if (more < 0)
        more = 0;
    len += more;
    if (len > (int)sizeof line - 2)
        len = (int)sizeof line - 2;
    line[len++] = '\r';
    line[len++] = '\n';
    EnterCriticalSection(&g_lock);
    DWORD written;
    WriteFile(g_file, line, (DWORD)len, &written, NULL);
    FlushFileBuffers(g_file);
    LeaveCriticalSection(&g_lock);
}
