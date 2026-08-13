/* The shared cooked-sky table. See runtime_sky_state.h for why it is shared. */
#include "runtime_sky_state.h"

/* Zero-initialised, and zero is a safe reading: every Hosek coefficient at
 * zero makes the model return black, which is what an app that never pushed a
 * cook should see. It must not be a hidden default sky — a black horizon is a
 * bug report, an invented blue one is a mystery. */
float rae_sky_hosek[36];

void rae_sky_hosek_push(int64_t index, float value) {
    if (index < 0 || index >= 36) return;
    rae_sky_hosek[(int)index] = value;
}
