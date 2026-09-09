// A tiny native mixer: four voice slots that must be acquired and released
// exactly once each. Stands in for any handle-based resource (a file, a GPU
// buffer, an audio voice) that the Rae compiler cannot copy or free by itself.
#include <stdint.h>
#include <stdio.h>

#define MIXER_SLOTS 4
static int mixer_taken[MIXER_SLOTS];
static int64_t mixer_live = 0;

int64_t rae_ext_mixerAcquire(void) {
    for (int i = 0; i < MIXER_SLOTS; i++) {
        if (!mixer_taken[i]) { mixer_taken[i] = 1; mixer_live++; return i; }
    }
    return -1;
}

void rae_ext_mixerRelease(int64_t slot) {
    if (slot < 0 || slot >= MIXER_SLOTS || !mixer_taken[slot]) {
        printf("[mixer] BAD release of slot %lld\n", (long long)slot);
        return;
    }
    mixer_taken[slot] = 0;
    mixer_live--;
}

int64_t rae_ext_mixerLive(void) { return mixer_live; }
