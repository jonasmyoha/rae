/* OS file-change notifications (lib/FileNotify.rae).
 *
 * The file watchers used to be timers: an app stat'ed its watched files every
 * 50-250 ms, so an idle UI app woke 4-20 times a second only to find nothing
 * changed. Here the OS tells us instead (kqueue EVFILT_VNODE on macOS/BSD,
 * inotify on Linux): one background thread blocks on the notification queue,
 * and when a watched file changes it marks the watchers that care and calls
 * rae_ext_EventLoop_wake, so a loop blocked in its event wait runs at once.
 *
 * A notification is a hint, not a diff: the caller still compares mtimes to
 * decide whether anything really changed. That keeps the design forgiving.
 * Each path is watched together with its parent DIRECTORY, because editors
 * save by writing a temp file and renaming it over the original: the old
 * inode's watch then fires once (delete/rename) and goes stale, but the rename
 * is also a change to the directory. A directory watch also fires for
 * unrelated files beside the watched one; that costs one cheap mtime check.
 * After a detected change the caller re-arms (clear + watch again) so the new
 * inode is watched too.
 *
 * Handles are small ints (one per Rae FileNotify). open returns -1 where
 * notifications are unavailable (Windows, WASM, no free slot, the OS refused),
 * and the caller falls back to a slow mtime poll.
 *
 * This module is included by rae_runtime.c into one translation unit. */

void rae_ext_EventLoop_wake(void);   /* runtime_gpu2d_platform.c, or the no-op */

#if (defined(__APPLE__) || defined(__FreeBSD__) || defined(__linux__)) && !defined(__wasm__) \
    && !defined(__EMSCRIPTEN__)
#define RAE_FILE_NOTIFY 1
#endif

#ifdef RAE_FILE_NOTIFY
#if defined(__linux__)
#include <sys/inotify.h>
#else
#include <sys/event.h>
#endif

#define RAE_FILE_NOTIFY_MAX 64

/* One registration: a watcher watches a path (file or directory). kqueue
 * needs an open descriptor per registration; inotify has one watch
 * descriptor per PATH, shared by every watcher of that path. */
typedef struct {
    int watcher;
    int fd;        /* kqueue: the open file; inotify: the watch descriptor */
    char* path;
} RaeFileNotifyEntry;

static pthread_mutex_t g_fn_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_fn_queue = -1;            /* the kqueue / inotify descriptor */
static int g_fn_thread_started = 0;
static unsigned char g_fn_used[RAE_FILE_NOTIFY_MAX];
static unsigned char g_fn_changed[RAE_FILE_NOTIFY_MAX];
static RaeFileNotifyEntry* g_fn_entries = NULL;
static int g_fn_entry_count = 0, g_fn_entry_cap = 0;

#if defined(__linux__)
/* Mark every watcher of the registration `fd` changed (called under the lock).
 * Only inotify needs it: kqueue passes the watcher in the event itself, while
 * inotify only knows the watch descriptor, which several watchers may share. */
static void rae_fn_mark_fd(int fd) {
    for (int i = 0; i < g_fn_entry_count; i++) {
        if (g_fn_entries[i].fd == fd) g_fn_changed[g_fn_entries[i].watcher] = 1;
    }
}
#endif

static void* rae_fn_thread(void* arg) {
    (void)arg;
    for (;;) {
        int marked = 0;
#if defined(__linux__)
        char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
        ssize_t n = read(g_fn_queue, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return NULL;
        }
        pthread_mutex_lock(&g_fn_lock);
        for (char* p = buf; p < buf + n;) {
            struct inotify_event* ev = (struct inotify_event*)p;
            rae_fn_mark_fd(ev->wd);
            marked = 1;
            p += sizeof(struct inotify_event) + ev->len;
        }
        pthread_mutex_unlock(&g_fn_lock);
#else
        struct kevent evs[16];
        int n = kevent(g_fn_queue, NULL, 0, evs, 16, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        pthread_mutex_lock(&g_fn_lock);
        for (int i = 0; i < n; i++) {
            int watcher = (int)(intptr_t)evs[i].udata;
            if (watcher >= 0 && watcher < RAE_FILE_NOTIFY_MAX && g_fn_used[watcher]) {
                g_fn_changed[watcher] = 1;
                marked = 1;
            }
        }
        pthread_mutex_unlock(&g_fn_lock);
#endif
        if (marked) rae_ext_EventLoop_wake();
    }
}

/* The queue and its thread, created on first use (under the lock). */
static int rae_fn_ensure_started(void) {
    if (g_fn_thread_started) return 1;
#if defined(__linux__)
    g_fn_queue = inotify_init1(IN_CLOEXEC);
#else
    g_fn_queue = kqueue();
#endif
    if (g_fn_queue < 0) return 0;
    pthread_t thread;
    if (pthread_create(&thread, NULL, rae_fn_thread, NULL) != 0) {
        close(g_fn_queue);
        g_fn_queue = -1;
        return 0;
    }
    pthread_detach(thread);
    g_fn_thread_started = 1;
    return 1;
}

/* Register `path` for `watcher` unless it already is (under the lock).
 * Returns 1 on success. */
static int rae_fn_add_path(int watcher, const char* path, int is_dir) {
    for (int i = 0; i < g_fn_entry_count; i++) {
        if (g_fn_entries[i].watcher == watcher && strcmp(g_fn_entries[i].path, path) == 0) return 1;
    }
#if defined(__linux__)
    uint32_t mask = is_dir
        ? (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_MODIFY)
        : (IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
    int fd = inotify_add_watch(g_fn_queue, path, mask | IN_MASK_ADD);
    if (fd < 0) return 0;
#else
    (void)is_dir;
#ifdef O_EVTONLY
    int fd = open(path, O_EVTONLY | O_CLOEXEC);
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0) return 0;
    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE,
           0, (void*)(intptr_t)watcher);
    if (kevent(g_fn_queue, &change, 1, NULL, 0, NULL) < 0) {
        close(fd);
        return 0;
    }
#endif
    if (g_fn_entry_count == g_fn_entry_cap) {
        int cap = g_fn_entry_cap ? g_fn_entry_cap * 2 : 16;
        RaeFileNotifyEntry* grown = (RaeFileNotifyEntry*)realloc(g_fn_entries, (size_t)cap * sizeof(*grown));
        if (!grown) {
#if !defined(__linux__)
            close(fd);
#endif
            return 0;
        }
        g_fn_entries = grown;
        g_fn_entry_cap = cap;
    }
    g_fn_entries[g_fn_entry_count].watcher = watcher;
    g_fn_entries[g_fn_entry_count].fd = fd;
    g_fn_entries[g_fn_entry_count].path = strdup(path);
    g_fn_entry_count++;
    return 1;
}

