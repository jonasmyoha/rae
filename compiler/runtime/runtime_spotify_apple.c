/* macOS Spotify bridge and artwork-fetch integration. Platform bridge stays C; app/service policy should move out of the core runtime.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* ============================================================
 * Spotify (macOS desktop app) bridge — see lib/sys/spotify.rae
 *
 * Drives the local Spotify desktop app via `osascript` (AppleScript).
 * No credentials, no SDK, no HTTP auth — pure local automation. The
 * runtime needs the Spotify app open and the one-time macOS
 * "Automation" permission ("allow rae to control Spotify"); first
 * run triggers the system prompt.
 *
 * Layout: one `spotifyRefresh()` call per poll runs osascript and
 * fills a 6-field static cache (state + track + artist + album +
 * track id + artwork url). Per-field getters then return fresh owned
 * String copies so the rae side can build a `SpotifyTrack` struct
 * without struct-FFI gymnastics.
 *
 * Album art: `spotifyFetchArtwork(url, outPath)` shells out to curl.
 * iTunes Search API fallback for when Spotify hands back an empty
 * artwork URL (local files, podcasts) — `itunesSearchArtworkUrl`
 * upscales the 100x100 thumb to 600x600 the same way SUMU does.
 * ============================================================ */

#if defined(__APPLE__)
#ifndef __wasm__
#include <sys/wait.h>
#endif

