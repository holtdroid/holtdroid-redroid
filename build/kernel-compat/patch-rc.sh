#!/bin/sh
# Rewrites the image's init files in /in for Android 16 on a mainline kernel:
# netbpfload through holtdroid_netbpfload, CAP_BPF for netd, and the ashmem
# shim for services the global LD_PRELOAD (holtdroid-ashmem.rc, an absolute
# path that system and APEX namespaces may load) can't serve: vendor ones
# load it by name from /vendor, 32-bit ones from /system/lib.
set -eu
cd /in
grep -q '^    exec_start bpfloader$' system/etc/init/hw/init.rc
sed -i 's/^    exec_start bpfloader$/    exec_start holtdroid_netbpfload/' system/etc/init/hw/init.rc
grep -q '^    capabilities CHOWN .* NET_ADMIN ' system/etc/init/netd.rc
sed -i 's/^\(    capabilities CHOWN .*\)$/\1 BPF/' system/etc/init/netd.rc
grep -q ' NET_ADMIN .* BPF$' system/etc/init/netd.rc
elf32() {
  case "$1" in
  /apex/*) case "$1" in *32) return 0 ;; esac; return 1 ;;
  /system/*) [ -f "/bin-in${1#/system}" ] && [ "$(od -An -tx1 -j4 -N1 "/bin-in${1#/system}" | tr -d ' ')" = 01 ] ;;
  *) return 1 ;;
  esac
}
for f in $(find . -name '*.rc'); do
  case "$f" in ./vendor/*) part=vendor ;; *) part=system ;; esac
  : > /tmp/rc
  add=""
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
    "service "*)
      set -- $line
      add=""
      if [ "$part" = vendor ]; then add=libholtdroid_ashmem.so; elif elf32 "$3"; then add=/system/lib/libholtdroid_ashmem.so; fi
      ;;
    esac
    printf '%s\n' "$line" >> /tmp/rc
    case "$line" in *\\) ;; *) [ -z "$add" ] || { printf '    setenv LD_PRELOAD %s\n' "$add" >> /tmp/rc; add=""; } ;; esac
  done < "$f"
  cmp -s "$f" /tmp/rc || cp /tmp/rc "$f"
done
for f in $(grep -rlE '^ *exec(_background)? .*-- /(vendor|odm)/' .); do
  sed -i -E 's#^( *exec(_background)? .*-- )(/(vendor|odm)/)#\1/system/bin/env LD_PRELOAD=libholtdroid_ashmem.so \3#' "$f"
done
! grep -rqE '^ *exec(_background)? .*-- /(vendor|odm)/' .
grep -A1 '^service zygote_secondary ' system/etc/init/hw/init.zygote64_32.rc | grep -q 'setenv LD_PRELOAD /system/lib/libholtdroid_ashmem.so'
grep -rq 'setenv LD_PRELOAD libholtdroid_ashmem.so' vendor/etc/init
! grep -rh -B1 '^    setenv LD_PRELOAD' . | grep -q '\\$'
find . -type d -exec chmod 755 {} +
find . -type f -exec chmod 644 {} +
