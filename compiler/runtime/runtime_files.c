/* runtime_files.c — what a folder's entries are, under lib/Files.
 *
 * Platform reason (docs/runtime-c-audit.md): readdir's and stat's per-OS
 * struct layouts and constants. Each function makes one call and holds no
 * policy; lib/Files says what a kind, a hidden file or a volume is.
 *
 * This module is included by rae_runtime.c after runtime_filesystem.c, whose
 * rae_file_errno_set it uses.
 */
#if defined(__APPLE__)
#include <sys/mount.h>
#elif !defined(__wasm__) && !defined(_WIN32)
#include <sys/statvfs.h>
#endif

/* An entry's kind as lib/Files numbers it: 0 other, 1 file, 2 folder,
 * 3 symlink, -1 not known from readdir */
static int64_t rae_files_kind_of_mode(mode_t mode) {
  if (S_ISREG(mode)) return 1;
  if (S_ISDIR(mode)) return 2;
#ifdef S_ISLNK
  if (S_ISLNK(mode)) return 3;
#endif
  return 0;
}

/* The next entry of an open folder (rae_sys_dir_open), or NULL after the last
 * or on an error (the errno kept, 0 after the last). Valid until the next
 * call on the same folder. */
void* rae_ext_Files_nextEntry(void* folder) {
  if (!folder) return NULL;
  errno = 0;
  struct dirent* entry = readdir((DIR*)folder);
  rae_file_errno_set(entry ? 0 : errno);
  return entry;
}

rae_String rae_ext_Files_entryName(void* entry) {
  if (!entry) return (rae_String){NULL, 0, 0, 0};
  return rae_ext_rae_str_from_cstr((void*)((struct dirent*)entry)->d_name);
}

int64_t rae_ext_Files_entryKind(void* entry) {
  if (!entry) return -1;
#if defined(_WIN32)
  return -1;
#else
  switch (((struct dirent*)entry)->d_type) {
    case DT_REG: return 1;
    case DT_DIR: return 2;
    case DT_LNK: return 3;
    case DT_UNKNOWN: return -1;
    default: return 0;
  }
#endif
}

/* stat (`follow`) or lstat of `path` into six words: the kind, the size, the
 * modified time in whole seconds and nanoseconds, 1 when the OS's hidden flag
 * is set, the device. 0, or -errno. */
int64_t rae_ext_Files_status(rae_String path, rae_Bool follow, void* words) {
  if (!path.data || !words) return -EINVAL;
  struct stat st;
#if defined(_WIN32)
  (void)follow;
  if (stat((const char*)path.data, &st) != 0) return -(int64_t)errno;
#else
  int failed = follow ? stat((const char*)path.data, &st) : lstat((const char*)path.data, &st);
  if (failed) return -(int64_t)errno;
#endif
  int64_t* out = (int64_t*)words;
  out[0] = rae_files_kind_of_mode(st.st_mode);
  out[1] = (int64_t)st.st_size;
  out[4] = 0;
#if defined(__APPLE__)
  out[2] = (int64_t)st.st_mtimespec.tv_sec;
  out[3] = (int64_t)st.st_mtimespec.tv_nsec;
  out[4] = (st.st_flags & UF_HIDDEN) ? 1 : 0;
#elif defined(_WIN32)
  out[2] = (int64_t)st.st_mtime;
  out[3] = 0;
#else
  out[2] = (int64_t)st.st_mtim.tv_sec;
  out[3] = (int64_t)st.st_mtim.tv_nsec;
#endif
  out[5] = (int64_t)st.st_dev;
  return 0;
}

/* The flags of the volume `path` is on: 1 read-only, 2 removable, 4 not to be
 * shown to people (the last two where the OS says so), or -errno */
int64_t rae_ext_Files_volumeFlags(rae_String path) {
  if (!path.data) return -EINVAL;
#if defined(__APPLE__)
  struct statfs st;
  if (statfs((const char*)path.data, &st) != 0) return -(int64_t)errno;
  return ((st.f_flags & MNT_RDONLY) ? 1 : 0) | ((st.f_flags & MNT_REMOVABLE) ? 2 : 0) |
         ((st.f_flags & MNT_DONTBROWSE) ? 4 : 0);
#elif defined(__wasm__) || defined(_WIN32)
  return -ENOSYS;
#else
  struct statvfs st;
  if (statvfs((const char*)path.data, &st) != 0) return -(int64_t)errno;
  return (st.f_flag & ST_RDONLY) ? 1 : 0;
#endif
}
