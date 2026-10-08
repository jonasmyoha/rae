/* macOS Spotify bridge: running the two programs lib/sys/Spotify.rae drives,
 * `osascript` (AppleScript, to control the Spotify desktop app) and `curl`
 * (artwork and the iTunes search). Platform reason: fork/exec/waitpid and
 * reading a pipe. Everything else — the AppleScript text, URL encoding,
 * parsing osascript's "||"-separated answer, the iTunes JSON, the artwork
 * completeness check and the atomic rename — is Rae in lib/sys/Spotify.rae
 * (docs/runtime-c-audit.md row 9). There is no cache here any more: the app's
 * poll worker sends each parsed answer to the UI thread on a Channel.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 */

#if defined(__APPLE__)
#ifndef __wasm__
#include <sys/wait.h>
#endif

/* A String as a NUL-terminated C copy (the caller frees it) */
static char* rae_spotify_cstr(rae_String text) {
    char* copy = malloc((size_t)text.len + 1);
    if (!copy) return NULL;
    if (text.len > 0) memcpy(copy, text.data, (size_t)text.len);
    copy[text.len] = '\0';
    return copy;
}

/* Run `program` with `argv` and wait for it. With `output` set, its stdout is
 * read to the end into a growing buffer. Answers the exit status, -1 when it
 * could not be started or did not exit normally. */
static int rae_spotify_run(const char* program, char* const* argv, char** output, size_t* output_len) {
    int fds[2] = {-1, -1};
    if (output && pipe(fds) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        if (output) { close(fds[0]); close(fds[1]); }
        return -1;
    }
    if (pid == 0) {
        if (output) { close(fds[0]); dup2(fds[1], 1); close(fds[1]); }
        execvp(program, argv);
        _exit(127);
    }
    if (output) {
        close(fds[1]);
        size_t cap = 4096, len = 0;
        char* buffer = malloc(cap);
        while (buffer) {
            if (len + 1 >= cap) {
                char* grown = realloc(buffer, cap * 2);
                if (!grown) break;
                buffer = grown; cap *= 2;
            }
            ssize_t n = read(fds[0], buffer + len, cap - 1 - len);
            if (n <= 0) break;
            len += (size_t)n;
        }
        close(fds[0]);
        *output = buffer; *output_len = buffer ? len : 0;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Captured bytes as an owned String when the program exited 0, else empty */
static rae_String rae_spotify_output_string(int status, char* output, size_t len) {
    rae_String text = (rae_String){NULL, 0, 0, 0};
    if (status == 0 && output && len > 0) text = rae_ext_rae_str_from_bytes((uint8_t*)output, 0, (int64_t)len);
    free(output);
    return text;
}

/* `osascript -e <script>` (a script may span lines): its exit status */
int64_t rae_ext_sys_Spotify_osascriptRun(rae_String script) {
    char* text = rae_spotify_cstr(script);
    if (!text) return -1;
    char* argv[] = { (char*)"osascript", (char*)"-e", text, NULL };
    int status = rae_spotify_run("/usr/bin/osascript", argv, NULL, NULL);
    free(text);
    return status;
}

/* `osascript -e <script>`: what it printed, empty unless it exited 0 */
rae_String rae_ext_sys_Spotify_osascriptOutput(rae_String script) {
    char* text = rae_spotify_cstr(script);
    if (!text) return (rae_String){NULL, 0, 0, 0};
    char* argv[] = { (char*)"osascript", (char*)"-e", text, NULL };
    char* output = NULL; size_t len = 0;
    int status = rae_spotify_run("/usr/bin/osascript", argv, &output, &len);
    free(text);
    return rae_spotify_output_string(status, output, len);
}

/* `curl -sLf <url>`: the body, empty unless curl exited 0 */
rae_String rae_ext_sys_Spotify_curlOutput(rae_String url) {
    char* link = rae_spotify_cstr(url);
    if (!link) return (rae_String){NULL, 0, 0, 0};
    char* argv[] = { (char*)"curl", (char*)"-sLf", link, NULL };
    char* output = NULL; size_t len = 0;
    int status = rae_spotify_run("/usr/bin/curl", argv, &output, &len);
    free(link);
    return rae_spotify_output_string(status, output, len);
}

/* `curl -sLf <url> -o <path>`: whether curl exited 0 */
rae_Bool rae_ext_sys_Spotify_curlToFile(rae_String url, rae_String path) {
    char* link = rae_spotify_cstr(url);
    char* file = rae_spotify_cstr(path);
    int status = -1;
    if (link && file) {
        char* argv[] = { (char*)"curl", (char*)"-sLf", link, (char*)"-o", file, NULL };
        status = rae_spotify_run("/usr/bin/curl", argv, NULL, NULL);
    }
    free(link); free(file);
    return status == 0;
}

#else  /* !__APPLE__ — the bridge is macOS-only: nothing runs, every answer is empty. */

int64_t rae_ext_sys_Spotify_osascriptRun(rae_String script) { (void)script; return -1; }
rae_String rae_ext_sys_Spotify_osascriptOutput(rae_String script) { (void)script; return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_Spotify_curlOutput(rae_String url) { (void)url; return (rae_String){NULL, 0, 0, 0}; }
rae_Bool rae_ext_sys_Spotify_curlToFile(rae_String url, rae_String path) { (void)url; (void)path; return false; }

#endif  /* __APPLE__ */
