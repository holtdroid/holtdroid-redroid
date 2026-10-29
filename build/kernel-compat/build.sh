#!/bin/sh
# Builds the shims for Android 16 on a mainline kernel: uname() reporting an
# LTS version to netbpfload, and the ACK kernel's ashmem ioctls on memfds.
set -eu
TC=/w/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p /out/system/lib64 /out/system/lib /out/vendor/lib64 /out/vendor/lib
"$TC/x86_64-linux-android30-clang" -shared -fPIC -O2 -Wall -Werror -o /out/system/lib64/libholtdroid_uname.so /w/uname.c
"$TC/x86_64-linux-android30-clang" -shared -fPIC -O2 -Wall -Werror -o /out/system/lib64/libholtdroid_ashmem.so /w/ashmem.c
"$TC/i686-linux-android30-clang" -shared -fPIC -O2 -Wall -Werror -o /out/system/lib/libholtdroid_ashmem.so /w/ashmem.c
cp /out/system/lib64/libholtdroid_ashmem.so /out/vendor/lib64/
cp /out/system/lib/libholtdroid_ashmem.so /out/vendor/lib/
"$TC/llvm-strip" /out/system/lib64/*.so /out/system/lib/*.so /out/vendor/lib64/*.so /out/vendor/lib/*.so
