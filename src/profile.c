/* The Curve Profile graph's rules and Set From Terrain, ported line for line:
   web/gradient-overlay.js (pickPoint, insertPoint, dragPoint, snap, slopes, slopeReadout) and core/curved.py
   points_from_ground with core/gradient.py sample_bilinear and simplified_profile. No game calls. */
#include "curvetool.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define COLLINEAR_SLOPE (1.0 / 8192)  /* gradient.py _COLLINEAR_SLOPE */
#define STRAIGHT_RUN_CELLS 2.828427   /* _STRAIGHT_RUN_CELLS */
#define SIMPLIFY_PASSES 6             /* _SIMPLIFY_PASSES */
#define FIRST_TOLERANCE_FT (1.0 / 128) /* _FIRST_TOLERANCE_FT */

double profile_snap(double value, double granularity)
{
    return granularity > 0 ? floor(value / granularity + 0.5) * granularity : value;
}

int profile_pick(const curve *c, double s, double h, double radius)
{
    int best = -1;
    double best_d = radius;
    for (int i = 0; i < c->points; i++) {
        double d = hypot(c->p[i].s - s, c->p[i].h - h);
        if (d <= best_d)
            best = i, best_d = d;
    }
    return best;
}

int profile_insert(curve *c, double s, double h)
{
    if (c->points >= CURVE_MAX_POINTS)
        return -1;
    for (int k = 1; k < c->points; k++)
        if (c->p[k - 1].s < s && s < c->p[k].s) {
            memmove(&c->p[k + 1], &c->p[k], (size_t)(c->points - k) * sizeof c->p[0]);
            profile_point added = {c->p[k - 1].type, s, h};
            c->p[k] = added;
            c->points++;
            return k;
        }
    return -1;
}

int profile_drag(curve *c, int index, double s, double h, double granularity)
{
    s = profile_snap(s, granularity);
    h = profile_snap(h, granularity);
    if (index == 0 || index == c->points - 1) {
        c->p[index].h = h; /* the ends only move up and down */
        return index;
    }
    if (c->p[index - 1].s < s && s < c->p[index + 1].s) {
        c->p[index].s = s, c->p[index].h = h;
        return index;
    }
    /* Past a neighbour: the game deletes the point and the drag ends. */
    memmove(&c->p[index], &c->p[index + 1], (size_t)(c->points - index - 1) * sizeof c->p[0]);
    c->points--;
    return -1;
}

double profile_slope_readout(const curve *c, int index)
{
    int n = c->points;
    if (n < 2)
        return 0;
    int at = c->p[index].type == 1 ? index : index + 1;
    if (at > n - 1)
        at = n - 1;
    double secant_before = 0, secant_after = 0;
    if (at > 0) {
        double run = c->p[at].s - c->p[at - 1].s;
        secant_before = run > 0 ? (c->p[at].h - c->p[at - 1].h) / run : 0;
    }
    if (at < n - 1) {
        double run = c->p[at + 1].s - c->p[at].s;
        secant_after = run > 0 ? (c->p[at + 1].h - c->p[at].h) / run : 0;
    }
    if (at == 0)
        return secant_after;
    if (at == n - 1)
        return secant_before;
    if (c->p[at - 1].type != 1)
        return secant_before;
    return c->p[at].type == 1 ? (secant_before + secant_after) / 2 : secant_after;
}

/* ── Set From Terrain ── */

static double sample_bilinear(const float *grid, int width, double cell, double x, double z)
{
    double c = x / cell, r = z / cell;
    c = c < 0 ? 0 : c > width - 1 ? width - 1 : c;
    r = r < 0 ? 0 : r > width - 1 ? width - 1 : r;
    int c0 = (int)floor(c), r0 = (int)floor(r);
    int c1 = c0 + 1 < width ? c0 + 1 : width - 1, r1 = r0 + 1 < width ? r0 + 1 : width - 1;
    double fc = c - c0, fr = r - r0;
    double top = grid[(size_t)r0 * width + c0] * (1 - fc) + grid[(size_t)r0 * width + c1] * fc;
    double bottom = grid[(size_t)r1 * width + c0] * (1 - fc) + grid[(size_t)r1 * width + c1] * fc;
    return top * (1 - fr) + bottom * fr;
}

typedef struct {
    double *x, *z, *along;
    int n, cap;
} samples;

static void keep_sample(void *ctx, double x, double z, double along)
{
    samples *s = ctx;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 4096;
        double *nx = realloc(s->x, cap * sizeof *nx), *nz = realloc(s->z, cap * sizeof *nz), *na = realloc(s->along, cap * sizeof *na);
        if (nx)
            s->x = nx;
        if (nz)
            s->z = nz;
        if (na)
            s->along = na;
        if (!nx || !nz || !na)
            return;
        s->cap = cap;
    }
    s->x[s->n] = x, s->z[s->n] = z, s->along[s->n] = along, s->n++;
}

