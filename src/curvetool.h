/* The curve tool inside the game's editor, without the app (docs/ingame-tools.md section 8). */
#pragma once

#include "mod.h"

#define CURVE_MAX_ANCHORS 1024
#define CURVE_MAX_POINTS 2048
#define CURVE_MAX 64

typedef struct {
    double x, z, ix, iz, ox, oz; /* a point on the path, and its handles arriving (i) and leaving (o) */
} pen_anchor;

typedef struct {
    int type; /* 1: the segment from this point to the next is a curve */
    double s, h;
} profile_point;

typedef struct {
    int closed, anchors, points;
    pen_anchor a[CURVE_MAX_ANCHORS];
    profile_point p[CURVE_MAX_POINTS];
} curve;

typedef struct {
    int count;
    curve c[CURVE_MAX];
} curve_set;

/* curvefile.c: the track's mx-tools-curves.json, the app's format (core/curved.py serialize_curves). */
const char *curves_parse(const char *json, size_t len, curve_set *out); /* NULL on success, else why */
size_t curves_write(const curve_set *set, char *out, size_t cap);      /* the same format; the length, 0 if too big */

/* pen.c: web/pen-path.js and the profile rules from core/curved.py, line for line. */
typedef struct {
    int segment;
    double t, distance;
} pen_hit;
pen_anchor pen_new_anchor(double x, double z);
int pen_is_smooth(const pen_anchor *a);
void pen_bezier(const pen_anchor *a, const pen_anchor *b, double t, double *x, double *z);
int pen_segments(const curve *c);
void pen_segment(const curve *c, int k, int *a, int *b);
pen_anchor pen_split(pen_anchor *a, pen_anchor *b, double t);
void pen_pull(pen_anchor *a, double x, double z, int split);
void pen_move_handle(pen_anchor *a, int out_side, double x, double z, int split);
void pen_move_anchor(pen_anchor *a, double x, double z);
void pen_toggle_corner(curve *c, int i);
double pen_sample(const curve *c, double step, void (*out)(void *, double, double, double), void *ctx);
double pen_length(const curve *c);
double profile_height(const profile_point *p, int n, double s);
void profile_fit(curve *c, double length, int stretch);
int pen_nearest(const curve *c, void (*to_px)(void *, double, double, double *, double *), void *ctx, double px, double py,
                double radius, pen_hit *hit);
int pen_points(const curve *c, int per_segment, double (*out)[2], int max);
void pen_offset(const double (*path)[2], int n, double d, double (*out)[2]);
int pen_spots(const curve *c, const double *distances, int count, double total, double (*out)[2]);

/* profile.c: the Curve Profile graph's rules (web/gradient-overlay.js) and Set From Terrain (curved.py). */
double profile_snap(double value, double granularity);
int profile_pick(const curve *c, double s, double h, double radius);  /* the point within radius of (s, h), or -1 */
int profile_insert(curve *c, double s, double h);                     /* strictly between two points: its index, or -1 */
int profile_drag(curve *c, int index, double s, double h, double granularity); /* -1: dragged past a neighbour, deleted */
double profile_slope_readout(const curve *c, int index);
int profile_from_ground(curve *c, const float *grid, int width, double cell); /* 0 if it couldn't */

/* library.c: profile sections kept across tracks, to paste onto any gradient's or curve's profile. */
#define SECTION_MAX 64
#define SECTION_MAX_POINTS 64
typedef struct {
    char name[48];
    int points;
    profile_point p[SECTION_MAX_POINTS]; /* the first at s = 0, h = 0 */
} section;
int library_count(void);
const char *const *library_names(void);
const section *library_get(int i);
const char *library_add(const profile_point *p, int first, int last); /* NULL on success, else why */
const char *library_delete(int i);
const char *library_rename(int i, const char *name);
int section_paste(const profile_point *p, int n, int at, const section *s, int room, profile_point *out, const char **why);

/* game.c: the game's selected gradient's profile, for the section library (game thread). */
int game_selected_profile(int *index, profile_point *out, int max, int *n, int *room, int *picked);
const char *game_set_profile(int index, const profile_point *p, int n, int picked);

/* game.c: whole gradients, for Delete, Undo Delete, Export and Place (game thread). */
typedef struct {
    float ax, az, bx, bz;
    int points;
    profile_point p[CURVE_MAX_POINTS];
} gradient_copy;
int game_gradient_count(void);                              /* -1 when the list can't be read */
int game_selected_gradient(void);                           /* -1 for none */
int game_gradient_copy(int index, gradient_copy *out);      /* 0 if there's no such gradient */
const char *game_gradient_delete(int index);                /* NULL on success, else why */
const char *game_gradient_insert(int index, const gradient_copy *g); /* at index (clamped), then selected */

