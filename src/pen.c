/* The pen and the profile rules, ported line for line so the game's curve tool behaves like the app's:
   web/pen-path.js (anchors, handles, split, corners, nearest on path, offset lines, spots along) and core/curved.py /
   core/gradient.py (sample_path, path_length, extended_points, stretched_points, profile_heights). No game calls here,
   so it builds and tests natively. */
#include "curvetool.h"

#include <math.h>
#include <string.h>

pen_anchor pen_new_anchor(double x, double z)
{
    pen_anchor a = {x, z, x, z, x, z};
    return a;
}

int pen_is_smooth(const pen_anchor *a) { return a->ix != a->x || a->iz != a->z || a->ox != a->x || a->oz != a->z; }

void pen_bezier(const pen_anchor *a, const pen_anchor *b, double t, double *x, double *z)
{
    double u = 1 - t, c0 = u * u * u, c1 = 3 * u * u * t, c2 = 3 * u * t * t, c3 = t * t * t;
    *x = c0 * a->x + c1 * a->ox + c2 * b->ix + c3 * b->x;
    *z = c0 * a->z + c1 * a->oz + c2 * b->iz + c3 * b->z;
}

int pen_segments(const curve *c) { return c->anchors < 2 ? 0 : c->anchors - 1 + (c->closed && c->anchors > 2); }

void pen_segment(const curve *c, int k, int *a, int *b)
{
    *a = k;
    *b = (k + 1) % c->anchors;
}

/* de Casteljau: a new point at t without changing the shape; a's and b's handles on that side change. */
pen_anchor pen_split(pen_anchor *a, pen_anchor *b, double t)
{
#define MIX(p, q) ((p) + ((q) - (p)) * t)
    double p1x = MIX(a->x, a->ox), p1z = MIX(a->z, a->oz);
    double midx = MIX(a->ox, b->ix), midz = MIX(a->oz, b->iz);
    double p3x = MIX(b->ix, b->x), p3z = MIX(b->iz, b->z);
    double q1x = MIX(p1x, midx), q1z = MIX(p1z, midz);
    double q2x = MIX(midx, p3x), q2z = MIX(midz, p3z);
    pen_anchor out = {MIX(q1x, q2x), MIX(q1z, q2z), q1x, q1z, q2x, q2z};
#undef MIX
    a->ox = p1x, a->oz = p1z;
    b->ix = p3x, b->iz = p3z;
    return out;
}

void pen_pull(pen_anchor *a, double x, double z, int split)
{
    a->ox = x, a->oz = z;
    if (!split)
        a->ix = a->x * 2 - x, a->iz = a->z * 2 - z;
}

/* The other handle keeps its length and swings round to stay in line, unless split. */
void pen_move_handle(pen_anchor *a, int out_side, double x, double z, int split)
{
    double *ownx = out_side ? &a->ox : &a->ix, *ownz = out_side ? &a->oz : &a->iz;
    double *othx = out_side ? &a->ix : &a->ox, *othz = out_side ? &a->iz : &a->oz;
    *ownx = x, *ownz = z;
    double reach = hypot(*othx - a->x, *othz - a->z);
    double dx = x - a->x, dz = z - a->z, length = hypot(dx, dz);
    if (!split && reach && length)
        *othx = a->x - dx / length * reach, *othz = a->z - dz / length * reach;
}

void pen_move_anchor(pen_anchor *a, double x, double z)
{
    double dx = x - a->x, dz = z - a->z;
    a->x += dx, a->z += dz, a->ix += dx, a->iz += dz, a->ox += dx, a->oz += dz;
}

/* A corner becomes a curve, with handles along the line between its neighbours, a third of the way to each. */
void pen_toggle_corner(curve *c, int i)
{
    pen_anchor *a = &c->a[i];
    if (pen_is_smooth(a)) {
        a->ix = a->ox = a->x;
        a->iz = a->oz = a->z;
        return;
    }
    const pen_anchor *before = i > 0 ? &c->a[i - 1] : c->closed ? &c->a[c->anchors - 1] : a;
    const pen_anchor *after = i + 1 < c->anchors ? &c->a[i + 1] : c->closed ? &c->a[0] : a;
    double dx = after->x - before->x, dz = after->z - before->z, length = hypot(dx, dz);
    if (!length)
        length = 1;
    double to_before = hypot(a->x - before->x, a->z - before->z) / 3, to_after = hypot(after->x - a->x, after->z - a->z) / 3;
    a->ix = a->x - dx / length * to_before, a->iz = a->z - dz / length * to_before;
    a->ox = a->x + dx / length * to_after, a->oz = a->z + dz / length * to_after;
}

