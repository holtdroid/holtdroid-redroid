#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#define LTS_MAJOR 6
#define LTS_MINOR 12

int uname(struct utsname *u) {
  static int (*real)(struct utsname *);
  if (!real) real = (int (*)(struct utsname *))dlsym(RTLD_NEXT, "uname");
  int r = real(u);
  int major, minor, n;
  if (r == 0 && sscanf(u->release, "%d.%d%n", &major, &minor, &n) == 2 &&
      (major > LTS_MAJOR || (major == LTS_MAJOR && minor > LTS_MINOR))) {
    char rest[sizeof u->release];
    snprintf(rest, sizeof rest, "%s", u->release + n);
    snprintf(u->release, sizeof u->release, "%d.%d%s", LTS_MAJOR, LTS_MINOR, rest);
  }
  return r;
}
