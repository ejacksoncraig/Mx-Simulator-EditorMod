/* The curve tool inside the game's editor, without the app. */
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

/* profile.c: the Curve Profile graph's rules (web/gradient-overlay.js) and Set From Terrain. */
double profile_snap(double value, double granularity);
int profile_pick(const curve *c, double s, double h, double radius);  /* the point within radius of (s, h), or -1 */
int profile_insert(curve *c, double s, double h);                     /* strictly between two points: its index, or -1 */
int profile_drag(curve *c, int index, double s, double h, double granularity); /* -1: dragged past a neighbour, deleted */
/* Points first..last moved as one with `held` under the mouse at (s, h); from = the profile when the drag began. */
void profile_group_drag(const profile_point *from, int n, int first, int last, int held, double s, double h,
                        double granularity, profile_point *out);
double profile_slope_readout(const curve *c, int index);
typedef struct {
    int corners;         /* sharp edges found */
    double worst_ft;     /* the furthest the finished profile sits from the ground it read */
    double tolerance_ft; /* the tolerance used: more than asked for only if the points wouldn't fit */
} trace_result;
/* c's profile from the ground under its path, within tolerance_ft. 0 if it couldn't. out can be NULL. */
int profile_trace_ground(curve *c, const float *grid, int width, double cell, double tolerance_ft, trace_result *out);

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

/* presets.c: named settings kept across tracks, for the curve tool and for the game's brush. A field set to NAN isn't
   saved, and using the preset leaves that setting alone. */
enum { PRESET_CURVE, PRESET_BRUSH, PRESET_KINDS };
#define PRESET_MAX 32
#define PRESET_FIELDS 9
enum { PC_INNER, PC_OUTER, PC_BANK, PC_STRENGTH, PC_ADD, PC_LIMIT };                 /* a curve preset's fields */
enum { PB_OUTER, PB_INNER, PB_SHAPE, PB_ANGLE, PB_OVAL, PB_SWEEP, PB_SIDE, PB_TRI_WIDTH, PB_TRI_LEAN }; /* a brush preset's (radii in ft) */
typedef struct {
    char name[48];
    double v[PRESET_FIELDS];
} preset;
int preset_count(int kind);
const char *const *preset_names(int kind);
const preset *preset_get(int kind, int i);
const char *preset_add(int kind, const double *v); /* named from its values; NULL on success, else why */
const char *preset_rename(int kind, int i, const char *name);
const char *preset_delete(int kind, int i);

/* gradtool.c: the radial sweep, for brush presets. */
void gradtool_set_sweep(double degrees, int side);

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
enum { CURVE_MODE_OFF, CURVE_MODE_PATH, CURVE_MODE_PROFILE, CURVE_MODE_STAMP /* stamptool.c has the map */ };
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
void curvetool_mouse(const curve_view *v, int type, double px, double py, int ctrl, int alt, int shift); /* the left button */
int curvetool_dragging(void);
/* The game's Gradient Edit (our tool off): a press on its graph that selects or drags a range of points. 1 if it was ours
   (then its moves and release come through curvetool_mouse); 0 leaves the click to the game. */
int curvetool_graph_down(const curve_view *v, double px, double py, int shift);
int curvetool_range_key(int key);           /* Esc with a range selected: lets go of it. 1 if used */
int curvetool_key(int key);                 /* 1 if the tool used it */
int curvetool_save(void);                   /* writes the file if anything changed; 0 if that failed */
int curvetool_before_save(void);            /* the game is about to read the ground: Live Preview comes off. 0 if it couldn't */
int curvetool_brush_feet(double *outer, double *inner); /* the game brush's size in feet now; 0 if it can't be read */
void curvetool_scene(scene *out, const curve_view *v, double mouse_px, double mouse_py); /* the curves as draw items */
const char *curvetool_status(void);         /* for hello: how many curves, saved or not, or why none */
int curvetool_gradient_length(void);        /* GRADIENT_LENGTH_*: what dragging a game gradient's B does */
enum { BRUSH_CIRCLE, BRUSH_SQUARE, BRUSH_OVAL, BRUSH_TRIANGLE }; /* saved in presets by number: add new ones at the end */
int curvetool_brush_shape(void);            /* BRUSH_*: the shape the game's brush paints with */
double curvetool_brush_angle(void);         /* degrees */
double curvetool_brush_oval(void);          /* an oval's width as a share of its length, 0.1..1 */
double curvetool_brush_tri_width(void);     /* a triangle's base as a share of its height */
double curvetool_brush_tri_lean(void);      /* its tip along the base: 0 over the middle, -1 or 1 over an end (a right angle) */
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
/* sweep.c: how much of a radial brush a preview gives that cell, 0..1, if the wedge out to end_s is painted at full
   weight: 1 inside, fading over fade ft (the brush's falloff) past the wedge's edges and the profile's end. */
