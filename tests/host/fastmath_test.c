/* SPDX-License-Identifier: GPL-3.0-only */
/* fastmath.h against libm over each function's documented range. */
#include <math.h>
#include <stdio.h>
#include "../../firmware/src/dsp/fastmath.h"

static int fails;
static void check(const char *name, double err, double bound)
{
    printf("  %-8s max err %.3g (bound %.3g)%s\n", name, err, bound, err > bound ? "  FAIL" : "");
    if (err > bound)
        fails++;
}

int main(void)
{
    double e;
    float x;
    int i;
    for (e = 0, i = 0; i <= 200000; i++) {                 /* exp2f, relative */
        x = -126.0f + 253.0f * (float)i / 200000.0f;
        double r = exp2((double)x), d = fabs(fm_exp2f(x) - r) / r;
        if (d > e) e = d;
    }
    check("exp2f", e, 3e-7);
    for (e = 0, i = 0; i <= 200000; i++) {
        x = -87.0f + 175.0f * (float)i / 200000.0f;
        double r = exp((double)x), d = fabs(fm_expf(x) - r) / r;
        if (d > e) e = d;
    }
    check("expf", e, 6e-7);
    for (e = 0, i = 0; i <= 200000; i++) {                 /* log2f, absolute, 1e-30..1e30 */
        x = (float)pow(10.0, -30.0 + 60.0 * i / 200000.0);
        double d = fabs(fm_log2f(x) - log2((double)x));
        if (d > e) e = d;
    }
    check("log2f", e, 4e-6);                               /* |log2| up to 100: ~2 ulp of the result */
    for (e = 0, i = 0; i <= 200000; i++) {                 /* near 1, where it matters */
        x = 0.25f + 3.75f * (float)i / 200000.0f;
        double d = fabs(fm_log2f(x) - log2((double)x));
        if (d > e) e = d;
    }
    check("log2f@1", e, 3e-7);
    for (e = 0, i = 0; i <= 100000; i++) {
        float b = 0.01f + 100.0f * (float)i / 100000.0f, y = -3.0f + 6.0f * (float)(i % 997) / 997.0f;
        double r = pow((double)b, (double)y);
        if (r < 1e-30 || r > 1e30) continue;
        double d = fabs(fm_powf(b, y) - r) / r;
        if (d > e) e = d;
    }
    check("powf", e, 1e-5);
    for (e = 0, i = 0; i <= 200000; i++) {
        x = -12.0f + 24.0f * (float)i / 200000.0f;
        double d = fabs(fm_tanhf(x) - tanh((double)x));
        if (d > e) e = d;
    }
    check("tanhf", e, 3e-7);
    for (e = 0, i = 0; i <= 400000; i++) {
        x = -100.0f + 200.0f * (float)i / 400000.0f;
        double d = fabs(fm_sinf(x) - sin((double)x));
        if (d > e) e = d;
        d = fabs(fm_cosf(x) - cos((double)x));
        if (d > e) e = d;
    }
    check("sin/cos", e, 1e-6);
    for (e = 0, i = 0; i <= 200000; i++) {
        x = -3.14159f + 6.28318f * (float)i / 200000.0f;
        double d = fabs(fm_sinf(x) - sin((double)x));
        if (d > e) e = d;
    }
    check("sin[-pi,pi]", e, 6e-7);                         /* the result near +-pi is ~ulp(pi) */
    for (e = 0, i = 1; i <= 200000; i++) {                 /* tan on the filter's range, 0..1.5 */
        x = 1.5f * (float)i / 200000.0f;
        double r = tan((double)x), d = fabs(fm_tanf(x) - r) / r;
        if (d > e) e = d;
    }
    check("tanf", e, 1e-6);
    for (e = 0, i = 1; i <= 200000; i++) {
        x = (float)pow(10.0, -20.0 + 40.0 * i / 200000.0);
        double r = sqrt((double)x), d = fabs(fm_sqrtf(x) - r) / r;
        if (d > e) e = d;
    }
    check("sqrtf", e, 3e-7);
    printf("fastmath: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
