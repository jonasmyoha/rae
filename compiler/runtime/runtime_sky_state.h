/* Cooked Hosek-Wilkie coefficients — ONE table, read by BOTH renderers.
 *
 * The cook itself is Rae (lib/sky_hosek.rae, from the fitted dataset in
 * lib/data), and it is memoized there because it is far too heavy to redo per
 * frame. What lives here is only the landing pad: Rae pushes 36 scalars in,
 * a sky pass copies them into its uniform.
 *
 * SHARED ON PURPOSE. The forward and deferred paths exist to be compared side
 * by side (examples 111 and 112 are the same scene twice), and two copies of
 * this table would let them drift — one renderer a frame behind the other, or
 * fed by an app that remembered to push to one and not the other. Then the
 * comparison measures the bookkeeping instead of the renderers.
 *
 * Layout is the contract with the WGSL `hosek: array<vec4<f32>, 9>`: three
 * vec4 per channel, [A B C D][E F G H][I radiance _ _], R then G then B.
 */
#ifndef RAE_SKY_STATE_H
#define RAE_SKY_STATE_H

#include <stdint.h>

extern float rae_sky_hosek[36];

/* Out-of-range indices are dropped rather than trusted: this is an extern
 * boundary, and a bad index would otherwise scribble over whatever the
 * linker put next to the table. */
void rae_sky_hosek_push(int64_t index, float value);

#endif /* RAE_SKY_STATE_H */
