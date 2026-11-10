/* The radial brushes' s with any sweep. Pure math, no game calls.

   The game (0x14005fa10): v = cell - A; behind A (dot(v, B - A) < 0) v loses its part along A to B; s = |v|. That is a
   180 degree sweep centred on B, running straight past its edges. Here: inside the wedge (within half of its middle
   direction) s = |v|; outside it, s = v's length along the nearer edge, or 0 if v points behind that edge too. At 180
   degrees centred on B both rules give the same s. half >= pi is a full circle. */
#include <math.h>

#define PI 3.14159265358979323846

double sweep_s(double vx, double vz, double mid, double half)
{
    double s = sqrt(vx * vx + vz * vz);
    if (half >= PI)
        return s;
    double d = fmod(atan2(vz, vx) - mid + PI, 2 * PI);
    d = (d < 0 ? d + 2 * PI : d) - PI;
    if (fabs(d) <= half)
        return s;
    double edge = mid + (d > 0 ? half : -half), along = vx * cos(edge) + vz * sin(edge);
    return along > 0 ? along : 0;
}

double sweep_weight(double vx, double vz, double mid, double half, double s, double end_s, double fade)
{
    double gap = s - end_s;
    if (half < PI) {
        double d = fmod(atan2(vz, vx) - mid + PI, 2 * PI);
        d = (d < 0 ? d + 2 * PI : d) - PI;
        if (fabs(d) > half) {
            /* How far past the nearer edge: straight out from it, or from A when it's behind that edge too. */
            double edge = mid + (d > 0 ? half : -half), along = vx * cos(edge) + vz * sin(edge);
            double out = along > 0 ? fabs(-vx * sin(edge) + vz * cos(edge)) : sqrt(vx * vx + vz * vz);
            gap = out > gap ? out : gap;
        }
    }
    if (gap <= 0)
        return 1;
    if (gap >= fade)
        return 0;
    double u = gap / fade;
    return 1 - 3 * u * u + 2 * u * u * u; /* the brush's own falloff */
}
