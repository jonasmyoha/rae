/* Socket waits that suspend a task (docs/lightweight-spawn-design.md §4.3,
 * §15). A resumable task waiting on a socket or a poller (lib/net's
 * systemPollOne / systemPollerWait, c_twin.c) registers its interest here and
 * suspends; the scheduler's readiness thread, which runs Scheduler.rae's
 * schedulerIoLoop, takes the tasks whose socket is ready (or whose timeout
 * passed) one at a time and queues them again.
 *
 * The kernel object is one kqueue owned by the scheduler. A wait is two
 * registrations, both one-shot and carrying the task: the socket's read or
 * write filter, and a timer when the wait has a timeout. The first to fire
 * wins: taking a task deletes its other registration before the task is
 * queued, so no stale event can wake it a second time. `g_sched_io_lock`
 * orders a registration against the taking of its task, and `io_armed`
 * (under that lock) tells a live wait from one whose registration failed.
 *
 * Only what C must do is here: the kevent calls and the thread. Which task
 * runs next is Scheduler.rae's. Without kqueue (Linux for now, the browser)
 * a socket wait does not suspend: the twin makes the call with its own
 * timeout, as the normal function does. */

static void (*g_sched_io_loop)(void) = NULL;
static void (*g_sched_frame)(void) = NULL;

/* The generated main installs Scheduler.rae's readiness loop and its frame
 * step (§16: in the browser each frame of `mainLoop` runs the tasks that
 * can go on) */
void rae_sched_install_io(void (*io_loop)(void), void (*frame)(void)) {
  g_sched_io_loop = io_loop;
  g_sched_frame = frame;
}

void rae_sched_frame(void) {
  if (g_sched_frame) g_sched_frame();
}

/* Whether this is a browser build, whose one thread runs the page's frames
 * and may not block (platform constant) */
rae_Bool rae_ext_Scheduler_inBrowser(void) {
#ifdef __EMSCRIPTEN__
  return 1;
#else
  return 0;
#endif
}

#if defined(RAE_HAS_KQUEUE)
#include <sys/event.h>
#include <poll.h>

static pthread_mutex_t g_sched_io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_sched_io_once = PTHREAD_ONCE_INIT;
static int g_sched_io_queue = -1;

static void* rae_sched_io_thread(void* unused) {
  (void)unused;
  g_sched_io_loop();
  return NULL;
}

static void rae_sched_io_start(void) {
  int queue = kqueue();
  if (queue < 0) return;
  pthread_t thread;
  if (pthread_create(&thread, NULL, rae_sched_io_thread, NULL) != 0) {
    close(queue);
    return;
  }
  pthread_detach(thread);
  g_sched_io_queue = queue;
}

/* Inside a resumable task: suspend until `fd` is ready (`events`: POLLIN /
 * POLLOUT bits; -1: readable) or `timeout_ms` passes (negative: no timeout).
 * 1 when the task is registered (it must not be touched again: the readiness
 * thread may already have queued it), 0 when it did not suspend. */
int rae_sched_io_wait(int64_t fd, int64_t events, int64_t timeout_ms) {
  RaeTask* current = g_sched_current_task;
  if (!current || !g_sched_io_loop || fd < 0 || timeout_ms == 0) return 0;
  pthread_once(&g_sched_io_once, rae_sched_io_start);
  if (g_sched_io_queue < 0) return 0;
  int16_t filter = (events < 0 || (events & POLLIN)) ? EVFILT_READ : EVFILT_WRITE;
  struct kevent changes[2];
  struct kevent receipts[2];
  int count = 0;
  EV_SET(&changes[count++], (uintptr_t)fd, filter, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, current);
  if (timeout_ms > 0) {
    EV_SET(&changes[count++], (uintptr_t)current, EVFILT_TIMER, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0,
           (intptr_t)timeout_ms, current);
  }
  pthread_mutex_lock(&g_sched_io_lock);
  int answered = kevent(g_sched_io_queue, changes, count, receipts, count, NULL);
  int failed = answered != count;
  for (int i = 0; i < answered; i++) {
    if ((receipts[i].flags & EV_ERROR) && receipts[i].data != 0) failed = 1;
  }
  if (failed) {
    /* Take back what did register; a timer that fired meanwhile finds the
     * task not armed and is ignored */
    for (int i = 0; i < count; i++) changes[i].flags = EV_DELETE | EV_RECEIPT;
    kevent(g_sched_io_queue, changes, count, receipts, count, NULL);
    pthread_mutex_unlock(&g_sched_io_lock);
    return 0;
  }
  current->io_fd = fd;
  current->io_filter = filter;
  current->io_timer = timeout_ms > 0;
  current->io_armed = 1;
  pthread_mutex_unlock(&g_sched_io_lock);
  return 1;
}

/* The readiness thread: wait up to `timeout_ms` for one event, and answer the
 * task it wakes, its other registration deleted (0: none, or a stale one) */
int64_t rae_ext_Scheduler_ioNext(int64_t timeout_ms) {
  struct kevent event;
  struct timespec timeout = { (time_t)(timeout_ms / 1000), (long)((timeout_ms % 1000) * 1000000) };
  int ready = kevent(g_sched_io_queue, NULL, 0, &event, 1, &timeout);
  if (ready != 1) return 0;
  RaeTask* task = (RaeTask*)event.udata;
  pthread_mutex_lock(&g_sched_io_lock);
  if (!task || !task->io_armed) {
    pthread_mutex_unlock(&g_sched_io_lock);
    return 0;
  }
  task->io_armed = 0;
  struct kevent changes[2];
  struct kevent receipts[2];
  int count = 0;
  EV_SET(&changes[count++], (uintptr_t)task->io_fd, task->io_filter, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
  if (task->io_timer) EV_SET(&changes[count++], (uintptr_t)task, EVFILT_TIMER, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
  kevent(g_sched_io_queue, changes, count, receipts, count, NULL);
  pthread_mutex_unlock(&g_sched_io_lock);
  return (int64_t)(intptr_t)task;
}

#else

int rae_sched_io_wait(int64_t fd, int64_t events, int64_t timeout_ms) {
  (void)fd; (void)events; (void)timeout_ms;
  return 0;
}

int64_t rae_ext_Scheduler_ioNext(int64_t timeout_ms) {
  (void)timeout_ms;
  return 0;
}

#endif
