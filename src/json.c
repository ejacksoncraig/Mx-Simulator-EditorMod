/* The JSON reader shared by the scene (scene.c) and the track's curve file (curvefile.c). See json.h. */
#include "json.h"

#include <stdlib.h>
#include <string.h>

static void ws(json_reader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r'))
        r->p++;
}

int json_peek(json_reader *r)
{
    ws(r);
    return r->p < r->end ? (unsigned char)*r->p : -1;
}

int json_take(json_reader *r, char c)
{
    if (json_peek(r) != c)
        return 0;
    r->p++;
    return 1;
}

int json_fail(json_reader *r, const char *why)
{
    if (!r->error)
        r->error = why;
    return 0;
}

int json_word(json_reader *r, const char *w)
{
    size_t n = strlen(w);
    ws(r);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, w, n) != 0)
        return 0;
    r->p += n;
    return 1;
}

/* Escapes other than \" \\ \/ \n \t become '?'. */
int json_string(json_reader *r, char *out, size_t cap)
{
    if (!json_take(r, '"'))
        return json_fail(r, "expected a string");
    size_t n = 0;
    while (r->p < r->end && *r->p != '"') {
        char c = *r->p++;
        if (c == '\\' && r->p < r->end) {
            char e = *r->p++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : (e == '"' || e == '\\' || e == '/') ? e : '?';
            if (e == 'u') {
                /* é and the like: the app writes every non-ASCII character this way (Python's json.dumps), and
                   the track's files are Latin-1, so a code point under 256 is that byte. Anything else stays '?'. */
                unsigned code = 0;
                int digits = 0;
                for (; digits < 4 && r->p < r->end; digits++, r->p++) {
                    char h = *r->p;
                    int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (v < 0)
                        break;
                    code = code * 16 + (unsigned)v;
                }
                if (digits == 4 && code < 256)
                    c = (char)code;
            }
        }
        if (n + 1 < cap)
            out[n++] = c;
    }
    if (cap)
        out[n] = 0;
    if (!json_take(r, '"'))
        return json_fail(r, "unterminated string");
    return 1;
}

int json_number(json_reader *r, double *out)
{
    ws(r);
    char buf[64];
    size_t n = 0;
    while (r->p < r->end && n + 1 < sizeof buf && strchr("+-0123456789.eE", *r->p))
        buf[n++] = *r->p++;
    buf[n] = 0;
    char *stop;
    *out = strtod(buf, &stop);
    if (n == 0 || *stop)
        return json_fail(r, "expected a number");
    return 1;
}

int json_bool(json_reader *r, int *out)
{
    if (json_word(r, "true"))
        return *out = 1, 1;
    if (json_word(r, "false") || json_word(r, "null"))
        return *out = 0, 1;
    return json_fail(r, "expected true or false");
}

static int skip_container(json_reader *r, char open, char close, int depth)
{
    if (depth > 32)
        return json_fail(r, "nested too deep");
    json_take(r, open);
    if (json_take(r, close))
        return 1;
    do {
        if (open == '{') {
            char key[2];
            if (!json_string(r, key, sizeof key) || !json_take(r, ':'))
                return json_fail(r, "bad object");
        }
        if (!json_skip(r, depth + 1))
            return 0;
    } while (json_take(r, ','));
    return json_take(r, close) ? 1 : json_fail(r, "missing ] or }");
}

int json_skip(json_reader *r, int depth)
{
    int c = json_peek(r);
    double d;
    char s[2];
    if (c == '{')
        return skip_container(r, '{', '}', depth);
    if (c == '[')
        return skip_container(r, '[', ']', depth);
    if (c == '"')
        return json_string(r, s, sizeof s);
    if (json_word(r, "true") || json_word(r, "false") || json_word(r, "null"))
        return 1;
    return json_number(r, &d);
}

int json_numbers(json_reader *r, float *out, int max, int *count)
{
    *count = 0;
    if (!json_take(r, '['))
        return json_fail(r, "expected an array of numbers");
    if (json_take(r, ']'))
        return 1;
    do {
        double d;
        if (!json_number(r, &d))
            return 0;
        if (*count < max)
            out[*count] = (float)d;
        (*count)++;
    } while (json_take(r, ','));
    return json_take(r, ']') ? 1 : json_fail(r, "missing ]");
}

/* *first starts at 1. Returns 1 with the key read and the ':' taken, 0 at the closing '}' (or on an error). */
int json_next_key(json_reader *r, char *key, size_t cap, int *first)
{
    if (*first) {
        *first = 0;
        if (!json_take(r, '{'))
            return json_fail(r, "expected an object");
        if (json_take(r, '}'))
            return 0;
    } else if (!json_take(r, ',')) {
        if (!json_take(r, '}'))
            json_fail(r, "missing } or ,");
        return 0;
    }
    if (!json_string(r, key, cap) || !json_take(r, ':'))
        return json_fail(r, "bad object key");
    return 1;
}

int json_next_item(json_reader *r, int *first)
{
    if (*first) {
        *first = 0;
        if (!json_take(r, '['))
            return json_fail(r, "expected an array");
        return !json_take(r, ']');
    }
    if (json_take(r, ','))
        return 1;
    if (!json_take(r, ']'))
        json_fail(r, "missing ] or ,");
    return 0;
}
