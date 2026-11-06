/* App <-> mod protocol. Mirrored by mxtools/core/game_link.py; tests/test_game_link.py checks they match.

   Frame: u32 payload_length, u32 type, then payload (little-endian).
   HELLO        app -> mod: empty.             mod -> app: JSON (see game.c, mod_hello_json).
   GET_HEIGHTS  app -> mod: i32 col0, row0, cols, rows.
   HEIGHTS      mod -> app: u32 cols, rows, then cols*rows float32 heights in feet, row-major (row = z, col = x).
   ERROR        mod -> app: JSON {"error": "..."}.
   SET_HEIGHTS  app -> mod: i32 col0, row0, cols, rows, u32 flags, then cols*rows float32 heights in feet.
                Applied on the game thread at the next frame. A big edit is sent as strips of at most
                MXMOD_MAX_SET_FLOATS: the first strip has MXMOD_SET_FIRST, the last MXMOD_SET_FINAL (one strip: both).
                FIRST flushes the user's pending edit into the undo ring; FINAL takes one undo snapshot for the whole
                edit and shows a status line, so one Backspace undoes it.
   APPLIED      mod -> app: JSON {"applied": true, "changes": n, "minFt": f, "maxFt": f}.
   GET_GRADIENTS app -> mod: empty.
   GRADIENTS    mod -> app: i32 count, selected, selected point (-1 = none); then per gradient f32 ax, az, bx, bz,
                u32 points, u32 room, points * (f32 s, h), points * u8 type (1 = Curve to the next point).
   SET_GRADIENT app -> mod: i32 index, f32 ax, az, bx, bz, u32 n, n * (f32 s, h), n * u8 type. Rewrites one
                gradient in place, within the room it has. Reply APPLIED {"applied", "index", "points"}.
   SET_SCENE    app -> mod: JSON {"tool": "name"|null, "draw": [{"k": "line", "pts": [[x, z], ...], "rgba": [r, g, b, a],
                "closed": bool} | {"k": "dot"|"cross", "x", "z", "size" (marker units), "rgba"}]}, in world feet. Drawn
                over the editor from the next frame until replaced; cleared when the app disconnects. A tool shows our
                panel and lets it take clicks. Reply APPLIED {"applied", "items", "points"}.
   GET_EVENTS   app -> mod: empty.
   EVENTS       mod -> app: JSON {"view": {"cx", "cz", "viewFt", "w", "h", "mode", "u", "tool"}, "dropped": n,
                "events": [{"t": "down"|"up"|"move"|"key"|"toolOff", "x", "z" (world ft), "px", "py" (window px, y down),
                "key", "down", "ctrl", "shift", "alt"}]}. Only while our tool is on: map clicks (swallowed), keys other
                than pan, = -, arrows, s, Backspace and Tab (swallowed), moves sampled once a frame. A game mode button
                or number key turns the tool off (toolOff) and goes to the game. Up to 256 queued; oldest moves go first.
*/
#pragma once

#define MXMOD_PROTOCOL_VERSION 1
#define MXMOD_VERSION "0.25.3"
#define MXMOD_PIPE_NAME L"\\\\.\\pipe\\mxmod"

#define MXMOD_MSG_HELLO 1
#define MXMOD_MSG_GET_HEIGHTS 2
#define MXMOD_MSG_HEIGHTS 3
#define MXMOD_MSG_ERROR 4
#define MXMOD_MSG_SET_HEIGHTS 5
#define MXMOD_MSG_APPLIED 6
#define MXMOD_MSG_GET_GRADIENTS 7
#define MXMOD_MSG_GRADIENTS 8
#define MXMOD_MSG_SET_GRADIENT 9
#define MXMOD_MSG_GET_EVENTS 10
#define MXMOD_MSG_EVENTS 11
#define MXMOD_MSG_SET_SCENE 12
#define MXMOD_MAX_SCENE 262144u /* bytes of scene JSON */

#define MXMOD_SET_FIRST 1u
#define MXMOD_SET_FINAL 2u

#define MXMOD_MAX_PAYLOAD 67108864u          /* 64 MiB */
#define MXMOD_MAX_FLOATS_PER_REQUEST 4194304u /* 16 MiB of heights; a 2049 x 2049 map is two requests */
#define MXMOD_MAX_SET_FLOATS 262144u          /* 1 MiB of heights applied per frame, so the editor stays smooth */