double sweep_weight(double vx, double vz, double mid, double half, double s, double end_s, double fade);

/* bermpreview.c: Berm Preview, what Gradient Radial Add would do with the picked gradient, on the ground while it's edited
   in Placement and Edit. No undo step; the ground is put back when it stops. Game thread only. */
int bermpreview_on(void);
void bermpreview_toggle(void);
void bermpreview_frame(void);
int bermpreview_lift(void);                 /* the game is about to read the ground: off for a moment. 0 if it couldn't */

/* game.c: the editor's decals (game/decals.md). Game thread only. The record is the game's, 72 bytes. Image u (left to
   right) runs along (cos rot, -sin rot) and covers size * f5 ft; image v (rows) along (sin rot, cos rot), size ft. */
typedef struct {
    uint32_t tex, tnorm, tspec;   /* texture ids: colour, _tnorm, _tspec (0 = none) */
    float x, z, rot, size, f5;
    float sin_rot, cos_rot;       /* what the bins and the drawing use: whoever changes rot writes these too */
    float uv[8];                  /* corners (0,0) (1,0) (1,1) (0,1) */
} game_decal;
typedef char game_decal_is_72_bytes[sizeof(game_decal) == 72 ? 1 : -1];
int game_decals_known(void);                          /* this build's decal calls are ported and their code checks out */
const game_decal *game_decal_list(int *count);       /* the live array (read only), NULL if unreadable */
int game_decal_selected(void);                        /* Decals mode's selection, -1 for none */
void game_decal_select(int i);
int game_decal_group_count(void);                     /* decals in the game's group (m / M), -1 if unreadable */
const char *game_decal_editable(void);                /* NULL when an edit may go ahead, else why not */
/* Each NULL on success, else why. They set the track's modified flag (as the game's own edits do). */
const char *game_decal_replace(int i, const game_decal *d);
const char *game_decal_insert(int i, const game_decal *d);     /* fixes the selection and group as the game does; selects it */
const char *game_decal_delete(int i);                          /* fixes the selection and group as the game does */
int game_decal_fix_trig(int i);                       /* rewrites sin and cos from rot if they don't match: 1 if it did */

/* decaltool.c: editing the game's decals in Decals mode (15): pick by area, scale, stretch, turn, delete, our undo.
   Its rows go in our panel in mode 15; their kinds start at DECALTOOL_ROW. Game thread only. */
#define DECALTOOL_ROW 300
int decaltool_active(void);                 /* mode 15, our tool off, this build's decal calls known */
int decaltool_rows(panel_row *rows, int max, int *kinds, int *args);
void decaltool_press(int kind, int arg);
void decaltool_frame(void);
int decaltool_down(double x, double z, double ft_per_px, int shift, int alt); /* 1 if the press is ours */
void decaltool_move(double x, double z, int shift, int alt);
void decaltool_up(void);
int decaltool_dragging(void);
int decaltool_key(int key, int ctrl);       /* 1 if it used the key */
void decaltool_scene(scene *out, double mouse_x, double mouse_z, double ft_per_px);

/* stamptool.c: rut stamps, a height image (made in Photoshop, say) placed on the ground like a layer: moved and turned
   with the mouse, previewed live, applied as one undo step. Its rows go in our panel while CURVE_MODE_STAMP is on;
   their kinds start at STAMPTOOL_ROW. Game thread only.
   Shelved: its button is hidden unless built with MXMOD_STAMP=1 (not finished yet). */
#ifndef MXMOD_STAMP
#define MXMOD_STAMP 0
#endif
#define STAMPTOOL_ROW 200
int stamptool_rows(panel_row *rows, int max, int *kinds, int *args);
void stamptool_press(int kind, int arg);
void stamptool_slide(int kind, double value);
void stamptool_frame(int active);           /* each frame; not active puts the preview's ground back */
void stamptool_mouse(const curve_view *v, int type, double x, double z, int ctrl); /* the left button, x z in ft */
int stamptool_dragging(void);
int stamptool_key(int key);                 /* 1 if it used the key */
void stamptool_off(void);                   /* puts back the ground under the preview */
int stamptool_before_save(void);            /* the game is about to save: the preview comes off first. 0 if it couldn't */
void stamptool_scene(scene *out, double mouse_x, double mouse_z);