/* np.interp: xs increasing (repeats allowed), clamped at the ends. */
static double interp(double v, const double *xs, const double *ys, int n, int *hint)
{
    if (v <= xs[0])
        return ys[0];
    if (v >= xs[n - 1])
        return ys[n - 1];
    int i = *hint;
    while (i + 1 < n - 1 && xs[i + 1] <= v)
        i++;
    *hint = i;
    double run = xs[i + 1] - xs[i];
    return run > 0 ? ys[i] + (ys[i + 1] - ys[i]) * (v - xs[i]) / run : ys[i + 1];
}

/* The game's two simplifying steps (gradient.py simplified_profile) on n samples; the result goes into c's profile.
   Returns 0 if it doesn't fit. */
static int simplify_into(curve *c, double *ss, double *hs, int *curved, int n, double cell)
{
#define REMOVE(idx)                                                                \
    do {                                                                           \
        int at_ = (idx);                                                           \
        memmove(&ss[at_], &ss[at_ + 1], (size_t)(n - at_ - 1) * sizeof ss[0]);     \
        memmove(&hs[at_], &hs[at_ + 1], (size_t)(n - at_ - 1) * sizeof hs[0]);     \
        memmove(&curved[at_], &curved[at_ + 1], (size_t)(n - at_ - 1) * sizeof curved[0]); \
        n--;                                                                       \
    } while (0)
    /* 1. Straight runs become Line segments. */
    int i = n - 1;
    while (i >= 2) {
        int j = i - 1, k = i - 2;
        while (k >= 0) {
            double to_end = (hs[i] - hs[j]) / (ss[i] - ss[j]), before = (hs[j] - hs[k]) / (ss[j] - ss[k]);
            if (fabs(to_end - before) >= COLLINEAR_SLOPE)
                break;
            j--, k--;
        }
        int interior = i - k - 2, start = k + 1;
        if (interior > 0 && ss[i] - ss[start] >= cell * STRAIGHT_RUN_CELLS) {
            for (int index = i - 1; index > start; index--)
                REMOVE(index);
            curved[start] = 0;
            i = start;
        } else {
            i--;
        }
    }
    /* 2. Drop curve points that add nothing, coarser each pass. */
    double tolerance = FIRST_TOLERANCE_FT;
    for (int pass = 0; pass < SIMPLIFY_PASSES; pass++) {
        for (int m = n - 2; m > 0; m--) {
            if (m + 1 >= n || !(curved[m - 1] && curved[m]))
                continue;
            double t = (ss[m] - ss[m - 1]) / (ss[m + 1] - ss[m - 1]);
            if (fabs((hs[m] - hs[m - 1]) - t * (hs[m + 1] - hs[m - 1])) < tolerance)
                REMOVE(m);
        }
        tolerance *= 2;
    }
#undef REMOVE
    if (n > CURVE_MAX_POINTS)
        return 0;
    for (int m = 0; m < n; m++) {
        profile_point p = {curved[m], ss[m], hs[m]};
        c->p[m] = p;
    }
    c->points = n;
    return 1;
}

int profile_from_ground(curve *c, const float *grid, int width, double cell)
{
    samples path = {0};
    pen_sample(c, 0.25, keep_sample, &path); /* curved.py _MEASURE_STEP_FT */
    if (path.n < 2 || !(path.along[path.n - 1] > 0)) {
        free(path.x), free(path.z), free(path.along);
        return 0;
    }
    double total = path.along[path.n - 1];
    int n = (int)ceil(total / cell) + 1; /* arange(0, total, cell) plus the end */
    double *ss = malloc((size_t)n * sizeof *ss), *hs = malloc((size_t)n * sizeof *hs);
    int *curved = malloc((size_t)n * sizeof *curved);
    int ok = 0;
    if (ss && hs && curved) {
        int count = 0, hx = 0, hz = 0;
        for (double s = 0; s < total && count < n - 1; s = ++count * cell)
            ss[count] = s;
        /* np.arange stops before total; the computed count matches it except right at a multiple of cell. */
        ss[count++] = total;
        for (int m = 0; m < count; m++) {
            double x = interp(ss[m], path.along, path.x, path.n, &hx), z = interp(ss[m], path.along, path.z, path.n, &hz);
            hs[m] = sample_bilinear(grid, width, cell, x, z);
            curved[m] = 1;
        }
        ok = simplify_into(c, ss, hs, curved, count, cell);
    }
    free(ss), free(hs), free(curved), free(path.x), free(path.z), free(path.along);
    return ok;
}
