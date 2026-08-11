/* Rae binding for the Hosek-Wilkie analytic sky model.
 *
 * The COEFFICIENT DATASET is the authors' published data, vendored verbatim in
 * ArHosekSkyModelData_RGB.h with its licence header intact. The two "cook"
 * routines below are transcribed from the reference implementation
 * (ArHosekSkyModel.c, same distribution) because they are the documented way to
 * read that dataset: a quintic Bezier in the cube root of solar elevation,
 * blended over ground albedo and over the integer/fractional parts of
 * turbidity.
 *
 * Derived from the sample implementation of the models presented in
 *   "An Analytic Model for Full Spectral Sky-Dome Radiance" (SIGGRAPH 2012)
 *   "Adding a Solar Radiance Function to the Hosek Skylight Model" (2013)
 * by Lukas Hosek and Alexander Wilkie, Charles University in Prague.
 *
 * Copyright (c) 2012 - 2013, Lukas Hosek and Alexander Wilkie
 * All rights reserved. Published under the 3-clause BSD licence; the full text
 * is retained at the top of ArHosekSkyModelData_RGB.h in this directory.
 *
 * WHY ONLY THE DATASET AND THE COOK LIVE IN C: the per-pixel radiance formula
 * is nine multiply-adds and is implemented twice on our side — once in Rae
 * (lib/sky.rae, for CPU-side irradiance and for tests) and once in WGSL (the
 * deferred lighting pass). Only the dataset interpolation needs the 3861-line
 * table, and it runs once per sky change rather than once per pixel. The
 * spectral dataset from the same distribution is 33770 lines and is NOT
 * vendored: nothing here needs it.
 */

#include "rae_hosek.h"
#include "ArHosekSkyModelData_RGB.h"
#include <math.h>
#include <string.h>

/* Quintic Bezier over solar elevation, lerped across albedo and turbidity.
 * `dataset` is one channel's 9x6 coefficient block per (albedo, turbidity). */
static void hosek_cook_config(const double* dataset,
                              double* config,      /* out: 9 doubles */
                              double turbidity,
                              double albedo,
                              double solar_elevation) {
    int int_turbidity = (int)turbidity;
    double turbidity_rem = turbidity - (double)int_turbidity;
    /* The dataset is indexed by turbidity 1..10; clamp so a caller asking for
     * a clear-sky 1.0 or a hazy 10.0 lands in range instead of reading past
     * the table. The reference implementation asserts instead. */
    if (int_turbidity < 1) { int_turbidity = 1; turbidity_rem = 0.0; }
    if (int_turbidity > 9) { int_turbidity = 9; turbidity_rem = 1.0; }

    solar_elevation = pow(solar_elevation / (RAE_HOSEK_PI / 2.0), (1.0 / 3.0));

    const double s = solar_elevation;
    const double s1 = 1.0 - s;
    /* Quintic Bezier weights, hoisted: the reference recomputes these pow()
     * calls inside every one of the four blend terms. */
    const double w[6] = {
        s1 * s1 * s1 * s1 * s1,
        5.0 * s1 * s1 * s1 * s1 * s,
        10.0 * s1 * s1 * s1 * s * s,
        10.0 * s1 * s1 * s * s * s,
        5.0 * s1 * s * s * s * s,
        s * s * s * s * s
    };

    /* Four corners of the (albedo, turbidity) bilinear blend. Dataset layout:
     * albedo block = 9 coefficients x 6 Bezier control points x 10 turbidities. */
    const double* m[4];
    m[0] = dataset + (9 * 6 * (int_turbidity - 1));               /* alb 0, turb lo */
    m[1] = dataset + (9 * 6 * int_turbidity);                     /* alb 0, turb hi */
    m[2] = dataset + (9 * 6 * 10) + (9 * 6 * (int_turbidity - 1));/* alb 1, turb lo */
    m[3] = dataset + (9 * 6 * 10) + (9 * 6 * int_turbidity);      /* alb 1, turb hi */

    const double weight[4] = {
        (1.0 - albedo) * (1.0 - turbidity_rem),
        (1.0 - albedo) * turbidity_rem,
        albedo * (1.0 - turbidity_rem),
        albedo * turbidity_rem
    };

    for (int i = 0; i < 9; i++) {
        double acc = 0.0;
        for (int c = 0; c < 4; c++) {
            const double* em = m[c];
            double bez = 0.0;
            for (int k = 0; k < 6; k++) bez += w[k] * em[i + 9 * k];
            acc += weight[c] * bez;
        }
        config[i] = acc;
    }
}

