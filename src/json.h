/* A small JSON reader for what the app and the track's files hold: objects, arrays, numbers, strings, true, false,
   null. Each function returns 1 on success, or 0 with r->error set. Anything unknown can be skipped. */
#pragma once

#include <stddef.h>

typedef struct {
    const char *p, *end;
    const char *error;
} json_reader;

int json_peek(json_reader *r);                          /* next character after spaces, or -1 at the end */
int json_take(json_reader *r, char c);                  /* takes c if it's next */
int json_word(json_reader *r, const char *w);           /* takes true / false / null */
int json_fail(json_reader *r, const char *why);         /* sets the error (first one wins), returns 0 */
int json_string(json_reader *r, char *out, size_t cap); /* cut short to fit */
int json_number(json_reader *r, double *out);
int json_bool(json_reader *r, int *out);                /* null counts as false */
int json_skip(json_reader *r, int depth);               /* any value */
int json_numbers(json_reader *r, float *out, int max, int *count); /* [n, ...]: first max into out, *count = all */

/* Walk an object's keys: `while (json_next_key(r, key, sizeof key, &first)) { ...read the value... }`. */
int json_next_key(json_reader *r, char *key, size_t cap, int *first);
/* Walk an array's items: `while (json_next_item(r, &first)) { ...read the item... }`. */
int json_next_item(json_reader *r, int *first);
