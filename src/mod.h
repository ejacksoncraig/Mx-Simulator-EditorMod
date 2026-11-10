/* Shared declarations for the MX Simulator editor mod. See DEVELOPERS.md. */
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "builds.h"

/* log.c */
void mx_log_open(void);
int mx_data_dir(wchar_t *out, size_t cap); /* %LOCALAPPDATA%\MX Simulator\mxtools, made if needed */
void mx_log(const char *fmt, ...);

/* game.c: called from proxy.c */
void mod_on_attach(void);   /* inside DllMain: no loading, no threads, no files */
void mod_on_frame(void);    /* once per frame on the game's main thread */
void mod_on_clear(void);    /* each glClear; drives mod_on_frame if wglSwapBuffers never fires */

/* game.c: called from pipe.c (pipe thread). Blocks until the game thread answered or timeout_ms passed.
   On success *out is malloc'ed (caller frees) and *out_type is the reply message type. */
int mod_request(uint32_t type, const void *payload, uint32_t length,
                uint32_t *out_type, void **out, uint32_t *out_length, DWORD timeout_ms);

/* game.c: the editor's heightmap, for the in-game tools (game thread only). */
typedef struct {
    const float *heights; /* row-major, width x width, row = z, column = x */
    int width;
    float cell_ft;
    int locked;
} game_map;
int game_heights(game_map *out); /* 0 when no track is loaded */
void game_status(const char *text); /* the editor's status line; text must outlive it (a static string) */
int game_editor_mode(void);         /* 0..15, 9 = Gradient Placement */
/* The game's own sliders, checked by their label first. Radius is in screen px: outer ft = Radius * viewFt / screenH;
   inner = Focus * outer. 0 / an error if this build's aren't known. */
enum { GAME_SLIDER_RADIUS, GAME_SLIDER_FOCUS, GAME_SLIDER_GRANULARITY, GAME_SLIDER_ZOOM };
int game_slider_get(int which, double *value, double *step, double *min, double *max);
const char *game_slider_set(int which, double value); /* clamped to the slider's range; NULL on success */
/* Radius past its slider's 1..128 px, clamped to lo..hi instead: the caller keeps it to a size in feet the game's own
   brush reaches at some zoom (the brush code reads it unclamped; only a click on the slider clamps it again). */
const char *game_slider_set_unclamped(int which, double value, double lo, double hi);
int ingame_view(double *view_ft, int *screen_h);      /* ingame.c: the map's height in ft and the window's in px */
int game_track(char *out, size_t cap); /* the open track's folder name (under %LOCALAPPDATA%\MX Simulator), 0 if none */
enum { GRADIENT_LENGTH_LOCKED, GRADIENT_LENGTH_EXTEND, GRADIENT_LENGTH_STRETCH };
void game_gradient_length_frame(double mouse_x, double mouse_z, int resize); /* game thread, before the draw */
const char *game_write_heights(int col0, int row0, int cols, int rows, const float *src, int first, int final);
int game_mouse_held(void); /* a mouse button is down in the editor, so game_write_heights would refuse */
/* The track's lowest and highest height, which a write only ever widens. Save scales terrain.png to them, so a preview
   that widened them puts them back when it comes off. */
int game_height_range(float *lo, float *hi);
void game_set_height_range(float lo, float hi);

/* ingame.c: the hooks for tools inside the editor (M5+). Start on the game thread once the mod is enabled. */
void ingame_start(uintptr_t base);
void ingame_stop(void);          /* puts the original bytes back */
const char *ingame_status(void); /* "on", "off (...)", "failed (...)" */
long ingame_presses(void);       /* clicks on the test button */
int ingame_debug_json(char *out, size_t cap); /* counters and last overlay numbers, for hello */
void ingame_frame(void);                       /* each frame on the game thread: samples the mouse for move events */
int ingame_events_json(char *out, size_t cap); /* GET_EVENTS reply; empties the queue */

/* tripwire.c: while this DLL is loaded, the game can't send or connect to anything (any session, any build). */
int tripwire_arm(int may_load); /* DllMain (may_load 0), then again at the first frame (1). 1 when armed */
void tripwire_disarm(void);     /* only when the DLL unloads while the game runs on */
int tripwire_armed(void);
void tripwire_fail_closed(void); /* not armed: says so and ends the game; never returns */
long tripwire_blocked(void);
const char *tripwire_note(void); /* for the log and hello */

/* scene.c: what the app asks the mod to draw (SET_SCENE). Game thread only, except scene_clear_later. */
#define SCENE_MAX_ITEMS 1024
#define SCENE_MAX_POINTS 32768
enum { SCENE_LINE = 1, SCENE_DOT, SCENE_CROSS, SCENE_SEGMENTS, SCENE_QUAD, SCENE_RING };
typedef struct {
    int kind;         /* line: a polyline through pts[first..first+count); segments: separate pairs; quad: 4 pts filled,
                         counter-clockwise on screen; dot, cross, ring: at x, z */
    float rgba[4];
    int closed;       /* a line back to its first point */
    float x, z, size; /* feet; size in marker units u = max(viewFt / 100, 1 ft); a ring's radius */
    float width;      /* a ring's thickness, in u */
    int first, count;
} scene_item;
typedef struct {
    char tool[32]; /* empty: no tool of ours is active (no panel, no input taken) */
    int items, points;
    scene_item item[SCENE_MAX_ITEMS];
    float pt[SCENE_MAX_POINTS][2];
} scene;
const char *scene_set(const char *json, size_t len); /* NULL on success, else why */
const scene *scene_current(void);
void scene_frame(void);       /* each frame, before drawing: applies a pending clear */
void scene_clear_later(void); /* any thread: the app went away, so draw nothing from the next frame */
void scene_drop_tool(void);   /* game thread: our tool is off (the game's took over); the drawing stays */

/* proxy.c */
const wchar_t *mx_real_gl_path(void);

/* pipe.c */
void pipe_start(void);

/* Address check helper: true if [p, p+n) is committed, readable memory. */
int mx_readable(const void *p, size_t n);
