/* Reads "vx vz mid half" lines, prints sweep_s for each (tests/test_sweep.py). */
#include <stdio.h>

double sweep_s(double vx, double vz, double mid, double half);

int main(void)
{
    double vx, vz, mid, half;
    while (scanf("%lf %lf %lf %lf", &vx, &vz, &mid, &half) == 4)
        printf("%.17g\n", sweep_s(vx, vz, mid, half));
    return 0;
}