/* The path as short chords, like curved.py sample_path: each segment gets ceil(rough / step) + 1 samples (at least 2),
   rough being its control polygon's length. Calls out(x, z, along) for each point; returns the total length. */
double pen_sample(const curve *c, double step, void (*out)(void *, double, double, double), void *ctx)
{
    double along = 0, lx = 0, lz = 0;
    int have = 0;
    for (int k = 0; k < pen_segments(c); k++) {
        int ia, ib;
        pen_segment(c, k, &ia, &ib);
        const pen_anchor *a = &c->a[ia], *b = &c->a[ib];
        double rough = hypot(a->ox - a->x, a->oz - a->z) + hypot(b->ix - a->ox, b->iz - a->oz) + hypot(b->x - b->ix, b->z - b->iz);
        int n = (int)ceil(rough / step) + 1;
        if (n < 2)
            n = 2;
        for (int j = have ? 1 : 0; j < n; j++) {
            double x, z;
            pen_bezier(a, b, (double)j / (n - 1), &x, &z);
            if (have)
                along += hypot(x - lx, z - lz);
            if (out)
                out(ctx, x, z, along);
            lx = x, lz = z, have = 1;
        }
    }
    return along;
}

double pen_length(const curve *c) { return c->anchors > 1 ? pen_sample(c, 0.25, NULL, NULL) : 0; }

/* ── the profile: gradient.py profile_heights, curved.py flat / stretched / extended ── */

double profile_height(const profile_point *p, int n, double s)
{
    if (n <= 0)
        return 0;
    if (n == 1 || s <= p[0].s)
        return p[0].h;
    if (s >= p[n - 1].s)
        return p[n - 1].h;
    int seg = 0;
    while (seg < n - 2 && p[seg + 1].s <= s)
        seg++;
    double secant[CURVE_MAX_POINTS];
    for (int i = 0; i + 1 < n; i++) {
        double run = p[i + 1].s - p[i].s;
        secant[i] = run > 0 ? (p[i + 1].h - p[i].h) / run : 0;
    }
    double slope[2];
    for (int e = 0; e < 2; e++) {
        int i = seg + e;
        if (i == 0)
            slope[e] = secant[0];
        else if (i == n - 1)
            slope[e] = secant[n - 2];
        else if (p[i - 1].type != 1)
            slope[e] = secant[i - 1];
        else if (p[i].type == 1)
            slope[e] = (secant[i - 1] + secant[i]) / 2;
        else
            slope[e] = secant[i];
    }
    double length = p[seg + 1].s - p[seg].s, t = length > 0 ? (s - p[seg].s) / length : 0;
    double h0 = p[seg].h, h1 = p[seg + 1].h;
    if (p[seg].type != 1)
        return h0 + t * (h1 - h0);
    double m0 = slope[0] * length, m1 = slope[1] * length;
    return h0 + t * (m0 + t * (3 * (h1 - h0) - 2 * m0 - m1 + t * (m0 + m1 + 2 * (h0 - h1))));
}

static void flat(curve *c, double length)
{
    profile_point a = {0, 0, 0}, b = {0, length, 0};
    c->p[0] = a, c->p[1] = b, c->points = 2;
}

/* A new length for the path: stretch keeps the shape (every point the same fraction along); extend moves no point,
   adding flat ground at the last height or cutting off at the height there (curved.py extended_points). */
void profile_fit(curve *c, double length, int stretch)
{
    int n = c->points;
    profile_point *p = c->p;
    if (stretch) {
        double end = n ? p[n - 1].s : 0;
        if (!n || end <= 0) {
            flat(c, length);
            return;
        }
        for (int i = 0; i < n; i++)
            p[i].s = p[i].s * length / end;
        return;
    }
    if (!n || p[n - 1].s <= 0 || length <= 0) {
        flat(c, length > 0 ? length : 0);
        return;
    }
    profile_point last = p[n - 1];
    if (length == last.s)
        return;
    if (length < last.s) {
        double cut = profile_height(p, n, length);
        int keep = 0;
        while (keep < n && p[keep].s < length)
            keep++;
        profile_point end = {0, length, cut};
        p[keep] = end;
        c->points = keep + 1;
        return;
    }
    if (n > 1 && p[n - 2].type != 1 && p[n - 2].h == last.h) {
        profile_point end = {0, length, last.h};
        p[n - 1] = end;
        return;
    }
    if (n + 1 > CURVE_MAX_POINTS)
        return;
    profile_point keep = {0, last.s, last.h}, end = {0, length, last.h};
    p[n - 1] = keep, p[n] = end;
    c->points = n + 1;
}

/* ── picking and showing, in window px (web/pen-path.js nearestOnPath, offsetLines, spotsAlongPath) ── */

