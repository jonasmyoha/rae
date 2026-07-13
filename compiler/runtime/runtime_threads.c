/* Task and channel runtime primitives. Permanent C kernel for OS-thread and mutex-backed concurrency; higher task/channel policy can migrate to Rae later.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* ----- Task(T) runtime (compiled backend) ----------------------------- */
/* A spawned task gets one RaeTask + one OS thread. The per-spawned-function
 * thunk (emitted by the C backend) runs the function and stores its result
 * into `result`; `rae_task_await` joins exactly once and hands back the
 * buffer, which the get() call site casts to T. */
RaeTask* rae_task_new(size_t result_size) {
  RaeTask* t = (RaeTask*)malloc(sizeof(RaeTask));
  t->result = result_size ? malloc(result_size) : NULL;
  t->done = 0;
  t->joined = 0;
  return t;
}

void* rae_task_await(RaeTask* t) {
  if (!t) return NULL;
  if (!t->joined) {
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
    pthread_join(t->thread, NULL);
#endif
    t->joined = 1;
  }
  return t->result;
}

/* Scope-exit drop (join-on-drop): a Task local that goes out of scope is
 * joined so its worker can't outlive the scope (and isn't killed mid-run at
 * process teardown), then freed. NOTE: the result buffer's own contents are
 * not cascade-dropped here — a heap result that was never get()'d leaks its
 * payload; acceptable for now (callers normally get() the result). */
void rae_task_drop(RaeTask* t) {
  if (!t) return;
  if (!t->joined) {
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
    pthread_join(t->thread, NULL);
#endif
    t->joined = 1;
  }
  free(t->result);
  free(t);
}

/* ----- Channel(T) runtime: MPSC int64 channel (#271) ------------------ */
/* Backs lib/channel.rae's `Channel(T)`, Rae's first message-passing
 * primitive. Multi-producer, single-consumer: any thread may _send; only
 * the owning consumer thread drains via _recv (after checking _count). A
 * pthread mutex serialises every access and is hidden entirely inside the
 * runtime — ordinary Rae code never locks. `recv_count` is the monotonic
 * read-only observable of consumer progress. Fixed ring (cap chosen large
 * enough for the proof); a send to a full ring is dropped (the proof never
 * overflows). The channel is referenced from Rae by an opaque Int handle
 * (this pointer), so a `spawn`'d by-value copy of Channel(T) shares the one
 * underlying channel. The mutex calls are guarded out for single-threaded
 * wasm, matching RaeTask above. */
#define RAE_CHAN_CAP 4096
typedef struct {
  pthread_mutex_t mu;
  int64_t buf[RAE_CHAN_CAP];
  int64_t head;        /* next index to pop */
  int64_t count;       /* items currently buffered */
  int64_t recv_count;  /* total drained (read-only observable) */
} RaeChannel;

int64_t rae_ext_rae_chan_new(void) {
  RaeChannel* c = (RaeChannel*)malloc(sizeof(RaeChannel));
  c->head = 0; c->count = 0; c->recv_count = 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_init(&c->mu, NULL);
#endif
  return (int64_t)(intptr_t)c;
}

void rae_ext_rae_chan_send(int64_t ch, int64_t value) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_lock(&c->mu);
#endif
  if (c->count < RAE_CHAN_CAP) {
    int64_t tail = (c->head + c->count) % RAE_CHAN_CAP;
    c->buf[tail] = value;
    c->count++;
  }
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_unlock(&c->mu);
#endif
}

int64_t rae_ext_rae_chan_count(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_lock(&c->mu);
#endif
  int64_t n = c->count;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_unlock(&c->mu);
#endif
  return n;
}

int64_t rae_ext_rae_chan_recv(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_lock(&c->mu);
#endif
  int64_t v = 0;
  if (c->count > 0) {
    v = c->buf[c->head];
    c->head = (c->head + 1) % RAE_CHAN_CAP;
    c->count--;
    c->recv_count++;
  }
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_unlock(&c->mu);
#endif
  return v;
}

int64_t rae_ext_rae_chan_received(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_lock(&c->mu);
#endif
  int64_t n = c->recv_count;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_unlock(&c->mu);
#endif
  return n;
}

void rae_ext_rae_chan_free(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_destroy(&c->mu);
#endif
  free(c);
}

