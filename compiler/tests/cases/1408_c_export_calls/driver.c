#include <pthread.h>
#include <stdint.h>

/* The Rae functions this C calls, by the names their cExport gave them */
int64_t rae_test_scale(int64_t value, double factor);
float rae_test_mix(float left, float right);
int64_t rae_test_describe(int64_t number);
void rae_thread_attach(void);

/* A thread C makes, as an audio device or a decoder does: it attaches, then
 * calls Rae code that allocates a String and logs */
static void* foreign_thread(void* argument) {
  rae_thread_attach();
  int64_t length = rae_test_describe((int64_t)(intptr_t)argument);
  return (void*)(intptr_t)length;
}

/* Called from Rae's main (an ordinary extern): C calls back into Rae on this
 * thread, then from a thread of its own */
int64_t rae_ext_driveExports(int64_t rounds) {
  int64_t total = 0;
  for (int64_t i = 0; i < rounds; i++) total += rae_test_scale(i, 2.5);
  float mixed = rae_test_mix(0.25f, 0.5f);
  total += (int64_t)(mixed * 1000.0f);
  pthread_t thread;
  void* answer = 0;
  if (pthread_create(&thread, 0, foreign_thread, (void*)(intptr_t)42) == 0) {
    pthread_join(thread, &answer);
    total += (int64_t)(intptr_t)answer;
  }
  return total;
}
