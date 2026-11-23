#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/types.h>
#endif

/* What a test tree needs and Rae has no call for: a symlink, a pipe, the
 * OS's hidden flag (false where there is none) */
int64_t rae_ext_makeLink(const char* target, const char* path) { return symlink(target, path); }
int64_t rae_ext_makePipe(const char* path) { return mkfifo(path, 0600); }
int64_t rae_ext_hide(const char* path) {
#if defined(__APPLE__)
  return chflags(path, UF_HIDDEN);
#else
  (void)path;
  return -1;
#endif
}