/* The place on the path nearest (px, py), within radius px: 1 and *hit filled, else 0. */
int pen_nearest(const curve *c, void (*to_px)(void *, double, double, double *, double *), void *ctx, double px, double py,
                double radius, pen_hit *hit)
{
    double best = radius;
    int found = 0;
    for (int k = 0; k < pen_segments(c); k++) {
        int ia, ib;
        pen_segment(c, k, &ia, &ib);
        const pen_anchor *a = &c->a[ia], *b = &c->a[ib];
        double span = 0, lx = 0, ly = 0;
        for (int j = 0; j <= 12; j++) {
            double x, z, sx, sy;
            pen_bezier(a, b, j / 12.0, &x, &z);
            to_px(ctx, x, z, &sx, &sy);
            if (j)
                span += hypot(sx - lx, sy - ly);
            lx = sx, ly = sy;
        }
        int pieces = (int)ceil(span / 6);
        pieces = pieces < 12 ? 12 : pieces > 2000 ? 2000 : pieces;
        double fx, fy, x, z;
        pen_bezier(a, b, 0, &x, &z);
        to_px(ctx, x, z, &fx, &fy);
        for (int j = 1; j <= pieces; j++) {
            double tx, ty;
            pen_bezier(a, b, (double)j / pieces, &x, &z);
            to_px(ctx, x, z, &tx, &ty);
            double dx = tx - fx, dy = ty - fy, run = dx * dx + dy * dy;
            double along = run > 0 ? ((px - fx) * dx + (py - fy) * dy) / run : 0;
            along = along < 0 ? 0 : along > 1 ? 1 : along;
            double d = hypot(px - (fx + along * dx), py - (fy + along * dy));
            if (d <= best) {
                best = d;
                hit->segment = k, hit->t = (j - 1 + along) / pieces, hit->distance = d;
                found = 1;
            }
            fx = tx, fy = ty;
        }
    }
    return found;
}

/* The path sampled per_segment times a segment (pen-path.js's shared sampling); returns the count written. */
int pen_points(const curve *c, int per_segment, double (*out)[2], int max)
{
    int n = 0;
    for (int k = 0; k < pen_segments(c); k++) {
        int ia, ib;
        pen_segment(c, k, &ia, &ib);
        for (int j = n ? 1 : 0; j <= per_segment && n < max; j++, n++)
            pen_bezier(&c->a[ia], &c->a[ib], (double)j / per_segment, &out[n][0], &out[n][1]);
    }
    return n;
}

/* A line at distance d to the side of points (positive = the left of travel in x right, z down; as offsetLines). */
void pen_offset(const double (*path)[2], int n, double d, double (*out)[2])
{
    for (int i = 0; i < n; i++) {
        const double *from = path[i > 0 ? i - 1 : 0], *to = path[i + 1 < n ? i + 1 : n - 1];
        double dx = to[0] - from[0], dz = to[1] - from[1], length = hypot(dx, dz);
        if (!length)
            length = 1;
        out[i][0] = path[i][0] - dz / length * d;
        out[i][1] = path[i][1] + dx / length * d;
    }
}

/* Where each distance falls on the path, by the fraction of `total` (or feet along when total < 0). Returns 0 if
   there's no path. */
int pen_spots(const curve *c, const double *distances, int count, double total, double (*out)[2])
{
    static double path[CURVE_MAX_ANCHORS * 97][2], along[CURVE_MAX_ANCHORS * 97];
    int n = pen_points(c, 96, path, CURVE_MAX_ANCHORS * 97);
    if (n < 2 || (total >= 0 && !(total > 0)))
        return 0;
    along[0] = 0;
    for (int i = 1; i < n; i++)
        along[i] = along[i - 1] + hypot(path[i][0] - path[i - 1][0], path[i][1] - path[i - 1][1]);
    double length = along[n - 1];
    for (int k = 0; k < count; k++) {
        double d = distances[k], want;
        if (total < 0)
            want = d < 0 ? 0 : d > length ? length : d;
        else {
            double f = d / total;
            want = (f < 0 ? 0 : f > 1 ? 1 : f) * length;
        }
        int low = 0, high = n - 1;
        while (high - low > 1) {
            int mid = (low + high) >> 1;
            if (along[mid] <= want)
                low = mid;
            else
                high = mid;
        }
        double run = along[high] - along[low], f = run > 0 ? (want - along[low]) / run : 0;
        out[k][0] = path[low][0] + (path[high][0] - path[low][0]) * f;
        out[k][1] = path[low][1] + (path[high][1] - path[low][1]) * f;
    }
    return 1;
}
