/* Baking a curve into the ground: core/curved.py paint_curve (and gradient.py brush_weight), line for line, including
   its float32 working arrays (nearest distance, distance along, side) so the heights match the app's. No game calls. */
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

static int path_of(const curve *c, double cell, chords *out)
{
    memset(out, 0, sizeof *out);
    pen_sample(c, cell > MIN_CHORD_FT ? cell : MIN_CHORD_FT, keep, out);
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
                double outer, double strength, int add, int limit, double bank, int out_bounds[4])
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
