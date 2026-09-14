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

/* ----- Channel(T) runtime: MPSC ring of T-sized slots (#271, #969) ------ */
/* Backs lib/Channel.rae's `Channel(T)`, Rae's first message-passing
 * primitive. Multi-producer, single-consumer: any thread may send; only the
 * owning consumer thread drains. A pthread mutex serialises every access and
 * is hidden entirely inside the runtime — ordinary Rae code never locks.
 *
 * #969: the ring is a byte array of `cap * elem_size`, so ANY Rae value type
 * is a payload — the element size comes from `sizeof(T)` on the Rae side, and
 * the Rae side does the typed store/load (`rae_ext_rae_buf_set/get`, the same
 * per-T specialisation List uses) while this kernel only hands out slot
 * INDICES under the lock:
 *   reserve -> (Rae stores into slot) -> commit      one producer step
 *   take    -> (Rae copies out of slot) -> release   the consumer step
 * The lock is held across the Rae store/copy, which is a memcpy of T. A full
 * ring refuses the send (reserve = -1); an empty ring refuses the take. The
 * consumer moves the bytes out, so the ring never owns a live T after
 * `release`; `freeChannel` drains what is left (dropping each T in Rae)
 * before this frees the storage. `recv_count` is the monotonic read-only
 * observable of consumer progress. Guarded out for single-threaded wasm,
 * matching RaeTask above. */
#define RAE_CHAN_CAP 4096
typedef struct {
  pthread_mutex_t mu;
  uint8_t* buf;        /* cap * elem_size bytes */
  int64_t elem_size;
  int64_t cap;
  int64_t head;        /* next index to pop */
  int64_t count;       /* items currently buffered */
  int64_t recv_count;  /* total drained (read-only observable) */
} RaeChannel;

#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
#define RAE_CHAN_LOCK(c)   pthread_mutex_lock(&(c)->mu)
#define RAE_CHAN_UNLOCK(c) pthread_mutex_unlock(&(c)->mu)
#else
#define RAE_CHAN_LOCK(c)   ((void)0)
#define RAE_CHAN_UNLOCK(c) ((void)0)
#endif

int64_t rae_ext_rae_chan_new(int64_t elem_size) {
  RaeChannel* c = (RaeChannel*)malloc(sizeof(RaeChannel));
  if (elem_size < 1) elem_size = 1;
  c->elem_size = elem_size;
  c->cap = RAE_CHAN_CAP;
  c->buf = (uint8_t*)calloc((size_t)c->cap, (size_t)elem_size);
  c->head = 0; c->count = 0; c->recv_count = 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_init(&c->mu, NULL);
#endif
  return (int64_t)(intptr_t)c;
}

/* The slot array, typed on the Rae side as Buffer(T). */
void* rae_ext_rae_chan_ring(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  return c ? (void*)c->buf : NULL;
}

/* Producer: lock and hand out the tail slot, or -1 (and no lock held) when
 * the ring is full. The caller stores into the slot, then commits. */
int64_t rae_ext_rae_chan_reserve(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return -1;
  RAE_CHAN_LOCK(c);
  if (c->count >= c->cap) { RAE_CHAN_UNLOCK(c); return -1; }
  return (c->head + c->count) % c->cap;
}

void rae_ext_rae_chan_commit(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
  c->count++;
  RAE_CHAN_UNLOCK(c);
}

/* Consumer: lock and hand out the head slot, or -1 (no lock held) when
 * empty. The caller copies the value out, then releases. */
int64_t rae_ext_rae_chan_take(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return -1;
  RAE_CHAN_LOCK(c);
  if (c->count <= 0) { RAE_CHAN_UNLOCK(c); return -1; }
  return c->head;
}

void rae_ext_rae_chan_release(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
  /* The slot's bytes were moved out; zero it so a stale pointer never sits
   * in the ring (a later diagnostic dump reads clean slots). */
  memset(c->buf + (size_t)c->head * (size_t)c->elem_size, 0, (size_t)c->elem_size);
  c->head = (c->head + 1) % c->cap;
  c->count--;
  c->recv_count++;
  RAE_CHAN_UNLOCK(c);
}

int64_t rae_ext_rae_chan_count(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
  RAE_CHAN_LOCK(c);
  int64_t n = c->count;
  RAE_CHAN_UNLOCK(c);
  return n;
}

int64_t rae_ext_rae_chan_received(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
  RAE_CHAN_LOCK(c);
  int64_t n = c->recv_count;
  RAE_CHAN_UNLOCK(c);
  return n;
}

void rae_ext_rae_chan_free(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_destroy(&c->mu);
#endif
  free(c->buf);
  free(c);
}

