/* What the app asks the mod to draw over the editor (SET_SCENE, docs/ingame-tools.md section 4).

   The JSON is parsed on the game thread into one of two buffers and the draw hook reads the other, so a frame never
   sees half a scene. Anything unknown is skipped, so the app can add fields before the mod knows them. */
#include "mod.h"
#include "json.h"

#include <string.h>

static scene g_scenes[2];
static int g_current; /* g_scenes[g_current] is what's drawn */
static volatile LONG g_clear_pending;

const scene *scene_current(void) { return &g_scenes[g_current]; }

void scene_clear_later(void) { InterlockedExchange(&g_clear_pending, 1); }

void scene_drop_tool(void) { g_scenes[g_current].tool[0] = 0; }

void scene_frame(void)
{
    if (InterlockedExchange(&g_clear_pending, 0))
        memset(&g_scenes[g_current], 0, sizeof g_scenes[g_current]);
}

static int item(json_reader *r, scene *s)
{
    if (s->items >= SCENE_MAX_ITEMS)
        return json_fail(r, "too many draw items");
    scene_item *it = &s->item[s->items];
    memset(it, 0, sizeof *it);
    it->rgba[0] = it->rgba[1] = it->rgba[2] = it->rgba[3] = 1;
    it->size = 1;
    it->first = s->points;
    char key[16];
    int first = 1;
    while (json_next_key(r, key, sizeof key, &first)) {
        double d;
        int n;
        if (!strcmp(key, "k")) {
            char k[16];
            if (!json_string(r, k, sizeof k))
                return 0;
            it->kind = !strcmp(k, "line")       ? SCENE_LINE
                       : !strcmp(k, "dot")      ? SCENE_DOT
                       : !strcmp(k, "cross")    ? SCENE_CROSS
                       : !strcmp(k, "segments") ? SCENE_SEGMENTS
                       : !strcmp(k, "quad")     ? SCENE_QUAD
                       : !strcmp(k, "ring")     ? SCENE_RING
                                                : 0;
        } else if (!strcmp(key, "pts")) {
            int each = 1;
            while (json_next_item(r, &each)) {
                float xz[2];
                if (!json_numbers(r, xz, 2, &n))
                    return 0;
                if (n != 2)
                    return json_fail(r, "each point is [x, z]");
                if (s->points >= SCENE_MAX_POINTS)
                    return json_fail(r, "too many points");
                s->pt[s->points][0] = xz[0], s->pt[s->points][1] = xz[1];
                s->points++;
            }
            if (r->error)
                return 0;
        } else if (!strcmp(key, "rgba")) {
            if (!json_numbers(r, it->rgba, 4, &n))
                return 0;
        } else if (!strcmp(key, "closed")) {
            if (!json_bool(r, &it->closed))
                return 0;
        } else if (!strcmp(key, "x") || !strcmp(key, "z") || !strcmp(key, "size") || !strcmp(key, "width")) {
            if (!json_number(r, &d))
                return 0;
            *(key[0] == 'x' ? &it->x : key[0] == 'z' ? &it->z : key[0] == 's' ? &it->size : &it->width) = (float)d;
        } else if (!json_skip(r, 1)) {
            return 0;
        }
    }
    if (r->error)
        return 0;
    it->count = s->points - it->first;
    if (!it->kind)
        return json_fail(r, "a draw item needs k: line, dot or cross");
    s->items++;
    return 1;
}

/* Parse a scene into the buffer not being drawn, then make it the current one. NULL on success, else why. */
const char *scene_set(const char *json, size_t len)
{
    scene *s = &g_scenes[1 - g_current];
    memset(s, 0, sizeof *s);
    json_reader r = {json, json + len, NULL};
    if (json_peek(&r) != '{')
        return "a scene must be a JSON object";
    char key[16];
    int first = 1;
    while (json_next_key(&r, key, sizeof key, &first)) {
        if (!strcmp(key, "draw")) {
            if (json_peek(&r) != '[')
                return "draw must be an array";
            int each = 1;
            while (json_next_item(&r, &each)) {
                if (json_peek(&r) != '{')
                    return "a draw item must be an object";
                if (!item(&r, s))
                    return r.error;
            }
        } else if (!strcmp(key, "tool")) {
            if (json_peek(&r) == '"') {
                if (!json_string(&r, s->tool, sizeof s->tool))
                    return r.error;
            } else if (!json_word(&r, "null")) {
                return "tool must be a string or null";
            }
        } else if (!json_skip(&r, 1)) {
            return r.error;
        }
        if (r.error)
            return r.error;
    }
    if (r.error)
        return r.error;
    if (json_peek(&r) != -1)
        return "text after the scene";
    g_current = 1 - g_current;
    return NULL;
}
