#include <stdint.h>
#include <unistd.h>

/* C that holds its thread for `ms` milliseconds, as a file read or a name
 * lookup can */
int64_t rae_ext_holdThread(int64_t ms) {
  usleep((useconds_t)(ms * 1000));
  return ms;
}
