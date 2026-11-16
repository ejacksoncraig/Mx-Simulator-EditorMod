/* Baking a curve into the ground: core/curved.py paint_curve (and gradient.py brush_weight), line for line, including
   its float32 working arrays (nearest distance, distance along, side) so the heights match the app's, except for
   easing plain points (eased, below), which the app doesn't do yet. No game calls. */
#include "curvetool.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIN_CHORD_FT 0.5 /* curved.py _MIN_CHORD_FT */

typedef struct {
    double (*p)[2];
    double *along;
    int n, cap;
} chords;

static void keep(void *ctx, double x, double z, double along)
{
    chords *c = ctx;
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 1024;
        void *p = realloc(c->p, (size_t)cap * sizeof c->p[0]), *a = realloc(c->along, (size_t)cap * sizeof c->along[0]);
        if (!p || !a) {
            free(p ? p : c->p), free(a ? a : c->along);
            c->p = NULL, c->along = NULL, c->n = c->cap = 0;
            return;
        }
        c->p = p, c->along = a, c->cap = cap;
    }
    if (!c->p)
        return;
    c->p[c->n][0] = x, c->p[c->n][1] = z, c->along[c->n] = along;
    c->n++;
}

/* Easing plain points (on unless turned off; the app doesn't do this yet, so with it on the two bake differently).
   A plain point has handles of zero length, so where the next segment bends away the path leaves it at zero speed
   and turns almost on the spot (a radius of a few inches), which shows as a notch across the inside of a jump where a
   straight runs into a bend. For the bake only, a plain handle at a point where the path carries on within
   EASE_MAX_DEG of straight gets a short handle along the path (EASE_HANDLE of the way to that side's neighbour), so
   it turns over a few feet instead. Sharper corners stay sharp, the drawn line is unchanged, and the bake's line moves
   well under a foot near such a point (0.6 ft on the case it was tuned on). */
#define EASE_MAX_DEG 30.0
#define EASE_HANDLE 0.1

static int g_ease_plain = 1;
void curve_ease_plain(int on) { g_ease_plain = on != 0; }

static int unit_dir(double dx, double dz, double *ux, double *uz)
{
    double l = hypot(dx, dz);
    if (!(l > 1e-9))
        return 0;
    *ux = dx / l, *uz = dz / l;
    return 1;
}

/* The curve the bake uses: c itself, or an eased copy (only the path; no profile). Game thread only. */
static const curve *eased(const curve *c)
{
    static curve e;
    int n = c->anchors, changed = 0;
    if (!g_ease_plain || n < 3)
        return c;
    e.closed = c->closed, e.anchors = n, e.points = 0;
    memcpy(e.a, c->a, (size_t)n * sizeof c->a[0]);
    double most = cos(EASE_MAX_DEG * 3.14159265358979323846 / 180);
    for (int i = 0; i < n; i++) {
        int closed = c->closed && n > 2;
        if (!closed && (i == 0 || i == n - 1))
            continue; /* an open end has no neighbour on one side */
        const pen_anchor *a = &c->a[i], *prev = &c->a[(i + n - 1) % n], *next = &c->a[(i + 1) % n];
        int in_plain = a->ix == a->x && a->iz == a->z, out_plain = a->ox == a->x && a->oz == a->z;
        if (!in_plain && !out_plain)
            continue;
        /* The way the path arrives at a (and leaves it): its own handle if it has one, else toward the neighbour's
           nearer handle, else the neighbour itself. */
        double ix, iz, ox, oz;
        if (!(unit_dir(a->x - a->ix, a->z - a->iz, &ix, &iz) || unit_dir(a->x - prev->ox, a->z - prev->oz, &ix, &iz) ||
              unit_dir(a->x - prev->x, a->z - prev->z, &ix, &iz)))
            continue;
        if (!(unit_dir(a->ox - a->x, a->oz - a->z, &ox, &oz) || unit_dir(next->ix - a->x, next->iz - a->z, &ox, &oz) ||
              unit_dir(next->x - a->x, next->z - a->z, &ox, &oz)))
            continue;
        double tx, tz;
        if (ix * ox + iz * oz < most || !unit_dir(ix + ox, iz + oz, &tx, &tz))
            continue; /* a real corner: kept sharp */
        if (out_plain) {
            double reach = hypot(next->x - a->x, next->z - a->z) * EASE_HANDLE;
            e.a[i].ox = a->x + tx * reach, e.a[i].oz = a->z + tz * reach;
        }
        if (in_plain) {
            double reach = hypot(a->x - prev->x, a->z - prev->z) * EASE_HANDLE;
            e.a[i].ix = a->x - tx * reach, e.a[i].iz = a->z - tz * reach;
        }
        changed = 1;
    }
    return changed ? &e : c;
}

static int path_of(const curve *c, double cell, chords *out)
{
    memset(out, 0, sizeof *out);
    double step = cell > MIN_CHORD_FT ? cell : MIN_CHORD_FT;
    const curve *use = eased(c);
    double length = pen_sample(use, step, keep, out);
    if (use != c && out->n && length > 0) {
        /* The profile was made to the drawn line's length: spread it over the eased one the same way. */
        double scale = pen_sample(c, step, NULL, NULL) / length;
        for (int i = 0; i < out->n; i++)
            out->along[i] *= scale;
    }
    return out->n;
}

