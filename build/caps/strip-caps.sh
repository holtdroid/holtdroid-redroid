#!/bin/sh
# Keeps in every init service's capabilities only those Holtdroid's
# unprivileged Android container has in its bounding set, so init can start
# the service instead of failing it (logd's SYSLOG and AUDIT_CONTROL, the
# Wi-Fi HAL's SYS_MODULE, uprobestats' PERFMON). Changed files go to /out.
set -eu
allowed=" CHOWN DAC_OVERRIDE DAC_READ_SEARCH FOWNER FSETID KILL SETGID SETUID SETPCAP NET_BIND_SERVICE NET_ADMIN NET_RAW IPC_LOCK SYS_CHROOT SYS_PTRACE SYS_ADMIN SYS_BOOT SYS_NICE SYS_RESOURCE WAKE_ALARM BLOCK_SUSPEND BPF "
cd /in
for f in $(grep -rlE '^[[:space:]]*capabilities([[:space:]]|$)' .); do
  awk -v allowed="$allowed" '
    /^[ \t]*capabilities([ \t]|$)/ {
      match($0, /^[ \t]*/); out = substr($0, 1, RLENGTH) "capabilities"
      for (i = 2; i <= NF; i++) if (index(allowed, " " $i " ")) out = out " " $i
      print out; next
    }
    { print }' "$f" > /tmp/rc
  if ! cmp -s "$f" /tmp/rc; then
    mkdir -p "/out/${f%/*}"
    cp /tmp/rc "/out/$f"
  fi
done
grep -qx '    capabilities' /out/system/etc/init/logd.rc
