/* The Curve Profile graph's rules, ported line for line from web/gradient-overlay.js (pickPoint, insertPoint, dragPoint,
   snap, slopes, slopeReadout), and the mod's Set From Terrain, which keeps jump lips and sharp edges the game's own
   method rounds off (game/editor-gradients-ui.md has the game's). No game calls. */
#include "curvetool.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define COLLINEAR_SLOPE (1.0 / 8192)  /* gradient.py _COLLINEAR_SLOPE */
#define STRAIGHT_RUN_CELLS 2.828427   /* _STRAIGHT_RUN_CELLS */
#define TRACE_SAMPLES_PER_CELL 4      /* how finely Set From Terrain reads the ground */
#define CORNER_BASELINE 4             /* samples each side a slope is measured over: one cell */
#define CORNER_ARM 2                  /* samples from a sharp edge to the end of its Line: half a cell */
#define CORNER_SLOPE 0.25             /* the slope change (rise/run) that counts as a sharp edge */
#define TRACE_MAX_SWEEPS 16

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

/* Moving a range of points as one (the mod's own addition, no game rule to match). The held point goes under the mouse,
   snapped like a single drag; every other point in the range moves by the same amount, so the shape is kept. The range
   stops just short of its unselected neighbours instead of passing them (a single drag deletes the point there; a
   group would lose its shape), and with an end of the profile in it, it only moves up and down, as an end does. */