static int bounds_of(const chords *path, double cell, double outer, int width, int b[4])
{
    double minx = DBL_MAX, maxx = -DBL_MAX, minz = DBL_MAX, maxz = -DBL_MAX;
    for (int i = 0; i < path->n; i++) {
        minx = fmin(minx, path->p[i][0]), maxx = fmax(maxx, path->p[i][0]);
        minz = fmin(minz, path->p[i][1]), maxz = fmax(maxz, path->p[i][1]);
    }
    int col0 = (int)floor((minx - outer) / cell), col1 = (int)ceil((maxx + outer) / cell);
    int row0 = (int)floor((minz - outer) / cell), row1 = (int)ceil((maxz + outer) / cell);
    col0 = col0 < 0 ? 0 : col0, row0 = row0 < 0 ? 0 : row0;
    col1 = col1 > width - 1 ? width - 1 : col1, row1 = row1 > width - 1 ? width - 1 : row1;
    if (col0 > col1 || row0 > row1)
        return 0;
    b[0] = row0, b[1] = row1, b[2] = col0, b[3] = col1;
    return 1;
}

int curve_bounds(const curve *c, double cell, double outer, int width, int b[4])
{
    if (c->anchors < 2)
        return 0;
    chords path;
    int ok = path_of(c, cell, &path) && bounds_of(&path, cell, outer, width, b);
    free(path.p), free(path.along);
    return ok;
}

static double brush_weight(double distance, double inner, double outer)
{
    double u = (distance - inner) / (outer - inner);
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    return 1 - 3 * u * u + 2 * u * u * u;
}

int curve_paint(double *grid, int at_row, int at_col, int rows, int cols, int width, double cell, const curve *c, double inner,
                double outer, double strength, int add, int limit, double bank, int bake_side, int out_bounds[4])
{
    if (!(outer > 0) || inner < 0 || c->anchors < 2)
        return 0;
    chords path;
    int b[4];
    if (!path_of(c, cell, &path) || !bounds_of(&path, cell, outer, width, b)) {
        free(path.p), free(path.along);
        return 0;
    }
    int row0 = b[0], row1 = b[1], col0 = b[2], col1 = b[3];
    if (row0 < at_row || col0 < at_col || row1 >= at_row + rows || col1 >= at_col + cols) {
        free(path.p), free(path.along);
        return 0; /* the ground given doesn't cover the whole curve */
    }
    int h = row1 - row0 + 1, w = col1 - col0 + 1;
    size_t cells = (size_t)h * w;
    float *nearest = malloc(cells * sizeof(float)), *along = malloc(cells * sizeof(float)), *side = malloc(cells * sizeof(float));
    if (!nearest || !along || !side) {
        free(nearest), free(along), free(side), free(path.p), free(path.along);
        return 0;
    }
    for (size_t i = 0; i < cells; i++)
        nearest[i] = INFINITY, along[i] = 0, side[i] = 0;

    /* The nearest chord for each cell, looking only at the cells around each chord. */
    for (int i = 0; i + 1 < path.n; i++) {
        double ax = path.p[i][0], az = path.p[i][1], bx = path.p[i + 1][0], bz = path.p[i + 1][1];
        int c0 = (int)floor((fmin(ax, bx) - outer) / cell), c1 = (int)ceil((fmax(ax, bx) + outer) / cell);
        int r0 = (int)floor((fmin(az, bz) - outer) / cell), r1 = (int)ceil((fmax(az, bz) + outer) / cell);
        c0 = c0 < col0 ? col0 : c0, c1 = c1 > col1 ? col1 : c1, r0 = r0 < row0 ? row0 : r0, r1 = r1 > row1 ? row1 : r1;
        if (c0 > c1 || r0 > r1)
            continue;
        double dx = bx - ax, dz = bz - az, run = dx * dx + dz * dz;
        for (int r = r0; r <= r1; r++) {
            double z = r * cell;
            for (int col = c0; col <= c1; col++) {
                double x = col * cell;
                double t = run > 0 ? ((x - ax) * dx + (z - az) * dz) / run : 0;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                double ox = x - (ax + t * dx), oz = z - (az + t * dz), d = hypot(ox, oz);
                size_t k = (size_t)(r - row0) * w + (col - col0);
                if (d < nearest[k]) {
                    double cross = dx * oz - dz * ox;
                    nearest[k] = (float)d;
                    along[k] = (float)(path.along[i] + t * (path.along[i + 1] - path.along[i]));
                    side[k] = (float)((cross > 0 ? 1 : cross < 0 ? -1 : 0) * d);
                }
            }
        }
    }

    double s = strength < 0 ? 0 : strength > 1 ? 1 : strength;
    for (int r = 0; r < h; r++)
        for (int col = 0; col < w; col++) {
            size_t k = (size_t)r * w + col;
            if (!isfinite(nearest[k]))
                continue;
            /* One side only: side > 0 is right of the curve, looking from its start to its end on the map. The
               full-height core between the two inner lines is kept; only the other side's fade (inner to outer) is
               left out, so that side ends in a hard edge at its inner line. */
            if ((bake_side == CURVE_SIDE_LEFT && side[k] > inner) || (bake_side == CURVE_SIDE_RIGHT && side[k] < -inner))
                continue;
            double gap = nearest[k];
            double weight = outer > inner ? brush_weight(gap, inner, outer) : (gap <= outer ? 1 : 0);
            double *cell_h = &grid[(size_t)(row0 + r - at_row) * cols + (col0 + col - at_col)];
            double current = *cell_h;
            double height = profile_height(c->p, c->points, along[k]) + bank * side[k];
            double changed = current + s * weight * (add ? height : height - current);
            if (limit == CURVE_LIMIT_RAISE)
                changed = changed > current ? changed : current;
            else if (limit == CURVE_LIMIT_LOWER)
                changed = changed < current ? changed : current;
            *cell_h = changed;
        }
    free(nearest), free(along), free(side), free(path.p), free(path.along);
    if (out_bounds)
        memcpy(out_bounds, b, sizeof b);
    return 1;
}
