/* iOS launch shim (#520/#522).
 *
 * Rae examples read assets by paths relative to the working directory (e.g.
 * "assets/Roboto-Regular.mtsdf.json"). iOS has no writable CWD at the repo root;
 * resources live in the .app bundle. SDL_GetBasePath() on iOS returns the bundle
 * resource directory, so chdir there before the app's main runs and every
 * existing relative asset read resolves — with zero Objective-C. The constructor
 * runs before main; SDL_GetBasePath needs no SDL_Init on iOS (it queries the
 * bundle). */
#include "../device_log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <SDL3/SDL.h>
#include <mach-o/ldsyms.h>
#include <mach-o/loader.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

/* A text of Info.plist, "unknown" without one */
static const char *plist_text(const char *key, char *out, size_t size) {
    snprintf(out, size, "unknown");
    CFBundleRef bundle = CFBundleGetMainBundle();
    CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef value = bundle && name ? CFBundleGetValueForInfoDictionaryKey(bundle, name) : NULL;
    if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)value, out, (CFIndex)size, kCFStringEncodingUTF8);
    }
    if (name) CFRelease(name);
    return out;
}

static const char *system_text(const char *name, char *out, size_t size) {
    size_t length = size;
    if (sysctlbyname(name, out, &length, NULL, 0) != 0) snprintf(out, size, "unknown");
    return out;
}

/* The binary's UUID, as the system's crash and GPU reports name it (and
 * `dwarfdump --uuid` on the built app) */
static const char *binary_uuid(char out[64]) {
    snprintf(out, 64, "unknown");
    const struct load_command *command = (const struct load_command *)(&_mh_execute_header + 1);
    for (uint32_t i = 0; i < _mh_execute_header.ncmds; i++) {
        if (command->cmd == LC_UUID) {
            const uint8_t *id = ((const struct uuid_command *)command)->uuid;
            char *at = out;
            for (int byte = 0; byte < 16; byte++) {
                if (byte == 4 || byte == 6 || byte == 8 || byte == 10) *at++ = '-';
                at += snprintf(at, 3, "%02X", id[byte]);
            }
            break;
        }
        command = (const struct load_command *)((const char *)command + command->cmdsize);
    }
    return out;
}

/* What runs, for the log's first lines */
static void log_identity(void) {
    char name[128], version[64], build[64], bundle[128], rae[96], commit[96], uuid[64];
    char model[64], system[64], system_build[64], lines[1024];
    uint64_t memory = 0;
    size_t length = sizeof(memory);
    sysctlbyname("hw.memsize", &memory, &length, NULL, 0);
    const char *simulated = getenv("SIMULATOR_MODEL_IDENTIFIER");
    snprintf(lines, sizeof(lines),
             "[run] app %s %s (%s), %s, commit %s\n"
             "[run] binary UUID %s\n"
             "[run] Rae %s\n"
             "[run] device %s%s, iOS %s (%s), %llu MB of memory, process %d\n",
             plist_text("CFBundleDisplayName", name, sizeof(name)),
             plist_text("CFBundleShortVersionString", version, sizeof(version)),
             plist_text("CFBundleVersion", build, sizeof(build)),
             plist_text("CFBundleIdentifier", bundle, sizeof(bundle)),
             plist_text("RaeAppCommit", commit, sizeof(commit)), binary_uuid(uuid),
             plist_text("RaeVersion", rae, sizeof(rae)),
             simulated ? simulated : system_text("hw.machine", model, sizeof(model)),
             simulated ? " (Simulator)" : "",
             system_text("kern.osproductversion", system, sizeof(system)),
             system_text("kern.osversion", system_build, sizeof(system_build)),
             (unsigned long long)(memory / (1024 * 1024)), (int)getpid());
    device_log_identity(lines);
}

__attribute__((constructor))
static void rae_ios_bootstrap(void) {
    const char *base = SDL_GetBasePath();   /* owned by SDL; do not free */
    if (base && *base) {
        (void)chdir(base);
    }
    /* iOS apps have no visible stdout: stdout and stderr (wgpu's errors and a
     * panic's message among them) go to a file in the app's writable Documents
     * folder, each line with its time (tools/device_log.c), pulled with
     * `xcrun devicectl device copy from`. The last runs' logs stay beside it. */
    const char *home = getenv("HOME");
    if (home && *home) {
        char folder[900];
        snprintf(folder, sizeof(folder), "%s/Documents", home);
        if (device_log_start(folder, NULL)) log_identity();
        /* Frame profiler (#527): the cwd is the read-only bundle, so a relative
         * RAE_PROFILE_OUT would fail to write. When a capture is requested
         * (RAE_PROFILE set, e.g. by run-ios.sh --profile) and no explicit output
         * path was given, default it to the writable Documents dir so the trace
         * lands next to rae.log and is pulled the same way. */
        /* A headless screenshot (RAE_GPU2D_SCREENSHOT) given as a bare file
         * name lands in Documents too, pullable like rae.log: the only way to
         * see what an iPhone run drew is to look at that image. */
        const char *shot = getenv("RAE_GPU2D_SCREENSHOT");
        if (shot && *shot && shot[0] != '/') {
            char shotPath[1024];
            snprintf(shotPath, sizeof(shotPath), "%s/Documents/%s", home, shot);
            setenv("RAE_GPU2D_SCREENSHOT", shotPath, 1);
        }
        if (getenv("RAE_PROFILE") && !getenv("RAE_PROFILE_OUT")) {
            char out[1024];
            snprintf(out, sizeof(out), "%s/Documents/rae_profile.json", home);
            setenv("RAE_PROFILE_OUT", out, 1);
        }
        /* The capture file (RAE_PERF_OUT, lib/perf/PerfFile) given as a bare file name lands
         * in Documents as well, pulled like the trace */
        const char *perfOut = getenv("RAE_PERF_OUT");
        if (perfOut && *perfOut && perfOut[0] != '/') {
            char perfPath[1024];
            snprintf(perfPath, sizeof(perfPath), "%s/Documents/%s", home, perfOut);
            setenv("RAE_PERF_OUT", perfPath, 1);
        }
    }
}
