#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

enum {
  ASHMEM_SET_NAME = 1,
  ASHMEM_GET_NAME,
  ASHMEM_SET_SIZE,
  ASHMEM_GET_SIZE,
  ASHMEM_SET_PROT_MASK,
  ASHMEM_GET_PROT_MASK,
  ASHMEM_PIN,
  ASHMEM_UNPIN,
  ASHMEM_GET_PIN_STATUS,
  ASHMEM_PURGE_ALL_CACHES,
};

extern char **environ;

__attribute__((constructor)) static void hide(void) {
  char **out = environ;
  for (char **e = environ; *e; e++) {
    if (strncmp(*e, "LD_PRELOAD=", 11) == 0) memset(*e, 0, strlen(*e));
    else *out++ = *e;
  }
  *out = NULL;
}

static int memfd_ioctl(int fd, int nr, void *arg, int *ret) {
  int seals = fcntl(fd, F_GET_SEALS);
  if (seals < 0) return 0;
  int sealed = seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE);
  struct stat st;
  switch (nr) {
  case ASHMEM_SET_NAME:
  case ASHMEM_PIN:
  case ASHMEM_UNPIN:
  case ASHMEM_PURGE_ALL_CACHES:
    *ret = 0;
    return 1;
  case ASHMEM_GET_NAME: {
    char path[32], link[256];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, link, sizeof link - 1);
    link[n < 0 ? 0 : n] = 0;
    char *name = strncmp(link, "/memfd:", 7) == 0 ? link + 7 : link;
    char *deleted = strstr(name, " (deleted)");
    if (deleted) *deleted = 0;
    snprintf(arg, 256, "%s", name);
    *ret = 0;
    return 1;
  }
  case ASHMEM_SET_SIZE:
    *ret = ftruncate(fd, (off_t)(size_t)arg);
    return 1;
  case ASHMEM_GET_SIZE:
    *ret = fstat(fd, &st) == 0 ? (int)st.st_size : -1;
    return 1;
  case ASHMEM_SET_PROT_MASK:
    *ret = 0;
    if (!((unsigned long)arg & PROT_WRITE) && !sealed && fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) < 0 && errno != EPERM)
      *ret = -1;
    return 1;
  case ASHMEM_GET_PROT_MASK:
    *ret = PROT_READ | PROT_EXEC | (sealed ? 0 : PROT_WRITE);
    return 1;
  case ASHMEM_GET_PIN_STATUS:
    *ret = 1;
    return 1;
  }
  return 0;
}

int ioctl(int fd, int req, ...) {
  static int (*real)(int, int, ...);
  va_list ap;
  va_start(ap, req);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (!real) real = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "ioctl");
  int ret;
  if (_IOC_TYPE((unsigned)req) == 0x77 && memfd_ioctl(fd, _IOC_NR((unsigned)req), arg, &ret)) return ret;
  return real(fd, req, arg);
}
