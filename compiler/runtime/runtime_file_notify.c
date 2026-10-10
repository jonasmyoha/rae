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
 * On a Linux kernel (RAE_HAS_INOTIFY, the C twin of `hasInotify`) a FileNotify
 * owns an inotify descriptor instead, with the same one-call shape:
 *
 *   inotifyCreate   inotify_init1(IN_NONBLOCK | IN_CLOEXEC)
 *   inotifyWatch    one inotify_add_watch: a watch descriptor for the path
 *   inotifyForget   one inotify_rm_watch
 *   drain           one non-blocking read: the bytes of pending events (they
 *                   are consumed; a notification is a hint)
 *   wakeOnChange    the waker watches the descriptor with epoll (EPOLLET: once
 *                   per new event, without consuming it)
 *   takePending     the same mark as on kqueue
 *
 * Platform reasons: open/kevent/inotify/epoll, errno, the per-OS EVFILT_VNODE
 * / NOTE_* / IN_* constants and the thread. lib/FileNotify.rae declares these
 * shims only where they exist, so no "unsupported" stubs exist elsewhere;
 * there the watchers poll mtimes. Included by rae_runtime.c. */

void rae_ext_EventLoop_wake(void);   /* runtime_gpu2d_platform.c, or the no-op */

#if defined(RAE_HAS_KQUEUE) || defined(RAE_HAS_INOTIFY)
static int g_fn_waker_queue = -1;
static pthread_once_t g_fn_waker_once = PTHREAD_ONCE_INIT;

/* "This queue got an event since it was last asked", by queue descriptor. The
 * waker sets it; rae_ext_FileNotify_takePending swaps it back. Asking the
 * kernel instead costs ~10 us per call (a kevent or poll on a queue of vnode
 * watches), and lib/FileNotify asks every frame. A reused descriptor number
 * can carry one stale mark: one spurious "look now", which a hint allows. */
#define RAE_FN_PENDING_MAX 65536
static _Atomic unsigned char g_fn_pending[RAE_FN_PENDING_MAX];

/* Whether `queue` got an event since the last call (1 / 0), clearing the
 * mark; -1 when the descriptor is beyond the marks (ask the kernel) */
int64_t rae_ext_FileNotify_takePending(int64_t queue) {
  if (queue < 0 || queue >= RAE_FN_PENDING_MAX) return -1;
  return atomic_exchange_explicit(&g_fn_pending[queue], 0, memory_order_acq_rel);
}

static void rae_fn_mark_pending(int64_t queue) {
  if (queue >= 0 && queue < RAE_FN_PENDING_MAX)
    atomic_store_explicit(&g_fn_pending[queue], 1, memory_order_release);
}
#endif

#ifdef RAE_HAS_KQUEUE
#include <sys/event.h>

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


static void* rae_fn_waker(void* unused) {
  (void)unused;
  for (;;) {
    struct kevent ready[8];
    int count = kevent(g_fn_waker_queue, NULL, 0, ready, 8, NULL);
    if (count > 0) {
      for (int i = 0; i < count; i++) rae_fn_mark_pending((int64_t)ready[i].ident);
      rae_ext_EventLoop_wake();
    } else if (count < 0 && errno != EINTR) {
      return NULL;
    }
  }
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

#endif

#ifdef RAE_HAS_INOTIFY
#include <sys/epoll.h>
#include <sys/inotify.h>

int64_t rae_ext_FileNotify_inotifyCreate(void) {
  int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  return fd >= 0 ? fd : -errno;
}

/* Watch `path` for writes, attribute changes, creation, deletion and renames
 * (in a watched directory, of its entries; of the path itself otherwise): a
 * watch descriptor, or -errno */
int64_t rae_ext_FileNotify_inotifyWatch(int64_t queue, rae_String path) {
  char name[4096];
  if (!path.data || path.len <= 0 || path.len >= (int64_t)sizeof name) return -ENAMETOOLONG;
  memcpy(name, path.data, (size_t)path.len);
  name[path.len] = '\0';
  uint32_t mask = IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF |
                  IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO;
  int watch = inotify_add_watch((int)queue, name, mask);
  return watch >= 0 ? watch : -errno;
}

int64_t rae_ext_FileNotify_inotifyForget(int64_t queue, int64_t watch) {
  return inotify_rm_watch((int)queue, (int)watch) == 0 ? 0 : -errno;
}

/* One non-blocking read of the pending events: their bytes (0 when none) */
int64_t rae_ext_FileNotify_drain(int64_t queue) {
  char events[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t got = read((int)queue, events, sizeof events);
  if (got >= 0) return (int64_t)got;
  return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
}

static void* rae_fn_waker(void* unused) {
  (void)unused;
  for (;;) {
    struct epoll_event ready[8];
    int count = epoll_wait(g_fn_waker_queue, ready, 8, -1);
    if (count > 0) {
      for (int i = 0; i < count; i++) rae_fn_mark_pending((int64_t)ready[i].data.fd);
      rae_ext_EventLoop_wake();
    } else if (count < 0 && errno != EINTR) {
      return NULL;
    }
  }
}

static void rae_fn_start_waker(void) {
  int queue = epoll_create1(EPOLL_CLOEXEC);
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
  struct epoll_event change;
  memset(&change, 0, sizeof change);
  change.events = EPOLLIN | EPOLLET;
  change.data.fd = (int)queue;
  return epoll_ctl(g_fn_waker_queue, EPOLL_CTL_ADD, (int)queue, &change) == 0 ? 0 : -errno;
}

#endif
