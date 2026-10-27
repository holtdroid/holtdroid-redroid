#!/bin/sh
# Builds the camera HAL in /w: the AIDL NDK stubs of camera provider V1 and
# its imports, camera_metadata and hal.cpp, linked against the image's own
# libraries in /w/lib.
set -eu
cd /w
TC=/w/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
API=33
I=hardware-interfaces
api() { echo "$I/$1/aidl/aidl_api/$2/$3"; }
P1=$(api camera/provider android.hardware.camera.provider 1)
D1=$(api camera/device android.hardware.camera.device 1)
C1=$(api camera/common android.hardware.camera.common 1)
M1=$(api camera/metadata android.hardware.camera.metadata 1)
G5=$(api graphics/common android.hardware.graphics.common 5)
N2=$(api common android.hardware.common 2)
F1=$(api common/fmq android.hardware.common.fmq 1)
inc="-I $P1 -I $D1 -I $C1 -I $M1 -I $G5 -I $N2 -I $F1"
mkdir -p gen/src gen/include obj
for spec in "$P1 1" "$D1 1" "$C1 1" "$M1 1" "$G5 5" "$N2 2" "$F1 1"; do
  set -- $spec
  hash=$(tail -1 "$1/.hash")
  find "$1" -name '*.aidl' | while read -r f; do
    ./build-tools/aidl --lang=ndk --structured --stability=vintf --version "$2" --hash "$hash" $inc -h gen/include -o gen/src "$f"
  done
done
INC="-Igen/include -Inative/libs/binder/ndk/include_cpp -Inative/libs/binder/ndk/include_platform -Inative/libs/binder/ndk/include_ndk \
  -Inative/libs/nativewindow/include -Icore/libcutils/include -Imedia/camera/include -Imedia/camera/include/system \
  -Imedia/private/camera/include -Isrc -Isrc/include"
"$TC/x86_64-linux-android$API-clang" -O2 -fPIC $INC -include android/log.h -c media/camera/src/camera_metadata.c -o obj/camera_metadata.o
i=0
for f in $(find gen/src -name '*.cpp'); do
  i=$((i + 1))
  "$TC/x86_64-linux-android$API-clang++" -std=c++17 -O2 -fPIC -D__ANDROID_VENDOR__ $INC -c "$f" -o "obj/gen_$i.o"
done
"$TC/x86_64-linux-android$API-clang++" -std=c++20 -O2 -fPIC -D__ANDROID_VENDOR__ $INC -c src/hal.cpp -o obj/hal.o
mkdir -p /out/bin/hw
"$TC/x86_64-linux-android$API-clang++" -static-libstdc++ -fuse-ld=lld -Wl,--no-undefined -Wl,--allow-shlib-undefined \
  -o /out/bin/hw/android.hardware.camera.provider-holtdroid obj/*.o \
  lib/libbinder_ndk.so lib/libnativewindow.so lib/liblog.so
"$TC/llvm-strip" /out/bin/hw/android.hardware.camera.provider-holtdroid
