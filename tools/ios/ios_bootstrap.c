/* iOS launch shim (#520/#522).
 *
 * Rae examples read assets by paths relative to the working directory (e.g.
 * "assets/Roboto-Regular.mtsdf.json"). iOS has no writable CWD at the repo root;
 * resources live in the .app bundle. SDL_GetBasePath() on iOS returns the bundle
 * resource directory, so chdir there before the app's main runs and every
 * existing relative asset read resolves — with zero Objective-C. The constructor
 * runs before main; SDL_GetBasePath needs no SDL_Init on iOS (it queries the
 * bundle). */
#include <SDL3/SDL.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

__attribute__((constructor))
static void rae_ios_bootstrap(void) {
    const char *base = SDL_GetBasePath();   /* owned by SDL; do not free */
    if (base && *base) {
        (void)chdir(base);
    }
    /* Dev harness logging: iOS apps have no visible stdout, so mirror stdout +
     * stderr (incl. wgpu errors) to a file in the app's writable Documents dir,
     * pullable with `xcrun devicectl device copy from`. */
    const char *home = getenv("HOME");
    if (home && *home) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/Documents/rae.log", home);
        if (freopen(path, "w", stdout)) setvbuf(stdout, NULL, _IOLBF, 0);
        (void)freopen(path, "a", stderr);
        /* Frame profiler (#527): the cwd is the read-only bundle, so a relative
         * RAE_PROFILE_OUT would fail to write. When a capture is requested
         * (RAE_PROFILE set, e.g. by run-ios.sh --profile) and no explicit output
         * path was given, default it to the writable Documents dir so the trace
         * lands next to rae.log and is pulled the same way. */
        if (getenv("RAE_PROFILE") && !getenv("RAE_PROFILE_OUT")) {
            char out[1024];
            snprintf(out, sizeof(out), "%s/Documents/rae_profile.json", home);
            setenv("RAE_PROFILE_OUT", out, 1);
        }
    }
}