static int rae_osascript_run(const char* const* lines) {
    int argc = 0;
    while (lines[argc]) argc++;
    char** argv = malloc(sizeof(char*) * (2 + 2 * (size_t)argc + 1));
    if (!argv) return -1;
    int a = 0;
    argv[a++] = (char*)"osascript";
    for (int i = 0; i < argc; i++) {
        argv[a++] = (char*)"-e";
        argv[a++] = (char*)lines[i];
    }
    argv[a] = NULL;
    pid_t pid = fork();
    if (pid < 0) { free(argv); return -1; }
    if (pid == 0) {
        execvp("/usr/bin/osascript", argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    free(argv);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static long rae_osascript_capture(const char* const* lines, char* out, size_t out_cap) {
    int fds[2];
    if (pipe(fds) < 0) return -1;
    int argc = 0;
    while (lines[argc]) argc++;
    char** argv = malloc(sizeof(char*) * (2 + 2 * (size_t)argc + 1));
    if (!argv) { close(fds[0]); close(fds[1]); return -1; }
    int a = 0;
    argv[a++] = (char*)"osascript";
    for (int i = 0; i < argc; i++) {
        argv[a++] = (char*)"-e";
        argv[a++] = (char*)lines[i];
    }
    argv[a] = NULL;
    pid_t pid = fork();
    if (pid < 0) { free(argv); close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        execvp("/usr/bin/osascript", argv);
        _exit(127);
    }
    close(fds[1]);
    size_t off = 0;
    while (off + 1 < out_cap) {
        ssize_t n = read(fds[0], out + off, out_cap - 1 - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    out[off] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    free(argv);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    while (off > 0 && (out[off - 1] == '\n' || out[off - 1] == '\r')) {
        out[--off] = '\0';
    }
    return (long)off;
}

typedef struct {
    char* state;
    char* trackId;
    char* trackName;
    char* artistName;
    char* albumName;
    char* artworkUrl;
    double positionSec;     /* player position in seconds, 0 when stopped */
    double durationSec;     /* track duration in seconds, 0 when unknown */
} RaeSpotifyCache;
static RaeSpotifyCache g_spotify_cache = {0};

/* Spotify poll thread (#279). `rae_ext_sys_spotify_refresh` shells out to
 * `osascript` (~tens of ms) — running it on the UI thread every second
 * spiked the frame rate. A background pthread now owns the refresh; the
 * cache is guarded by `g_spotify_mu` and the (cheap) getters read it under
 * the lock. osascript here is a fork/exec subprocess, so it is safe to run
 * off the main thread (no in-process Cocoa/AppleScript, no main-thread
 * requirement). */
static pthread_mutex_t g_spotify_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_spotify_poll_thread;
static int g_spotify_poll_started = 0;
static volatile int g_spotify_poll_stop = 0;
static int g_spotify_poll_interval_ms = 1000;

static void rae_spotify_cache_set_field(char** slot, const char* src, size_t len) {
    free(*slot);
    *slot = malloc(len + 1);
    if (!*slot) return;
    memcpy(*slot, src, len);
    (*slot)[len] = '\0';
}

static rae_String rae_cstr_to_owned_rae_string(const char* s) {
    if (!s) return (rae_String){NULL, 0, 0, 0};
    size_t n = strlen(s);
    uint8_t* buf = malloc(n + 1);
    if (!buf) return (rae_String){NULL, 0, 0, 0};
    memcpy(buf, s, n);
    buf[n] = '\0';
    rae_mem_str_tag(buf, (int64_t)n + 1, RAE_SITE_FROM_CSTR);
    return (rae_String){buf, (int64_t)n, (int64_t)n + 1, 1};
}

void rae_ext_sys_spotify_launch(void) {
    fprintf(stderr, "[spotify-c] launch\n");
    static const char* lines[] = {
        "tell application \"Spotify\" to if it is not running then launch",
        NULL
    };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] launch failed (osascript rc=%d)\n", rc);
}

void rae_ext_sys_spotify_play(void) {
    fprintf(stderr, "[spotify-c] play\n");
    static const char* lines[] = { "tell application \"Spotify\" to play", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] play failed (osascript rc=%d) — Spotify not running or Automation permission denied?\n", rc);
}

void rae_ext_sys_spotify_pause(void) {
    fprintf(stderr, "[spotify-c] pause\n");
    static const char* lines[] = { "tell application \"Spotify\" to pause", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] pause failed (osascript rc=%d)\n", rc);
}

void rae_ext_sys_spotify_next(void) {
    fprintf(stderr, "[spotify-c] next\n");
    static const char* lines[] = { "tell application \"Spotify\" to next track", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] next failed (osascript rc=%d)\n", rc);
}

void rae_ext_sys_spotify_previous(void) {
    fprintf(stderr, "[spotify-c] previous\n");
    static const char* lines[] = { "tell application \"Spotify\" to previous track", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] previous failed (osascript rc=%d)\n", rc);
}

/* Play a specific Spotify URI directly. Accepts spotify:track:<id>,
 * spotify:album:<id>, spotify:playlist:<id>, etc. Used when the local
 * album.json carries an explicit Spotify URI. */
void rae_ext_sys_spotify_playUri(rae_String uri) {
    if (!uri.data || uri.len == 0) return;
    fprintf(stderr, "[spotify-c] play uri=%.*s\n", (int)uri.len, (const char*)uri.data);
    char* uri_c = malloc((size_t)uri.len + 1);
    if (!uri_c) return;
    memcpy(uri_c, uri.data, (size_t)uri.len); uri_c[uri.len] = '\0';
    char script[1024];
    snprintf(script, sizeof(script), "tell application \"Spotify\" to play track \"%s\"", uri_c);
    free(uri_c);
    const char* lines[] = { script, NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] play uri failed (osascript rc=%d)\n", rc);
}

/* Search-and-play: feed the query string to Spotify's search URI scheme,
 * wait briefly for the search panel to populate, then issue `play`. This
 * is the AppleScript equivalent of the user typing into the search box
 * and pressing the play button — Spotify auto-plays the top hit when
 * the search loads with focus on it.
 *
 * The query is URL-encoded inline (alphanumerics + a few safe chars
 * pass through; everything else %xx). Quotes are escaped for the
 * AppleScript string literal. */
void rae_ext_sys_spotify_playQuery(rae_String query) {
    if (!query.data || query.len == 0) return;
    fprintf(stderr, "[spotify-c] play query=%.*s\n", (int)query.len, (const char*)query.data);
    /* URL-encode the query for the `spotify:search:` URI. A bare space
     * (or any reserved/unsafe char) makes Spotify's URI handler stop at
     * the first space: "Time to pretend MGMT" was being parsed as just
     * "Time". Percent-encode everything outside the RFC 3986 unreserved
     * set (A-Z a-z 0-9 - _ . ~) so the full multi-word query reaches
     * Spotify. The encoded output contains no `"` or `\`, so it is also
     * safe to drop straight into the AppleScript string literal below. */
    static const char rae_hexdig[] = "0123456789ABCDEF";
    char escaped[2048];
    size_t off = 0;
    for (size_t i = 0; i < (size_t)query.len && off + 4 < sizeof(escaped); i++) {
        unsigned char c = (unsigned char)query.data[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            escaped[off++] = (char)c;
        } else {
            escaped[off++] = '%';
            escaped[off++] = rae_hexdig[c >> 4];
            escaped[off++] = rae_hexdig[c & 0x0F];
        }
    }
    escaped[off] = '\0';
    /* One atomic AppleScript: launch Spotify if needed, then play the
     * search URI in a single `play track` call. `tell ... to play` /
     * `launch` (never `activate`) drive Spotify via background Apple
     * Events, like SUMU's play_spotify.
     *
     * Focus handling lives in C (`rae_ext_activateSelf` after the run),
     * NOT in this AppleScript. We deliberately do NOT capture-and-
     * restore "whatever was frontmost before the play" via System Events
     * `set frontmost`: that hands focus to the launching app (SUMU when
     * run from the dev tools), dropping our window out of the foreground
     * — macOS then demotes us to a background process (no Dock entry,
     * unfocusable, dead close button, launches behind SUMU). Re-asserting
     * our OWN app instead has no such failure mode. */
    char launch_line[128];
    char play_line[2160];
    snprintf(launch_line, sizeof(launch_line),
        "tell application \"Spotify\" to if it is not running then launch");
    snprintf(play_line, sizeof(play_line),
        "tell application \"Spotify\" to play track \"spotify:search:%s\"",
        escaped);
    const char* lines[] = {
        launch_line,
        play_line,
        NULL
    };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] play query failed (osascript rc=%d)\n", rc);
    /* `tell ... to play` is a background Apple Event, but `launch` (and
     * some Spotify configs) can pull Spotify to the front. Re-assert our
     * own window so the user keeps clicking the rae UI instead of having
     * focus stuck on Spotify. Activating SELF (not "the previous app")
     * avoids handing focus to the launcher (SUMU) — see rae_ext_activateSelf. */
    rae_ext_activateSelf();
}

void rae_ext_sys_spotify_refresh(void) {
    static const char* lines[] = {
        "tell application \"Spotify\"",
        "  set playerState to player state as text",
        "  if playerState is \"stopped\" then",
        "    return playerState & \"||\" & \"\" & \"||\" & \"\" & \"||\" & \"\" & \"||\" & \"\" & \"||\" & \"\" & \"||\" & \"0\" & \"||\" & \"0\"",
        "  end if",
        "  set trackId to \"\"",
        "  set trackName to \"\"",
        "  set artistName to \"\"",
        "  set albumName to \"\"",
        "  set artworkUrl to \"\"",
        "  set posSec to 0",
        "  set durMs to 0",
        "  try",
        "    set trackId to id of current track",
        "  end try",
        "  try",
        "    set trackName to name of current track",
        "  end try",
        "  try",
        "    set artistName to artist of current track",
        "  end try",
        "  try",
        "    set albumName to album of current track",
        "  end try",
        "  try",
        "    set artworkUrl to artwork url of current track",
        "  end try",
        "  try",
        "    set posSec to player position",
        "  end try",
        "  try",
        "    set durMs to duration of current track",
        "  end try",
        "  return playerState & \"||\" & trackId & \"||\" & trackName & \"||\" & artistName & \"||\" & albumName & \"||\" & artworkUrl & \"||\" & (posSec as text) & \"||\" & (durMs as text)",
        "end tell",
        NULL
    };
    char buf[4096];
    /* Slow osascript shell-out runs OUTSIDE the lock (poll thread) so it
     * never blocks a UI-thread getter; only the fast parse below holds it. */
    long n = rae_osascript_capture(lines, buf, sizeof(buf));
    pthread_mutex_lock(&g_spotify_mu);
    char** slots[] = {
        &g_spotify_cache.state,
        &g_spotify_cache.trackId,
        &g_spotify_cache.trackName,
        &g_spotify_cache.artistName,
        &g_spotify_cache.albumName,
        &g_spotify_cache.artworkUrl,
    };
    /* Reset everything before re-filling so a failure leaves a known-empty cache. */
    for (int i = 0; i < 6; i++) {
        rae_spotify_cache_set_field(slots[i], "", 0);
    }
    g_spotify_cache.positionSec = 0.0;
    g_spotify_cache.durationSec = 0.0;
    if (n >= 0) {
        char* p = buf;
        /* Parse 6 string fields. */
        for (int i = 0; i < 6; i++) {
            char* sep = strstr(p, "||");
            size_t len = sep ? (size_t)(sep - p) : strlen(p);
            rae_spotify_cache_set_field(slots[i], p, len);
            if (!sep) break;
            p = sep + 2;
        }
        /* Parse the trailing position + duration (numeric). The strstr walk
         * above leaves p pointing past the last "||" of the strings if every
         * separator was found. Re-walk from the buffer start to be safe. */
        char* q = buf;
        for (int i = 0; i < 6; i++) {
            char* sep = strstr(q, "||");
            if (!sep) { q = NULL; break; }
            q = sep + 2;
        }
        if (q) {
            char* sep = strstr(q, "||");
            if (sep) {
                *sep = '\0';
                g_spotify_cache.positionSec = atof(q);
                char* d = sep + 2;
                g_spotify_cache.durationSec = atof(d) / 1000.0;
            }
        }
    }
    pthread_mutex_unlock(&g_spotify_mu);
}

/* Poll thread body: refresh the cache on an interval until asked to stop.
 * Sleeps in small slices so stopPoller returns promptly. */
static void* rae_spotify_poll_thread_fn(void* arg) {
    (void)arg;
    while (!g_spotify_poll_stop) {
        rae_ext_sys_spotify_refresh();
        int slept = 0;
        while (slept < g_spotify_poll_interval_ms && !g_spotify_poll_stop) {
            usleep(50 * 1000);
            slept += 50;
        }
    }
    return NULL;
}

void rae_ext_sys_spotify_startPoller(int64_t interval_ms) {
    if (g_spotify_poll_started) return;
    g_spotify_poll_started = 1;
    g_spotify_poll_stop = 0;
    g_spotify_poll_interval_ms = (interval_ms > 0) ? (int)interval_ms : 1000;
    pthread_create(&g_spotify_poll_thread, NULL, rae_spotify_poll_thread_fn, NULL);
}

void rae_ext_sys_spotify_stopPoller(void) {
    if (!g_spotify_poll_started) return;
    g_spotify_poll_stop = 1;
    pthread_join(g_spotify_poll_thread, NULL);
    g_spotify_poll_started = 0;
}

/* Getters read the cache under the lock (the poll thread writes it). Each
 * field read is race-free; a rare cross-field tear during a track change at
 * the exact poll instant is cosmetic and self-corrects on the next frame. */
static rae_String rae_spotify_read_field(char* const* slot) {
    pthread_mutex_lock(&g_spotify_mu);
    rae_String s = rae_cstr_to_owned_rae_string(*slot);
    pthread_mutex_unlock(&g_spotify_mu);
    return s;
}
rae_String rae_ext_sys_spotify_state(void)       { return rae_spotify_read_field(&g_spotify_cache.state); }
rae_String rae_ext_sys_spotify_trackId(void)     { return rae_spotify_read_field(&g_spotify_cache.trackId); }
rae_String rae_ext_sys_spotify_trackName(void)   { return rae_spotify_read_field(&g_spotify_cache.trackName); }
rae_String rae_ext_sys_spotify_artistName(void)  { return rae_spotify_read_field(&g_spotify_cache.artistName); }
rae_String rae_ext_sys_spotify_albumName(void)   { return rae_spotify_read_field(&g_spotify_cache.albumName); }
rae_String rae_ext_sys_spotify_artworkUrl(void)  { return rae_spotify_read_field(&g_spotify_cache.artworkUrl); }
double rae_ext_sys_spotify_position(void)        { pthread_mutex_lock(&g_spotify_mu); double v = g_spotify_cache.positionSec; pthread_mutex_unlock(&g_spotify_mu); return v; }
double rae_ext_sys_spotify_duration(void)        { pthread_mutex_lock(&g_spotify_mu); double v = g_spotify_cache.durationSec; pthread_mutex_unlock(&g_spotify_mu); return v; }

/* Completeness check for a downloaded artwork file. Interrupted curl
 * writes leave truncated files (sizes are clean 4 KiB multiples), and
 * lenient decoders (ImageIO) render those half-image-half-grey with
 * NO error — the "In Electric Blue" glitch. A JPEG must carry its EOI
 * marker (FF D9) near the end (some encoders pad a few trailing
 * bytes, so scan the last 64). Non-JPEG payloads only need to be
 * non-empty — PNG never comes through this path today. */
static int rae_spotify_artwork_file_complete(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char head[3] = {0, 0, 0};
    if (fread(head, 1, 3, f) != 3) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 4) { fclose(f); return 0; }
    if (head[0] != 0xFF || head[1] != 0xD8 || head[2] != 0xFF) {
        fclose(f);
        return 1;
    }
    long tail_len = sz < 64 ? sz : 64;
    if (fseek(f, -tail_len, SEEK_END) != 0) { fclose(f); return 0; }
    unsigned char tail[64];
    if (fread(tail, 1, (size_t)tail_len, f) != (size_t)tail_len) { fclose(f); return 0; }
    fclose(f);
    for (long i = tail_len - 2; i >= 0; i--) {
        if (tail[i] == 0xFF && tail[i + 1] == 0xD9) return 1;
    }
    return 0;
}

/* Fetch `url_c` into `out_c` ATOMICALLY: curl writes to `<out>.part`,
 * the result is completeness-checked, and only then renamed into
 * place. An app quit mid-download (auto-exit headless runs included)
 * can no longer commit a half-file into the cache. */
static int rae_spotify_fetch_artwork_atomic(const char* url_c, const char* out_c) {
    if (!url_c || !out_c || !url_c[0] || !out_c[0]) return 0;
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.part", out_c) >= (int)sizeof(tmp)) return 0;
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        char* argv[] = { (char*)"curl", (char*)"-sLf", (char*)url_c, (char*)"-o", (char*)tmp, NULL };
        execvp("/usr/bin/curl", argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok) ok = rae_spotify_artwork_file_complete(tmp);
    if (!ok) {
        unlink(tmp);
        return 0;
    }
    if (rename(tmp, out_c) != 0) {
        unlink(tmp);
        return 0;
    }
    return 1;
}

rae_Bool rae_ext_sys_spotify_fetchArtwork(rae_String url, rae_String outPath) {
    if (!url.data || url.len == 0 || !outPath.data || outPath.len == 0) return false;
    char* url_c = malloc((size_t)url.len + 1);
    char* out_c = malloc((size_t)outPath.len + 1);
    if (!url_c || !out_c) { free(url_c); free(out_c); return false; }
    memcpy(url_c, url.data, (size_t)url.len); url_c[url.len] = '\0';
    memcpy(out_c, outPath.data, (size_t)outPath.len); out_c[outPath.len] = '\0';
    int ok = rae_spotify_fetch_artwork_atomic(url_c, out_c);
    free(url_c); free(out_c);
    return ok ? true : false;
}

typedef struct {
    char* url;
    char* out;
    int status; /* 0=pending, 1=success, 2=failed */
    pthread_t thread;
} RaeSpotifyArtworkJob;

#define RAE_SPOTIFY_ART_JOBS 64
static RaeSpotifyArtworkJob g_spotify_art_jobs[RAE_SPOTIFY_ART_JOBS];
static pthread_mutex_t g_spotify_art_mu = PTHREAD_MUTEX_INITIALIZER;

static char* rae_strndup_bytes(const uint8_t* data, int64_t len) {
    if (!data || len <= 0) return NULL;
    char* out = (char*)malloc((size_t)len + 1);
    if (!out) return NULL;
    memcpy(out, data, (size_t)len);
    out[len] = '\0';
    return out;
}

static void* rae_spotify_art_worker(void* arg) {
    RaeSpotifyArtworkJob* job = (RaeSpotifyArtworkJob*)arg;
    int ok = rae_spotify_fetch_artwork_atomic(job->url, job->out);
    pthread_mutex_lock(&g_spotify_art_mu);
    job->status = ok ? 1 : 2;
    pthread_mutex_unlock(&g_spotify_art_mu);
    return NULL;
}

rae_Bool rae_ext_sys_spotify_fetchArtworkAsync(rae_String url, rae_String outPath) {
    if (!url.data || url.len == 0 || !outPath.data || outPath.len == 0) return false;
    char* url_c = rae_strndup_bytes(url.data, url.len);
    char* out_c = rae_strndup_bytes(outPath.data, outPath.len);
    if (!url_c || !out_c) { free(url_c); free(out_c); return false; }

    pthread_mutex_lock(&g_spotify_art_mu);
    int free_idx = -1;
    for (int i = 0; i < RAE_SPOTIFY_ART_JOBS; i++) {
        if (g_spotify_art_jobs[i].out) {
            if (strcmp(g_spotify_art_jobs[i].out, out_c) == 0) {
                pthread_mutex_unlock(&g_spotify_art_mu);
                free(url_c);
                free(out_c);
                return true;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) {
        pthread_mutex_unlock(&g_spotify_art_mu);
        free(url_c);
        free(out_c);
        return false;
    }
    RaeSpotifyArtworkJob* job = &g_spotify_art_jobs[free_idx];
    job->url = url_c;
    job->out = out_c;
    job->status = 0;
    if (pthread_create(&job->thread, NULL, rae_spotify_art_worker, job) != 0) {
        job->url = NULL;
        job->out = NULL;
        job->status = 2;
        pthread_mutex_unlock(&g_spotify_art_mu);
        free(url_c);
        free(out_c);
        return false;
    }
    pthread_detach(job->thread);
    pthread_mutex_unlock(&g_spotify_art_mu);
    return true;
}

int64_t rae_ext_sys_spotify_fetchArtworkStatus(rae_String outPath) {
    if (!outPath.data || outPath.len == 0) return 0;
    char* out_c = rae_strndup_bytes(outPath.data, outPath.len);
    if (!out_c) return 0;
    int result = 0;
    pthread_mutex_lock(&g_spotify_art_mu);
    for (int i = 0; i < RAE_SPOTIFY_ART_JOBS; i++) {
        RaeSpotifyArtworkJob* job = &g_spotify_art_jobs[i];
        if (job->out && strcmp(job->out, out_c) == 0) {
            result = job->status;
            if (job->status != 0) {
                free(job->url);
                free(job->out);
                job->url = NULL;
                job->out = NULL;
                job->status = 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_spotify_art_mu);
    free(out_c);
    return result;
}

static void rae_url_encode_append(char* out, size_t* off, size_t cap, const char* s, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n && *off + 3 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == '.' || c == '~';
        if (safe) {
            out[(*off)++] = (char)c;
        } else {
            out[(*off)++] = '%';
            out[(*off)++] = hex[c >> 4];
            out[(*off)++] = hex[c & 0xF];
        }
    }
}

rae_String rae_ext_sys_spotify_itunesSearchArtworkUrl(rae_String term) {
    if (!term.data || term.len == 0) return (rae_String){NULL, 0, 0, 0};
    char url[1024];
    size_t off = 0;
    const char* prefix = "https://itunes.apple.com/search?term=";
    size_t plen = strlen(prefix);
    if (plen > sizeof(url)) return (rae_String){NULL, 0, 0, 0};
    memcpy(url, prefix, plen);
    off = plen;
    rae_url_encode_append(url, &off, sizeof(url), (const char*)term.data, (size_t)term.len);
    const char* suffix = "&entity=song&limit=1";
    size_t slen = strlen(suffix);
    if (off + slen + 1 > sizeof(url)) return (rae_String){NULL, 0, 0, 0};
    memcpy(url + off, suffix, slen);
    off += slen;
    url[off] = '\0';
    int fds[2];
    if (pipe(fds) < 0) return (rae_String){NULL, 0, 0, 0};
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return (rae_String){NULL, 0, 0, 0}; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        char* argv[] = { (char*)"curl", (char*)"-sLf", url, NULL };
        execvp("/usr/bin/curl", argv);
        _exit(127);
    }
    close(fds[1]);
    char body[32 * 1024];
    size_t bo = 0;
    while (bo + 1 < sizeof(body)) {
        ssize_t n = read(fds[0], body + bo, sizeof(body) - 1 - bo);
        if (n <= 0) break;
        bo += (size_t)n;
    }
    body[bo] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return (rae_String){NULL, 0, 0, 0};
    const char* key = "\"artworkUrl100\":\"";
    char* k = strstr(body, key);
    if (!k) return (rae_String){NULL, 0, 0, 0};
    k += strlen(key);
    char* end = strchr(k, '"');
    if (!end) return (rae_String){NULL, 0, 0, 0};
    size_t len = (size_t)(end - k);
    char* art = malloc(len + 1);
    if (!art) return (rae_String){NULL, 0, 0, 0};
    memcpy(art, k, len);
    art[len] = '\0';
    char* hit = strstr(art, "100x100");
    if (hit) memcpy(hit, "600x600", 7);
    rae_mem_str_tag((uint8_t*)art, (int64_t)len + 1, RAE_SITE_FROM_CSTR);
    return (rae_String){(uint8_t*)art, (int64_t)len, (int64_t)len + 1, 1};
}

#else  /* !__APPLE__ — Spotify bridge is macOS-only. Stubs return empty/false. */

void rae_ext_sys_spotify_launch(void)   {}
void rae_ext_sys_spotify_play(void)     {}
void rae_ext_sys_spotify_pause(void)    {}
void rae_ext_sys_spotify_next(void)     {}
void rae_ext_sys_spotify_previous(void) {}
void rae_ext_sys_spotify_refresh(void)  {}
void rae_ext_sys_spotify_startPoller(int64_t interval_ms) { (void)interval_ms; }
void rae_ext_sys_spotify_stopPoller(void) {}
rae_String rae_ext_sys_spotify_state(void)      { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_trackId(void)    { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_trackName(void)  { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_artistName(void) { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_albumName(void)  { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_artworkUrl(void) { return (rae_String){NULL, 0, 0, 0}; }
double rae_ext_sys_spotify_position(void)       { return 0.0; }
double rae_ext_sys_spotify_duration(void)       { return 0.0; }
void rae_ext_sys_spotify_playUri(rae_String uri) { (void)uri; }
void rae_ext_sys_spotify_playQuery(rae_String query) { (void)query; }
rae_Bool rae_ext_sys_spotify_fetchArtwork(rae_String url, rae_String outPath) { (void)url; (void)outPath; return false; }
rae_Bool rae_ext_sys_spotify_fetchArtworkAsync(rae_String url, rae_String outPath) { (void)url; (void)outPath; return false; }
int64_t rae_ext_sys_spotify_fetchArtworkStatus(rae_String outPath) { (void)outPath; return 2; }
rae_String rae_ext_sys_spotify_itunesSearchArtworkUrl(rae_String term) { (void)term; return (rae_String){NULL, 0, 0, 0}; }

#endif  /* __APPLE__ */