/* curvemath.c: core/curved.py paint_curve. grid is rows x cols doubles, its first cell at (at_row, at_col) of a
   width x width map; it has to cover curve_bounds. b = row0, row1, col0, col1. 0 when off the map or not covered. */
enum { CURVE_LIMIT_NONE, CURVE_LIMIT_RAISE, CURVE_LIMIT_LOWER };
int curve_bounds(const curve *c, double cell, double outer, int width, int b[4]);
int curve_paint(double *grid, int at_row, int at_col, int rows, int cols, int width, double cell, const curve *c, double inner,
                double outer, double strength, int add, int limit, double bank, int out_bounds[4]);

/* curvetool.c */
typedef struct {
    double cx, cz, view_ft; /* the view centre and the window's height in feet */
    int w, h;               /* window px */
} curve_view;
enum { CURVE_MODE_OFF, CURVE_MODE_PATH, CURVE_MODE_PROFILE };
enum { CURVE_DOWN, CURVE_MOVE, CURVE_UP };
void curvetool_start(void);                 /* game thread, once the hooks are in: finds the track, loads its curves */
void curvetool_frame(void);                 /* each frame: reloads the file when it changed on disk */
int curvetool_mode(void);
void curvetool_set_mode(int mode);          /* CURVE_MODE_OFF when the game's own tool takes over */
/* The panel's rows, drawn with the game's own widgets. A header names the group of rows under it; a hint is a line of
   help in plain words. Both are text only (nothing to click). */
enum { ROWK_BUTTON, ROWK_CHOICE, ROWK_SLIDER, ROWK_LABEL, ROWK_HEADER, ROWK_HINT, ROWK_FAINT /* small print, right */ };
typedef struct {
    int kind;
    const char *label;
    int on;                     /* a button lit */
    const char *const *options; /* a choice's */
    int count, selected;
    double value, min, max, step; /* a slider's */
    char text[64];              /* a label's */
} panel_row;
int curvetool_panel(panel_row *rows, int max); /* top to bottom */
void curvetool_press(int row);                 /* a click on a button or choice */
void curvetool_slide(int row, double value);   /* a slider moved (already stepped and clamped) */
void curvetool_mouse(const curve_view *v, int type, double px, double py, int ctrl, int alt); /* the left button */
int curvetool_dragging(void);
int curvetool_key(int key);                 /* 1 if the tool used it */
int curvetool_save(void);                   /* writes the file if anything changed; 0 if that failed */
void curvetool_scene(scene *out, const curve_view *v, double mouse_px, double mouse_py); /* the curves as draw items */
const char *curvetool_status(void);         /* for hello: how many curves, saved or not, or why none */
int curvetool_gradient_length(void);        /* GRADIENT_LENGTH_*: what dragging a game gradient's B does */
enum { BRUSH_CIRCLE, BRUSH_SQUARE, BRUSH_OVAL };
int curvetool_brush_shape(void);            /* BRUSH_*: the shape the game's brush paints with */
double curvetool_brush_angle(void);         /* degrees */
double curvetool_brush_oval(void);          /* an oval's width as a share of its length, 0.1..1 */
int curvetool_typing(void);                 /* a name is being typed: every key is ours */
void curvetool_type(int key);               /* a key's character while typing (Enter keeps, Esc cancels, Backspace) */
void curvetool_gradients_changed(void);     /* the game's gradient list was reordered: forget marks on its profiles */

/* gradtool.c: tools for the game's own gradients (Delete, Undo Delete, Export, Saved Gradients, Place) and the radial
   brushes' sweep. Its rows go in our panel while the game's tools are in use; their kinds start at GRADTOOL_ROW. */
#define GRADTOOL_ROW 100
int gradtool_rows(panel_row *rows, int max, int *kinds, int *args); /* how many rows it added */
void gradtool_press(int kind, int arg);
void gradtool_slide(int kind, double value);
void gradtool_frame(void);
int gradtool_placing(void);                 /* the next click on the map places a saved gradient */
void gradtool_place(double x, double z);
int gradtool_key(int key);                  /* 1 if it used the key (Del, Esc) */
void gradtool_scene(scene *out, double mouse_x, double mouse_z);
enum { SWEEP_BOTH, SWEEP_CLOCKWISE, SWEEP_COUNTER };
int gradtool_sweep(double *degrees, int *side); /* 1 when the radial brushes shouldn't sweep the game's own way */
/* The sweep's middle direction and half width in radians for a gradient from A to B (x right, z down the screen). */
void gradtool_wedge(double ax, double az, double bx, double bz, double *mid, double *half);
/* sweep.c: s for the cell at v = cell - A, with that wedge (half >= pi: a full circle). */
double sweep_s(double vx, double vz, double mid, double half);
