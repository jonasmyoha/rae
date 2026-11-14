#include <pthread.h>
#include <stdint.h>

/* The Rae function this C calls (cExport), and the named attach */
void rae_test_decode_step(void);
void rae_thread_attach_named(const char* name);

/* A thread C makes, as a decoder would: it attaches with a name, then runs Rae code */
static void* decoder_thread(void* argument) {
  (void)argument;
  rae_thread_attach_named("decoder");
  rae_test_decode_step();
  return 0;
}

int64_t rae_ext_runDecoder(void) {
  pthread_t thread;
  if (pthread_create(&thread, 0, decoder_thread, 0) != 0) return 0;
  pthread_join(thread, 0);
  return 1;
}
