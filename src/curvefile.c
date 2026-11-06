/* Reading the track's mx-tools-curves.json, as the app writes it (core/curved.py serialize_curves):
   [{"closed": bool, "anchors": [{"x", "z", "ix", "iz", "ox", "oz"}], "points": [{"type", "s", "h"}]}] */
#include "curvetool.h"
#include "json.h"

#include <string.h>

static int anchor(json_reader *r, pen_anchor *a)
{
    double *field[] = {&a->x, &a->z, &a->ix, &a->iz, &a->ox, &a->oz};
    static const char *const names[] = {"x", "z", "ix", "iz", "ox", "oz"};
    int seen = 0;
    char key[8];
    int first = 1;
    while (json_next_key(r, key, sizeof key, &first)) {
        int i = 0;
        while (i < 6 && strcmp(key, names[i]))
            i++;
        if (i == 6) {
            if (!json_skip(r, 1))
                return 0;
            continue;
        }
        if (!json_number(r, field[i]))
            return 0;
        seen |= 1 << i;
    }
    return r->error ? 0 : seen == 0x3f ? 1 : json_fail(r, "an anchor needs x, z, ix, iz, ox, oz");
}

static int point(json_reader *r, profile_point *p)
{
    double type = 0;
    int seen = 0;
    char key[8];
    int first = 1;
    while (json_next_key(r, key, sizeof key, &first)) {
        if (!strcmp(key, "type"))
            seen |= json_number(r, &type) ? 1 : 0;
        else if (!strcmp(key, "s"))
            seen |= json_number(r, &p->s) ? 2 : 0;
        else if (!strcmp(key, "h"))
            seen |= json_number(r, &p->h) ? 4 : 0;
        else if (!json_skip(r, 1))
            return 0;
        if (r->error)
            return 0;
    }
    p->type = type == 1 ? 1 : 0;
    return r->error ? 0 : seen == 7 ? 1 : json_fail(r, "a profile point needs type, s, h");
}

static int one_curve(json_reader *r, curve *c)
{
    memset(c, 0, sizeof *c);
    char key[16];
    int first = 1;
    while (json_next_key(r, key, sizeof key, &first)) {
        int each = 1;
        if (!strcmp(key, "closed")) {
            if (!json_bool(r, &c->closed))
                return 0;
        } else if (!strcmp(key, "anchors")) {
            while (json_next_item(r, &each)) {
                if (c->anchors >= CURVE_MAX_ANCHORS)
                    return json_fail(r, "a curve has more points than the mod holds");
                if (!anchor(r, &c->a[c->anchors++]))
                    return 0;
            }
        } else if (!strcmp(key, "points")) {
            while (json_next_item(r, &each)) {
                if (c->points >= CURVE_MAX_POINTS)
                    return json_fail(r, "a profile has more points than the mod holds");
                if (!point(r, &c->p[c->points++]))
                    return 0;
            }
        } else if (!json_skip(r, 1)) {
            return 0;
        }
        if (r->error)
            return 0;
    }
    return r->error ? 0 : 1;
}

#include <stdio.h>

/* The same JSON the app writes (closed, anchors, points), every number at full double precision. */
size_t curves_write(const curve_set *set, char *out, size_t cap)
{
    size_t n = 0;
#define PUT(...)                                                              \
    do {                                                                      \
        int wrote_ = snprintf(out + n, n < cap ? cap - n : 0, __VA_ARGS__);   \
        if (wrote_ < 0 || n + (size_t)wrote_ >= cap)                          \
            return 0;                                                         \
        n += (size_t)wrote_;                                                  \
    } while (0)
    PUT("[");
    for (int i = 0; i < set->count; i++) {
        const curve *c = &set->c[i];
        PUT("%s\n {\"closed\": %s, \"anchors\": [", i ? "," : "", c->closed ? "true" : "false");
        for (int k = 0; k < c->anchors; k++) {
            const pen_anchor *a = &c->a[k];
            PUT("%s{\"x\": %.17g, \"z\": %.17g, \"ix\": %.17g, \"iz\": %.17g, \"ox\": %.17g, \"oz\": %.17g}", k ? ", " : "",
                a->x, a->z, a->ix, a->iz, a->ox, a->oz);
        }
        PUT("], \"points\": [");
        for (int k = 0; k < c->points; k++)
            PUT("%s{\"type\": %d, \"s\": %.17g, \"h\": %.17g}", k ? ", " : "", c->p[k].type, c->p[k].s, c->p[k].h);
        PUT("]}");
    }
    PUT("\n]\n");
#undef PUT
    return n;
}

const char *curves_parse(const char *json, size_t len, curve_set *out)
{
    memset(out, 0, sizeof *out);
    json_reader r = {json, json + len, NULL};
    int each = 1;
    while (json_next_item(&r, &each)) {
        if (out->count >= CURVE_MAX)
            return "more curves than the mod holds";
        if (!one_curve(&r, &out->c[out->count++]))
            return r.error;
    }
    if (r.error)
        return r.error;
    return json_peek(&r) == -1 ? NULL : "text after the curves";
}
