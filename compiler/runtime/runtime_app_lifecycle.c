/* The app's lifecycle for lib/AppLifecycle (docs/app-lifecycle.md): what the system said as the
 * app went to the background and came back, kept for the program to read once a frame.
 *
 * Platform reason: the system's lifecycle callbacks. SDL does not queue them for the app: it hands
 * them to an event watcher at once, on the thread that noticed (the app's own on iOS, Java's on
 * Android), so they are recorded here as they come (rae_app_lifecycle_record, called by
 * runtime_gpu2d_platform.c's watcher, or by a simulation without a window) and the frame loop is
 * woken (EventLoop.wake) to read them. A small ring of event numbers and whether the app is in
 * front; what an event means to the program is Rae's.
 *
 * Included by rae_runtime.c in every build. */
#include <stdatomic.h>

/* 0 willEnterBackground, 1 didEnterBackground, 2 willEnterForeground, 3 didEnterForeground */
#define RAE_APP_LIFECYCLE_RING 64
static _Atomic int64_t rae_app_lifecycle_ring[RAE_APP_LIFECYCLE_RING];
static _Atomic int64_t rae_app_lifecycle_written = 0;
static _Atomic int64_t rae_app_lifecycle_read = 0;
static _Atomic int rae_app_in_foreground = 1;

void rae_ext_EventLoop_wake(void);

/* One lifecycle event, as the system sent it (any thread). The oldest is dropped when the program
 * has not read 64 of them. */
void rae_app_lifecycle_record(int64_t kind) {
  if (kind == 0) atomic_store(&rae_app_in_foreground, 0);
  if (kind == 3) atomic_store(&rae_app_in_foreground, 1);
  int64_t written = atomic_load(&rae_app_lifecycle_written);
  atomic_store(&rae_app_lifecycle_ring[written % RAE_APP_LIFECYCLE_RING], kind);
  atomic_store(&rae_app_lifecycle_written, written + 1);
  rae_ext_EventLoop_wake();
}

/* The next event not read yet, -1 for none */
int64_t rae_ext_AppLifecycle_next(void) {
  int64_t read = atomic_load(&rae_app_lifecycle_read);
  int64_t written = atomic_load(&rae_app_lifecycle_written);
  if (written - read > RAE_APP_LIFECYCLE_RING) read = written - RAE_APP_LIFECYCLE_RING;
  if (read >= written) return -1;
  int64_t kind = atomic_load(&rae_app_lifecycle_ring[read % RAE_APP_LIFECYCLE_RING]);
  atomic_store(&rae_app_lifecycle_read, read + 1);
  return kind;
}

/* Whether the app is in front: false from its willEnterBackground to its didEnterForeground */
rae_Bool rae_ext_AppLifecycle_inForeground(void) { return atomic_load(&rae_app_in_foreground) ? 1 : 0; }