#define GROUP_GAP_FT (1.0 / 64) /* the closest the range comes to a neighbour */
void profile_group_drag(const profile_point *from, int n, int first, int last, int held, double s, double h,
                        double granularity, profile_point *out)
{
    memcpy(out, from, (size_t)n * sizeof *out);
    if (n < 2 || first < 0 || last >= n || first > last || held < first || held > last)
        return;
    double ds = profile_snap(s, granularity) - from[held].s;
    double dh = profile_snap(h, granularity) - from[held].h;
    if (first == 0 || last == n - 1) {
        ds = 0;
    } else {
        double lo = from[first - 1].s + GROUP_GAP_FT - from[first].s; /* the furthest back it can go (<= 0) */
        double hi = from[last + 1].s - GROUP_GAP_FT - from[last].s;   /* the furthest on (>= 0) */
        lo = lo > 0 ? 0 : lo;
        hi = hi < 0 ? 0 : hi;
        ds = ds < lo ? lo : ds > hi ? hi : ds;
    }
    for (int i = first; i <= last; i++)
        out[i].s = from[i].s + ds, out[i].h = from[i].h + dh;
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

/* The tracer's working profile: which samples are still points, each one's segment type, and whether it's held in place
   because it shapes a sharp edge. */
typedef struct {
    const double *ss, *hs;
    int *at, *type, *held;
    int n;
} trace;

static void trace_remove(trace *t, int i)
{
    size_t move = (size_t)(t->n - i - 1) * sizeof(int);
    memmove(&t->at[i], &t->at[i + 1], move);
    memmove(&t->type[i], &t->type[i + 1], move);
    memmove(&t->held[i], &t->held[i + 1], move);
    t->n--;
}

/* A sharp edge is where the ground's slope, measured one cell each side, changes by CORNER_SLOPE or more and more than
   anywhere within a cell of it. A Curve always leaves a point tangent to its neighbour, so only a point with a Line on
   both sides can hold a kink (see profile_height): each edge gets a short Line down each face. Returns how many. */
static int trace_corners(trace *t)
{
    int n = t->n, corners = 0;
    const double *ss = t->ss, *hs = t->hs;
    double *kink = calloc((size_t)n, sizeof *kink);
    int *line = calloc((size_t)n, sizeof *line);
    if (!kink || !line) {
        free(kink), free(line);
        return 0;
    }
    for (int i = CORNER_BASELINE; i < n - CORNER_BASELINE; i++) {
        double before = (hs[i] - hs[i - CORNER_BASELINE]) / (ss[i] - ss[i - CORNER_BASELINE]);
        double after = (hs[i + CORNER_BASELINE] - hs[i]) / (ss[i + CORNER_BASELINE] - ss[i]);
        kink[i] = fabs(after - before);
    }
    for (int i = CORNER_BASELINE; i < n - CORNER_BASELINE; i++) {
        if (kink[i] < CORNER_SLOPE)
            continue;
        int peak = 1;
        for (int j = i - CORNER_BASELINE; peak && j <= i + CORNER_BASELINE; j++)
            peak = kink[j] < kink[i] || j == i || (kink[j] == kink[i] && j > i);
        if (!peak)
            continue;
        corners++;
        t->held[i] = t->held[i - CORNER_ARM] = t->held[i + CORNER_ARM] = 1;
        for (int j = i - CORNER_ARM; j < i + CORNER_ARM; j++)
            line[j] = 1;
    }
    for (int i = 0; i < n; i++)
        t->type[i] = !line[i];
    /* The samples inside those Lines stop being points. */
    for (int i = n - 2; i > 0; i--)
        if (!t->held[i] && line[i - 1] && line[i])
            trace_remove(t, i);
    free(kink), free(line);
    return corners;
}

/* The game's first simplifying step (gradient.py simplified_profile): straight runs become one Line. */
static void trace_straight_runs(trace *t, double cell)
{
    const double *ss = t->ss, *hs = t->hs;
#define S(k) ss[t->at[k]]
#define H(k) hs[t->at[k]]
    int i = t->n - 1;
    while (i >= 2) {
        int j = i - 1, k = i - 2;
        while (k >= 0) {
            double to_end = (H(i) - H(j)) / (S(i) - S(j)), before = (H(j) - H(k)) / (S(j) - S(k));
            if (fabs(to_end - before) >= COLLINEAR_SLOPE)
                break;
            j--, k--;
        }
        int interior = i - k - 2, start = k + 1;
        if (interior > 0 && S(i) - S(start) >= cell * STRAIGHT_RUN_CELLS) {
            for (int index = i - 1; index > start; index--)
                trace_remove(t, index);
            t->type[start] = 0;
            i = start;
        } else {
            i--;
        }
    }
#undef S
#undef H
}

/* Would taking point m out keep the rebuilt curve within tolerance of every ground sample it can change? Removing it
   changes the slopes at m-1 and m+1, so the segments out to m-2 and m+2; the slopes there need one point further out. */
static int trace_can_remove(const trace *t, int m, double tolerance)
{
    profile_point near_points[6];
    int count = 0;
    for (int k = m - 3; k <= m + 3; k++)
        if (k >= 0 && k < t->n && k != m) {
            profile_point p = {t->type[k], t->ss[t->at[k]], t->hs[t->at[k]]};
            near_points[count++] = p;
        }
    int from = t->at[m >= 2 ? m - 2 : 0], to = t->at[m + 2 < t->n ? m + 2 : t->n - 1];
    for (int i = from; i <= to; i++)
        if (fabs(profile_height(near_points, count, t->ss[i]) - t->hs[i]) >= tolerance)
            return 0;
    return 1;
}

static void trace_thin(trace *t, double tolerance)
{
    int changed = 1;
    for (int sweep = 0; changed && sweep < TRACE_MAX_SWEEPS; sweep++) {
        changed = 0;
        for (int m = t->n - 2; m > 0; m--) {
            if (m >= t->n - 1 || t->held[m] || t->type[m - 1] != 1 || t->type[m] != 1)
                continue;
            if (trace_can_remove(t, m, tolerance)) {
                trace_remove(t, m);
                changed = 1;
            }
        }
    }
}

int profile_trace_ground(curve *c, const float *grid, int width, double cell, double tolerance, trace_result *out)
{
    samples path = {0};
    pen_sample(c, 0.25, keep_sample, &path); /* curved.py _MEASURE_STEP_FT */
    if (path.n < 2 || !(path.along[path.n - 1] > 0) || !(cell > 0) || !(tolerance > 0)) {
        free(path.x), free(path.z), free(path.along);
        return 0;
    }
    double total = path.along[path.n - 1], step = cell / TRACE_SAMPLES_PER_CELL;
    int n = (int)ceil(total / step) + 1;
    double *ss = malloc((size_t)n * sizeof *ss), *hs = malloc((size_t)n * sizeof *hs);
    int *at = malloc((size_t)n * sizeof *at), *type = malloc((size_t)n * sizeof *type), *held = calloc((size_t)n, sizeof *held);
    int ok = 0;
    if (ss && hs && at && type && held) {
        int count = 0, hx = 0, hz = 0;
        for (double s = 0; s < total && count < n - 1; s = ++count * step)
            ss[count] = s;
        if (count > 0 && total - ss[count - 1] < step / 16)
            count--; /* no sliver of a segment at the end */
        ss[count++] = total;
        for (int m = 0; m < count; m++) {
            double x = interp(ss[m], path.along, path.x, path.n, &hx), z = interp(ss[m], path.along, path.z, path.n, &hz);
            hs[m] = sample_bilinear(grid, width, cell, x, z);
            at[m] = m, type[m] = 1;
        }
        trace t = {ss, hs, at, type, held, count};
        int corners = trace_corners(&t);
        trace_straight_runs(&t, cell);
        double used = tolerance;
        trace_thin(&t, used);
        /* Too many points for a profile: loosen until it fits. The result says what it took. */
        while (t.n > CURVE_MAX_POINTS && used < 0.5) {
            used *= 2;
            trace_thin(&t, used);
        }
        /* Ground so rough its held edges alone don't fit: let go of them, then loosen only as far as it takes. */
        if (t.n > CURVE_MAX_POINTS) {
            corners = 0;
            for (int m = 0; m < t.n; m++)
                t.type[m] = 1, t.held[m] = 0;
            trace_thin(&t, used);
            while (t.n > CURVE_MAX_POINTS && used < 64) {
                used *= 1.25;
                trace_thin(&t, used);
            }
        }
        if (t.n <= CURVE_MAX_POINTS) {
            for (int m = 0; m < t.n; m++) {
                profile_point p = {t.type[m], ss[at[m]], hs[at[m]]};
                c->p[m] = p;
            }
            c->points = t.n;
            if (out) {
                double worst = 0;
                for (int m = 0; m < count; m++) {
                    double e = fabs(profile_height(c->p, c->points, ss[m]) - hs[m]);
                    worst = e > worst ? e : worst;
                }
                out->corners = corners, out->worst_ft = worst, out->tolerance_ft = used;
            }
            ok = 1;
        }
    }
    free(ss), free(hs), free(at), free(type), free(held), free(path.x), free(path.z), free(path.along);
    return ok;
}
