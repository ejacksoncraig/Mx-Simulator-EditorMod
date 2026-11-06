/* The mx.exe builds the editor tools know (docs/builds.md). Every address is an RVA (an offset from the
   exe's load address). game.c picks the build by the running exe's SHA-256; with none, the tools stay off. */
#pragma once
#include <stdint.h>

typedef struct {
    const char *name, *sha256;
    uint32_t rva_editor_terrain;
    uint32_t rva_editor_mode;
    uint32_t rva_mouse_down;
    uint32_t rva_gradient_count;
    uint32_t rva_gradients;
    uint32_t rva_gradient_cap;
    uint32_t rva_gradient_stride;
    uint32_t rva_selected_point;
    uint32_t rva_selected_gradient;
    uint32_t rva_undo_pending;
    uint32_t rva_status_text;
    uint32_t rva_fn_mark_dirty;
    uint32_t rva_fn_undo_snapshot;
    uint32_t rva_ptr_window_struct;
    uint32_t rva_game_realloc;
    uint32_t rva_drag_state;
    uint32_t rva_editor_draw;
    uint32_t rva_key_handler;
    uint32_t frame_editor_draw;
    uint32_t frame_key_handler;
    uint32_t rva_ptr_window;
    uint32_t rva_ptr_keys;
    uint32_t rva_ptr_colour;
    uint32_t rva_ptr_batch_flag;
    uint32_t rva_ptr_white_tex;
    uint32_t rva_view_x;
    uint32_t rva_view_z;
    uint32_t rva_zoom;
    uint32_t rva_matrix_mode;
    uint32_t rva_identity;
    uint32_t rva_translate;
    uint32_t rva_scale;
    uint32_t rva_push;
    uint32_t rva_pop;
    uint32_t rva_enable;
    uint32_t rva_disable;
    uint32_t rva_bind_texture;
    uint32_t rva_blend_func;
    uint32_t rva_begin;
    uint32_t rva_vertex;
    uint32_t rva_end;
    uint32_t rva_next_row;
    uint32_t rva_button;
    uint32_t rva_choice;
    uint32_t rva_slider;
    uint32_t rva_readout;
    uint32_t rva_ptr_readout_a;
    uint32_t rva_ptr_readout_b;
    uint32_t rva_hovered_widget;
    uint32_t rva_brush_distance;
    uint32_t rva_brush_return;
    uint32_t rva_game_malloc;    /* void *(size_t): the game's malloc, with its allocation tracking */
    uint32_t rva_game_free;      /* void (void *): its free */
    uint32_t rva_radial_height;  /* float (float x, float z): Gradient Radial's profile height at a cell */
    uint32_t rva_profile_height; /* float (float s): the selected gradient's profile height at s */
} mx_build;

extern const mx_build *g_build; /* the running one; NULL until game.c finds it */
const mx_build *mx_build_find(const char *sha256); /* NULL for a build not listed */
