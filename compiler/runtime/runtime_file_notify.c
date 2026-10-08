/* OS file-change notifications: the platform calls lib/FileNotify.rae is
 * built on (docs/runtime-c-audit.md row 10). Everything that is policy — the
 * watcher's table of paths and handles, watching each path together with its
 * directory, re-arming, what a notification means — is Rae. Each FileNotify
 * owns a kqueue (made with lib/net's poller shim); these functions each make
 * ONE call on it:
 *
 *   openForEvents   open(path, O_EVTONLY): a handle to watch, no I/O access
 *   watchHandle     one kevent() change adding an EVFILT_VNODE watch
 *   drain           one kevent() with a zero timeout: how many events were
 *                   pending (they are consumed: a notification is a hint)
 *   wakeOnChange    hand the kqueue to the waker (below)
 *   takePending     swap the waker's "something arrived" mark (an atomic)
 *
 * The waker is the one piece of glue that is not a single call. A UI app
 * blocks in SDL's event wait, which cannot wait on a kqueue, so a thread does:
 * one process-wide "outer" kqueue watches every FileNotify's kqueue for
 * readability (EV_CLEAR: once per new event, without consuming it), and on
 * each wake-up calls rae_ext_EventLoop_wake. It holds no table and decides
 * nothing; closing a FileNotify's kqueue removes it from the outer one. It
 * can go when an app's loop waits on the poller itself.
 *
 * Platform reasons: open/kevent, errno, the per-OS EVFILT_VNODE / NOTE_*
 * constants and the thread. kqueue only (macOS, BSD); elsewhere every call
 * answers "unsupported" and the watchers poll mtimes (Linux inotify comes
 * with the epoll poller, decision F8). Included by rae_runtime.c. */

void rae_ext_EventLoop_wake(void);   /* runtime_gpu2d_platform.c, or the no-op */

#define RAE_FILE_NOTIFY_UNSUPPORTED -1000000

#if (defined(__APPLE__) || defined(__FreeBSD__)) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
#include <sys/event.h>

int64_t rae_ext_FileNotify_supported(void) { return 1; }

int64_t rae_ext_FileNotify_openForEvents(rae_String path) {
  char name[4096];
  if (!path.data || path.len <= 0 || path.len >= (int64_t)sizeof name) return -ENAMETOOLONG;
  memcpy(name, path.data, (size_t)path.len);
  name[path.len] = '\0';
#ifdef O_EVTONLY
  int fd = open(name, O_EVTONLY | O_CLOEXEC);
#else
  int fd = open(name, O_RDONLY | O_CLOEXEC);
#endif
  return fd >= 0 ? fd : -errno;
}

/* Watch `fd` for writes, growth, attribute changes, deletion, rename and
 * revocation (EV_CLEAR: reported once per change) */
int64_t rae_ext_FileNotify_watchHandle(int64_t queue, int64_t fd) {
  struct kevent change;
  EV_SET(&change, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
         NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE, 0, NULL);
  return kevent((int)queue, &change, 1, NULL, 0, NULL) == 0 ? 0 : -errno;
}

int64_t rae_ext_FileNotify_drain(int64_t queue) {
  struct kevent pending[32];
  struct timespec now = { 0, 0 };
  int count = kevent((int)queue, NULL, 0, pending, 32, &now);
  return count >= 0 ? count : -errno;
}

static int g_fn_waker_queue = -1;
static pthread_once_t g_fn_waker_once = PTHREAD_ONCE_INIT;

/* "This queue got an event since it was last asked", by queue descriptor. The
 * waker sets it; rae_ext_FileNotify_takePending swaps it back. Asking the
 * kernel instead costs ~10 us per call (a kevent or poll on a queue of vnode
 * watches), and lib/FileNotify asks every frame. A reused descriptor number
 * can carry one stale mark: one spurious "look now", which a hint allows. */
#define RAE_FN_PENDING_MAX 65536
static _Atomic unsigned char g_fn_pending[RAE_FN_PENDING_MAX];

static void* rae_fn_waker(void* unused) {
  (void)unused;
  for (;;) {
    struct kevent ready[8];
    int count = kevent(g_fn_waker_queue, NULL, 0, ready, 8, NULL);
    if (count > 0) {
      for (int i = 0; i < count; i++) {
        if (ready[i].ident < RAE_FN_PENDING_MAX)
          atomic_store_explicit(&g_fn_pending[ready[i].ident], 1, memory_order_release);
      }
      rae_ext_EventLoop_wake();
    } else if (count < 0 && errno != EINTR) {
      return NULL;
    }
  }
}

/* Whether `queue` got an event since the last call (1 / 0), clearing the
 * mark; -1 when the descriptor is beyond the marks (ask the kernel) */
int64_t rae_ext_FileNotify_takePending(int64_t queue) {
  if (queue < 0 || queue >= RAE_FN_PENDING_MAX) return -1;
  return atomic_exchange_explicit(&g_fn_pending[queue], 0, memory_order_acq_rel);
}

static void rae_fn_start_waker(void) {
  int queue = kqueue();
  if (queue < 0) return;
  pthread_t thread;
  g_fn_waker_queue = queue;
  if (pthread_create(&thread, NULL, rae_fn_waker, NULL) != 0) {
    close(queue);
    g_fn_waker_queue = -1;
    return;
  }
  pthread_detach(thread);
}

/* Wake the app's event loop whenever `queue` gets an event: 0, or -errno */
int64_t rae_ext_FileNotify_wakeOnChange(int64_t queue) {
  pthread_once(&g_fn_waker_once, rae_fn_start_waker);
  if (g_fn_waker_queue < 0) return -ENOSYS;
  if (queue >= 0 && queue < RAE_FN_PENDING_MAX)
    atomic_store_explicit(&g_fn_pending[queue], 0, memory_order_release);
  struct kevent change;
  EV_SET(&change, (uintptr_t)queue, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
  return kevent(g_fn_waker_queue, &change, 1, NULL, 0, NULL) == 0 ? 0 : -errno;
}

#else

int64_t rae_ext_FileNotify_supported(void) { return 0; }
int64_t rae_ext_FileNotify_openForEvents(rae_String path) { (void)path; return RAE_FILE_NOTIFY_UNSUPPORTED; }
int64_t rae_ext_FileNotify_watchHandle(int64_t queue, int64_t fd) { (void)queue; (void)fd; return RAE_FILE_NOTIFY_UNSUPPORTED; }
int64_t rae_ext_FileNotify_drain(int64_t queue) { (void)queue; return RAE_FILE_NOTIFY_UNSUPPORTED; }
int64_t rae_ext_FileNotify_wakeOnChange(int64_t queue) { (void)queue; return RAE_FILE_NOTIFY_UNSUPPORTED; }
int64_t rae_ext_FileNotify_takePending(int64_t queue) { (void)queue; return 0; }

#endif
