/* The links that opened this app (lib/AppLinks).
 *
 * Platform reason: the system hands a link to its own callback, which can run
 * before the program's main (a link that launched the app) and, on Android,
 * on another thread than the frame loop. So the callback's words wait here
 * until Rae asks. rae_app_links_push appends one line; rae_ext_AppLinks_take
 * returns what waits and empties it. Reading the lines is Rae's.
 *
 * rae_ext_AppLinks_open hands a link to the system (SDL_OpenURL: iOS
 * UIApplication open, Android an ACTION_VIEW intent, macOS NSWorkspace, the
 * desktop's default browser). Which links may go and what a headless run does
 * are Rae's. Platform reason: those system calls; a program without SDL3 (no
 * window) has none and answers 0. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_app_links_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_app_links[8192];
static size_t g_app_links_len = 0;

void rae_app_links_push(const char* url) {
    size_t n = strlen(url);
    pthread_mutex_lock(&g_app_links_lock);
    if (g_app_links_len + n + 1 < sizeof(g_app_links)) {
        memcpy(g_app_links + g_app_links_len, url, n);
        g_app_links_len += n;
        g_app_links[g_app_links_len++] = '\n';
        g_app_links[g_app_links_len] = 0;
    }
    pthread_mutex_unlock(&g_app_links_lock);
}

/* A test or the RAE_OPEN_URL stand-in, in the system's place */
void rae_ext_AppLinks_simulate(rae_String url) {
    if (url.data) rae_app_links_push((const char*)url.data);
}

rae_String rae_ext_AppLinks_take(void) {
    pthread_mutex_lock(&g_app_links_lock);
    rae_String taken = rae_ext_rae_str_from_cstr(g_app_links);
    g_app_links_len = 0;
    g_app_links[0] = 0;
    pthread_mutex_unlock(&g_app_links_lock);
    return taken;
}

/* Hand `url` to the system; 1 when it took it */
rae_Bool rae_ext_AppLinks_open(rae_String url) {
#ifdef RAE_HAS_SDL3
    char* text = (char*)malloc((size_t)url.len + 1);
    if (!text) return 0;
    if (url.len > 0) memcpy(text, url.data, (size_t)url.len);
    text[url.len] = 0;
    int opened = SDL_OpenURL(text) ? 1 : 0;
    free(text);
    return (rae_Bool)opened;
#else
    (void)url;
    return 0;
#endif
}