/* Same blend, but the radiance dataset carries a single value per control
 * point rather than nine. */
static double hosek_cook_radiance(const double* dataset,
                                  double turbidity,
                                  double albedo,
                                  double solar_elevation) {
    int int_turbidity = (int)turbidity;
    double turbidity_rem = turbidity - (double)int_turbidity;
    if (int_turbidity < 1) { int_turbidity = 1; turbidity_rem = 0.0; }
    if (int_turbidity > 9) { int_turbidity = 9; turbidity_rem = 1.0; }

    solar_elevation = pow(solar_elevation / (RAE_HOSEK_PI / 2.0), (1.0 / 3.0));

    const double s = solar_elevation;
    const double s1 = 1.0 - s;
    const double w[6] = {
        s1 * s1 * s1 * s1 * s1,
        5.0 * s1 * s1 * s1 * s1 * s,
        10.0 * s1 * s1 * s1 * s * s,
        10.0 * s1 * s1 * s * s * s,
        5.0 * s1 * s * s * s * s,
        s * s * s * s * s
    };

    const double* m[4];
    m[0] = dataset + (6 * (int_turbidity - 1));
    m[1] = dataset + (6 * int_turbidity);
    m[2] = dataset + (6 * 10) + (6 * (int_turbidity - 1));
    m[3] = dataset + (6 * 10) + (6 * int_turbidity);

    const double weight[4] = {
        (1.0 - albedo) * (1.0 - turbidity_rem),
        (1.0 - albedo) * turbidity_rem,
        albedo * (1.0 - turbidity_rem),
        albedo * turbidity_rem
    };

    double res = 0.0;
    for (int c = 0; c < 4; c++) {
        double bez = 0.0;
        for (int k = 0; k < 6; k++) bez += w[k] * m[c][k];
        res += weight[c] * bez;
    }
    return res;
}

/* One-entry memo. Rae reads the cooked state one scalar at a time (30 calls to
 * fill three channels), because a Rae extern taking a float array is more FFI
 * surface than this needs. Identical parameters therefore cost ONE cook, not
 * thirty. */
static struct {
    int    valid;
    double turbidity, albedo, elevation;
    double config[3][9];
    double radiance[3];
} g_memo = {0};

static void hosek_ensure(double turbidity, double albedo, double elevation) {
    if (g_memo.valid &&
        g_memo.turbidity == turbidity &&
        g_memo.albedo == albedo &&
        g_memo.elevation == elevation) {
        return;
    }
    for (int ch = 0; ch < 3; ch++) {
        hosek_cook_config(datasetsRGB[ch], g_memo.config[ch], turbidity, albedo, elevation);
        g_memo.radiance[ch] = hosek_cook_radiance(datasetsRGBRad[ch], turbidity, albedo, elevation);
    }
    g_memo.turbidity = turbidity;
    g_memo.albedo = albedo;
    g_memo.elevation = elevation;
    g_memo.valid = 1;
}

/* index 0..8: the nine analytic coefficients (A..I in the paper).
 * index 9:    that channel's radiance scale.
 * Out-of-range channel/index returns 0 rather than reading past the arrays —
 * this is called from Rae, where an off-by-one is a compile-clean mistake. */
double rae_ext_hosek_config(double turbidity, double albedo, double elevation,
                            int64_t channel, int64_t index) {
    if (channel < 0 || channel > 2 || index < 0 || index > 9) return 0.0;
    hosek_ensure(turbidity, albedo, elevation);
    if (index == 9) return g_memo.radiance[channel];
    return g_memo.config[channel][index];
}
