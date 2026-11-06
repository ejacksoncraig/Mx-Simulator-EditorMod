/* Named pipe server \\.\pipe\mxmod: one local client (the app) at a time. Runs on its own thread and never
   touches game memory: every request goes to the game thread through mod_request(). */
#include "mod.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>

#define REQUEST_TIMEOUT_MS 5000

static int read_exact(HANDLE pipe, void *buf, DWORD n)
{
    unsigned char *at = buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(pipe, at, n, &got, NULL) || got == 0)
            return 0;
        at += got;
        n -= got;
    }
    return 1;
}

static int write_exact(HANDLE pipe, const void *buf, DWORD n)
{
    const unsigned char *at = buf;
    while (n) {
        DWORD put = 0;
        if (!WriteFile(pipe, at, n, &put, NULL) || put == 0)
            return 0;
        at += put;
        n -= put;
    }
    return 1;
}

static int send_frame(HANDLE pipe, uint32_t type, const void *payload, uint32_t length)
{
    uint32_t head[2] = {length, type};
    return write_exact(pipe, head, sizeof head) && (length == 0 || write_exact(pipe, payload, length));
}

static int send_error(HANDLE pipe, const char *msg)
{
    char json[256];
    int n = snprintf(json, sizeof json, "{\"error\": \"%s\"}", msg);
    return send_frame(pipe, MXMOD_MSG_ERROR, json, (uint32_t)n);
}

/* Largest request this mod takes: SET_HEIGHTS with a full strip. */
#define MAX_REQUEST (20u + MXMOD_MAX_SET_FLOATS * 4u)

static void serve(HANDLE pipe)
{
    unsigned char *buf = malloc(MAX_REQUEST);
    if (!buf)
        return;
    for (;;) {
        uint32_t head[2];
        if (!read_exact(pipe, head, sizeof head))
            break;
        uint32_t length = head[0], type = head[1];
        uint32_t limit = type == MXMOD_MSG_SET_HEIGHTS || type == MXMOD_MSG_SET_GRADIENT ? MAX_REQUEST
                         : type == MXMOD_MSG_SET_SCENE                                  ? MXMOD_MAX_SCENE
                                                                                         : 64;
        if (length > limit) {
            /* Drain and refuse. */
            if (length > MXMOD_MAX_PAYLOAD)
                break;
            unsigned char sink[4096];
            int ok = 1;
            while (length && ok) {
                DWORD chunk = length > sizeof sink ? sizeof sink : length;
                ok = read_exact(pipe, sink, chunk);
                length -= chunk;
            }
            if (!ok || !send_error(pipe, "request too large; send heights in strips of at most 262144 floats"))
                break;
            continue;
        }
        if (length && !read_exact(pipe, buf, length))
            break;
        uint32_t reply_type = 0, reply_len = 0;
        void *reply = NULL;
        if (!mod_request(type, buf, length, &reply_type, &reply, &reply_len, REQUEST_TIMEOUT_MS)) {
            if (!send_error(pipe, "the game didn't answer (is the editor window drawing?)"))
                break;
            continue;
        }
        int ok = send_frame(pipe, reply_type, reply, reply_len);
        free(reply);
        if (!ok)
            break;
    }
    free(buf);
}

static DWORD WINAPI pipe_thread(LPVOID unused)
{
    (void)unused;
    mx_log("pipe listening on \\\\.\\pipe\\mxmod");
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(MXMOD_PIPE_NAME, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       1, 1 << 20, 1 << 20, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            mx_log("CreateNamedPipe failed (%lu); pipe stopped", GetLastError());
            return 0;
        }
        if (ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            mx_log("app connected");
            serve(pipe);
            scene_clear_later(); /* nothing of the app's stays on screen once it's gone */
            mx_log("app disconnected");
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void pipe_start(void)
{
    HANDLE t = CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        mx_log("could not start the pipe thread (%lu)", GetLastError());
}
