#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

/* What the tree needs and Rae has no call for */
int64_t rae_ext_makeLinkAt(const char* target, const char* path) { return symlink(target, path); }
int64_t rae_ext_makeHardLink(const char* path, const char* other) { return link(path, other); }
int64_t rae_ext_makePipeAt(const char* path) { return mkfifo(path, 0600); }