/* The directory part of `path`, in place ("." when there is none). */
static const char* rae_fn_parent_dir(char* path) {
    char* slash = strrchr(path, '/');
    if (!slash) return ".";
    if (slash == path) return "/";
    *slash = '\0';
    return path;
}

/* Drop every registration of `watcher` (under the lock). */
static void rae_fn_clear(int watcher) {
    int kept = 0;
    for (int i = 0; i < g_fn_entry_count; i++) {
        RaeFileNotifyEntry entry = g_fn_entries[i];
        if (entry.watcher != watcher) {
            g_fn_entries[kept++] = entry;
            continue;
        }
#if defined(__linux__)
        /* The watch descriptor is per path: remove it only when no other
         * watcher still uses it. */
        int shared = 0;
        for (int j = 0; j < g_fn_entry_count; j++) {
            if (j != i && g_fn_entries[j].fd == entry.fd && g_fn_entries[j].watcher != watcher) shared = 1;
        }
        if (!shared) inotify_rm_watch(g_fn_queue, entry.fd);
#else
        close(entry.fd);   /* closing the descriptor removes its kevent */
#endif
        free(entry.path);
    }
    g_fn_entry_count = kept;
}

int64_t rae_ext_FileNotify_open(void) {
    if (getenv("RAE_FILE_NOTIFY") && strcmp(getenv("RAE_FILE_NOTIFY"), "off") == 0) return -1;
    pthread_mutex_lock(&g_fn_lock);
    int64_t id = -1;
    if (rae_fn_ensure_started()) {
        for (int i = 0; i < RAE_FILE_NOTIFY_MAX; i++) {
            if (!g_fn_used[i]) {
                g_fn_used[i] = 1;
                g_fn_changed[i] = 0;
                id = i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_fn_lock);
    return id;
}

rae_Bool rae_ext_FileNotify_watch(int64_t id, rae_String path) {
    if (id < 0 || id >= RAE_FILE_NOTIFY_MAX || !path.data || path.len <= 0) return 0;
    char* file = strndup((const char*)path.data, (size_t)path.len);
    char* copy = strdup(file);
    if (!file || !copy) {
        free(file);
        free(copy);
        return 0;
    }
    pthread_mutex_lock(&g_fn_lock);
    int ok = 0;
    if (g_fn_used[id]) {
        /* The directory first: it is what still sees the file after a rename
         * over it, or its creation when it does not exist yet. */
        ok |= rae_fn_add_path((int)id, rae_fn_parent_dir(copy), 1);
        ok |= rae_fn_add_path((int)id, file, 0);
    }
    pthread_mutex_unlock(&g_fn_lock);
    free(file);
    free(copy);
    return ok ? 1 : 0;
}

void rae_ext_FileNotify_clear(int64_t id) {
    if (id < 0 || id >= RAE_FILE_NOTIFY_MAX) return;
    pthread_mutex_lock(&g_fn_lock);
    rae_fn_clear((int)id);
    g_fn_changed[id] = 0;
    pthread_mutex_unlock(&g_fn_lock);
}

rae_Bool rae_ext_FileNotify_takeChanged(int64_t id) {
    if (id < 0 || id >= RAE_FILE_NOTIFY_MAX) return 0;
    pthread_mutex_lock(&g_fn_lock);
    rae_Bool changed = g_fn_changed[id] ? 1 : 0;
    g_fn_changed[id] = 0;
    pthread_mutex_unlock(&g_fn_lock);
    return changed;
}

void rae_ext_FileNotify_close(int64_t id) {
    if (id < 0 || id >= RAE_FILE_NOTIFY_MAX) return;
    pthread_mutex_lock(&g_fn_lock);
    rae_fn_clear((int)id);
    g_fn_used[id] = 0;
    g_fn_changed[id] = 0;
    pthread_mutex_unlock(&g_fn_lock);
}

#else /* no OS notifications: every watcher falls back to its mtime poll */

int64_t rae_ext_FileNotify_open(void) { return -1; }
rae_Bool rae_ext_FileNotify_watch(int64_t id, rae_String path) { (void)id; (void)path; return 0; }
void rae_ext_FileNotify_clear(int64_t id) { (void)id; }
rae_Bool rae_ext_FileNotify_takeChanged(int64_t id) { (void)id; return 0; }
void rae_ext_FileNotify_close(int64_t id) { (void)id; }

#endif
