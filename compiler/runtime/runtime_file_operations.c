/* runtime_file_operations.c — the calls under lib/Files' operations (copy,
 * move, trash).
 *
 * Platform reason (docs/runtime-c-audit.md): the system calls themselves,
 * their per-OS flags and errno. One call each, 0 or a count or a handle on
 * success, -errno on failure; the sequence, the retries and what an error
 * means are lib/Files'. No call here follows a symlink at the path it is
 * given, and none overwrites: a new file, a new folder, a link and
 * renameExclusive all fail when the name is taken.
 */
#if defined(__APPLE__)
#include <copyfile.h>
#include <sys/clonefile.h>
#include <TargetConditionals.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__wasm__) || defined(_WIN32)
#define RAE_NO_FILE_OPERATIONS 1
#endif

#ifdef RAE_NO_FILE_OPERATIONS
#define RAE_FILES_CALL(expression) (-(int64_t)ENOSYS)
#else
#define RAE_FILES_CALL(expression) ((expression) == 0 ? 0 : -(int64_t)errno)
#endif

#define RAE_FILES_PATH(path) ((const char*)(path).data)

/* Open a file to read: never a link, never waiting for a pipe's writer */
int64_t rae_ext_Files_openRead(rae_String path) {
#ifdef RAE_NO_FILE_OPERATIONS
  (void)path; return -ENOSYS;
#else
  int fd = open(RAE_FILES_PATH(path), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  return fd >= 0 ? (int64_t)fd : -(int64_t)errno;
#endif
}

/* A new file to write, for its owner only until its metadata is copied; fails
 * when the name is taken */
int64_t rae_ext_Files_createNew(rae_String path) {
#ifdef RAE_NO_FILE_OPERATIONS
  (void)path; return -ENOSYS;
#else
  int fd = open(RAE_FILES_PATH(path), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  return fd >= 0 ? (int64_t)fd : -(int64_t)errno;
#endif
}

int64_t rae_ext_Files_readChunk(int64_t fd, void* bytes, int64_t count) {
  ssize_t got = read((int)fd, bytes, (size_t)count);
  return got >= 0 ? (int64_t)got : -(int64_t)errno;
}

int64_t rae_ext_Files_writeChunk(int64_t fd, const void* bytes, int64_t from, int64_t count) {
  ssize_t put = write((int)fd, (const uint8_t*)bytes + from, (size_t)count);
  return put >= 0 ? (int64_t)put : -(int64_t)errno;
}

int64_t rae_ext_Files_syncFile(int64_t fd) { return RAE_FILES_CALL(fsync((int)fd)); }

int64_t rae_ext_Files_closeFile(int64_t fd) { return close((int)fd) == 0 ? 0 : -(int64_t)errno; }

/* The file at `path` as a copy-on-write clone under a new name (APFS) */
int64_t rae_ext_Files_clone(rae_String path, rae_String copy) {
#if defined(__APPLE__)
  return RAE_FILES_CALL(clonefile(RAE_FILES_PATH(path), RAE_FILES_PATH(copy), CLONE_NOFOLLOW));
#else
  (void)path; (void)copy;
  return -ENOTSUP;
#endif
}

/* Permissions, dates, extended attributes and access lists, where one call
 * copies them */
int64_t rae_ext_Files_copyMetadata(rae_String path, rae_String copy) {
#if defined(__APPLE__)
  return RAE_FILES_CALL(copyfile(RAE_FILES_PATH(path), RAE_FILES_PATH(copy), NULL,
                                 COPYFILE_METADATA | COPYFILE_NOFOLLOW));
#else
  (void)path; (void)copy;
  return -ENOTSUP;
#endif
}

/* The modified (and accessed) time of `path` itself */
int64_t rae_ext_Files_setTimes(rae_String path, int64_t seconds, int64_t nanoseconds) {
#ifdef RAE_NO_FILE_OPERATIONS
  (void)path; (void)seconds; (void)nanoseconds; return -ENOSYS;
#else
  struct timespec times[2];
  times[0].tv_sec = (time_t)seconds;
  times[0].tv_nsec = (long)nanoseconds;
  times[1] = times[0];
  return RAE_FILES_CALL(utimensat(AT_FDCWD, RAE_FILES_PATH(path), times, AT_SYMLINK_NOFOLLOW));
#endif
}

/* What a symlink says; empty, with the errno kept, when it cannot be read */
rae_String rae_ext_Files_readLink(rae_String path) {
#ifdef RAE_NO_FILE_OPERATIONS
  (void)path;
  rae_file_errno_set(ENOSYS);
  return (rae_String){NULL, 0, 0, 0};
#else
  char target[4096];
  ssize_t length = readlink(RAE_FILES_PATH(path), target, sizeof target);
  /* a target as long as the buffer may have been cut */
  if (length < 0 || length >= (ssize_t)sizeof target) {
    rae_file_errno_set(length < 0 ? errno : ENAMETOOLONG);
    return (rae_String){NULL, 0, 0, 0};
  }
  rae_file_errno_set(0);
  return rae_ext_rae_str_from_buf((uint8_t*)target, (int64_t)length);
#endif
}

int64_t rae_ext_Files_makeLink(rae_String target, rae_String path) {
  return RAE_FILES_CALL(symlink(RAE_FILES_PATH(target), RAE_FILES_PATH(path)));
}

int64_t rae_ext_Files_makeFolder(rae_String path, int64_t mode) {
  return RAE_FILES_CALL(mkdir(RAE_FILES_PATH(path), (mode_t)mode));
}

int64_t rae_ext_Files_removeFile(rae_String path) { return RAE_FILES_CALL(unlink(RAE_FILES_PATH(path))); }

int64_t rae_ext_Files_removeFolder(rae_String path) { return RAE_FILES_CALL(rmdir(RAE_FILES_PATH(path))); }

/* Rename that fails with EEXIST when the new name is taken; ENOTSUP, EINVAL or
 * ENOSYS where the OS or the disk has no such call */
int64_t rae_ext_Files_renameExclusive(rae_String path, rae_String to) {
#if defined(__APPLE__)
  return RAE_FILES_CALL(renamex_np(RAE_FILES_PATH(path), RAE_FILES_PATH(to), RENAME_EXCL));
#elif defined(__linux__) && defined(SYS_renameat2)
  /* 1: RENAME_NOREPLACE */
  return RAE_FILES_CALL(syscall(SYS_renameat2, AT_FDCWD, RAE_FILES_PATH(path), AT_FDCWD,
                                RAE_FILES_PATH(to), 1));
#else
  (void)path; (void)to;
  return -ENOTSUP;
#endif
}

/* Rename as the OS has it (it replaces a file): 0 or -errno */
int64_t rae_ext_Files_renameAsIs(rae_String path, rae_String to) {
  return RAE_FILES_CALL(rename(RAE_FILES_PATH(path), RAE_FILES_PATH(to)));
}

/* The path with every symlink resolved; empty when it cannot be */
rae_String rae_ext_Files_realPath(rae_String path) {
#ifdef RAE_NO_FILE_OPERATIONS
  (void)path;
  return (rae_String){NULL, 0, 0, 0};
#else
  char* real = path.data ? realpath(RAE_FILES_PATH(path), NULL) : NULL;
  if (!real) return (rae_String){NULL, 0, 0, 0};
  rae_String text = rae_ext_rae_str_from_cstr((void*)real);
  free(real);
  return text;
#endif
}

int64_t rae_ext_Files_processId(void) {
#ifdef RAE_NO_FILE_OPERATIONS
  return 1;
#else
  return (int64_t)getpid();
#endif
}

int64_t rae_ext_Files_userId(void) {
#ifdef RAE_NO_FILE_OPERATIONS
  return 0;
#else
  return (int64_t)getuid();
#endif
}

#if defined(__APPLE__) && TARGET_OS_OSX
/* NSFileManager trashItemAtURL: where the item went; empty when it could not
 * be trashed */
rae_String rae_ext_Files_systemTrash(rae_String path) {
  rae_String result = {NULL, 0, 0, 0};
  if (!path.data) return result;
  extern void* objc_autoreleasePoolPush(void);
  extern void objc_autoreleasePoolPop(void* pool);
  void* pool = objc_autoreleasePoolPush();
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, path.data, (CFIndex)path.len, false);
  if (url) {
    void* manager = ((void* (*)(Class, SEL))objc_msgSend)(objc_getClass("NSFileManager"),
                                                          sel_registerName("defaultManager"));
    void* landed = NULL;
    signed char ok = ((signed char (*)(void*, SEL, const void*, void**, void**))objc_msgSend)(
        manager, sel_registerName("trashItemAtURL:resultingItemURL:error:"), url, &landed, NULL);
    char text[4096];
    if (ok && landed &&
        CFURLGetFileSystemRepresentation((CFURLRef)landed, true, (UInt8*)text, sizeof text)) {
      result = rae_ext_rae_str_from_cstr((void*)text);
    }
    CFRelease(url);
  }
  objc_autoreleasePoolPop(pool);
  return result;
}
#endif
