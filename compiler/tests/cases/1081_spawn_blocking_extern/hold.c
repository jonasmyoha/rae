/* Calls that hold their thread until they are released: each waits for a
 * signal, not for a time, so the case says the same on a slow machine. The
 * bounds only end a run that is already wrong. */
#include <pthread.h>
#include <stdint.h>
#include <time.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int64_t held = 0;
static int released = 0;
static int givenUp = 0;

static struct timespec deadlineIn(int seconds) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += seconds;
  return deadline;
}

/* Hold the calling thread until releaseThreads(); `value`, or -1 when nobody
 * released it (then no later call holds either) */
int64_t rae_ext_holdThread(int64_t value) {
  struct timespec deadline = deadlineIn(45);
  pthread_mutex_lock(&mutex);
  held++;
  pthread_cond_broadcast(&changed);
  while (!released && !givenUp) {
    if (pthread_cond_timedwait(&changed, &mutex, &deadline) != 0) {
      givenUp = 1;
      pthread_cond_broadcast(&changed);
    }
  }
  int64_t result = released && !givenUp ? value : -1;
  held--;
  pthread_mutex_unlock(&mutex);
  return result;
}

/* Wait until `count` calls hold a thread at once, and say how many do. Too few
 * after 30 s: the calls go on without being released, so the case ends. */
int64_t rae_ext_waitForHeld(int64_t count) {
  struct timespec deadline = deadlineIn(30);
  pthread_mutex_lock(&mutex);
  while (held < count) {
    if (pthread_cond_timedwait(&changed, &mutex, &deadline) != 0) break;
  }
  int64_t result = held;
  if (result < count) {
    givenUp = 1;
    pthread_cond_broadcast(&changed);
  }
  pthread_mutex_unlock(&mutex);
  return result;
}

int64_t rae_ext_heldThreads(void) {
  pthread_mutex_lock(&mutex);
  int64_t result = held;
  pthread_mutex_unlock(&mutex);
  return result;
}

void rae_ext_releaseThreads(void) {
  pthread_mutex_lock(&mutex);
  released = 1;
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&mutex);
}
