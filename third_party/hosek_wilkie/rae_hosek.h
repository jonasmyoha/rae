/* See rae_hosek.c for provenance and licence. */
#ifndef RAE_HOSEK_H
#define RAE_HOSEK_H

#include <stdint.h>

#define RAE_HOSEK_PI 3.141592653589793

/* Cooked Hosek-Wilkie state, read one scalar at a time.
 *   channel: 0=R 1=G 2=B
 *   index:   0..8 = analytic coefficients, 9 = radiance scale
 * `elevation` is the solar elevation in radians, measured from the horizon. */
double rae_ext_hosek_config(double turbidity, double albedo, double elevation,
                            int64_t channel, int64_t index);

#endif
