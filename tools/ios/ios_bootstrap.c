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

__attribute__((constructor))
static void rae_ios_chdir_to_bundle(void) {
    const char *base = SDL_GetBasePath();   /* owned by SDL; do not free */
    if (base && *base) {
        (void)chdir(base);
    }
}
