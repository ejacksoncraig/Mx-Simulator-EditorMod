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
    int smooth_shaped;           /* 1: Smooth (mode 2) uses the patched brush distance too (1684: its kernel setup at 0xe6784
                                    rejoins the main loop, 0xe5ff8 -> 0xe5b9e); 0: it has its own round loop (1.0, 1681) */
    uint32_t rva_game_malloc;    /* void *(size_t): the game's malloc, with its allocation tracking */
    uint32_t rva_game_free;      /* void (void *): its free */
    uint32_t rva_radial_height;  /* float (float x, float z): Gradient Radial's profile height at a cell */
    uint32_t rva_profile_height; /* float (float s): the selected gradient's profile height at s */
    /* double: the game's Radius, Focus and Granularity sliders' values (each at +8 in its 64-byte slider struct, whose
       label pointer at +0 the mod checks before using it). Found by the slider structs' labels in .data. */
    uint32_t rva_radius, rva_focus, rva_granularity;
    /* Decals (decal tools, Decals mode 15; .claude/skills/mx-editor-mod/game/decals.md). 0 = not ported: the decal tools
       stay off on that build. Each function's first 16 bytes are in decal_sig, checked before the first call. */
    uint32_t rva_decal_replace; /* void (terrain *, const rec *, int i): replace decal i and re-bake its old and new tiles */
    uint32_t rva_decal_shift;   /* void (terrain *, int i, int delta): add delta to every bin entry >= i */
    uint32_t rva_decal_insert;  /* void (terrain *, const rec *, int i): insert at i (grows the array) and bin it */
    uint32_t rva_decal_remove;  /* void (terrain *, int i): un-bin and remove decal i */
    uint32_t rva_selected_decal; /* int: Decals mode's selected decal, -1 none */
    uint32_t rva_decal_group;   /* {int *ptr; int cap, count, stride}: Decals mode's group of selected indices */
    unsigned char decal_sig[4][16]; /* replace, shift, insert, remove */
    /* Statues (statue group, Statues mode 13; .claude/skills/mx-editor-mod/game/statues.md). 0 = not ported. The list is a
       {ptr, cap, count, stride} vector at statue_list inside the window struct (*rva_ptr_window). */
    uint32_t statue_list;          /* offset of the statue vector in the window struct */
    uint32_t rva_selected_statue;  /* int: Statues mode's selected statue, -1 none */
    uint32_t rva_statue_update;    /* void (statue *): y from the ground + offset, and the turn matrix, from x, z, rot */
    unsigned char statue_sig[16];  /* rva_statue_update's first 16 bytes */
    /* Copying and removing statues (the game's `d` and Del in Statues mode). 0 = not ported. */
    uint32_t rva_statue_add;      /* int (float x, float y_offset, float z, float rot, mesh, texture, shape names): appends
                                     a statue the way the track loader does, returns its index */
    uint32_t rva_statue_free;     /* void (statue *): frees what a record owns, before the list drops it */
    uint32_t rva_statues_changed; /* void (void): the game calls it after every add or delete */
    uint32_t rva_texture_name;    /* const char *(int id): a texture's name, NULL if not found */
    unsigned char statue_add_sig[4][16]; /* add, free, changed, texture name */
} mx_build;

extern const mx_build *g_build; /* the running one; NULL until game.c finds it */
const mx_build *mx_build_find(const char *sha256); /* NULL for a build not listed */
