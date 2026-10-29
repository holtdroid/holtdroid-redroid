# redroid for Holtdroid sessions: software video encoders without DMA heaps,
# ARM64 translation from the matching Android emulator system image, and an
# init wrapper for privileged pods, the built-in WebView re-signed with
# Chromium's public debug key so Chromium snapshot builds install over it,
# and (VULKAN=mesa) newer Mesa Vulkan drivers built without android-strict.
# One build per Android version, pinned in
# versions/<version>.env. linux/amd64 only.

ARG REDROID_IMAGE
ARG VULKAN=stock
ARG BLUETOOTH=on
ARG CAMERA=off
ARG KERNEL_COMPAT=off
ARG GO_IMAGE=docker.io/library/golang:1.27.2-bookworm@sha256:5cf287a799e6b94384bad13d16b14904c531f51ba65792237e122ce42b392f61
ARG ALPINE_IMAGE=docker.io/library/alpine:3.22@sha256:2e0b81d60597f449baa56dc25d355367869d2dca4cd57e866f1e2b52af3eb55f
ARG DEBIAN_IMAGE=docker.io/library/debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f

FROM ${REDROID_IMAGE} AS redroid

FROM ${DEBIAN_IMAGE} AS mesa-build
ARG MESA_VERSION=26.2.5
ARG MESA_SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5
ARG NDK=r28c
ARG NDK_SHA1=a7b54a5de87fecd125a17d54f73c446199e72a64
ARG API=30
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl unzip xz-utils python3 python3-mako python3-yaml python3-packaging python3-ply \
      gcc g++ meson ninja-build pkgconf glslang-tools bison flex \
      llvm-19-dev libclang-19-dev clang-19 libllvmspirvlib-19-dev spirv-tools libdrm-dev zlib1g-dev libexpat1-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /w
RUN set -eux; \
    curl -fsSLo mesa.tar.xz "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz"; \
    echo "$MESA_SHA256  mesa.tar.xz" | sha256sum -c -; \
    mkdir mesa; tar -xJf mesa.tar.xz -C mesa --strip-components=1; rm mesa.tar.xz; \
    curl -fsSLo ndk.zip "https://dl.google.com/android/repository/android-ndk-$NDK-linux.zip"; \
    echo "$NDK_SHA1  ndk.zip" | sha1sum -c -; \
    unzip -q ndk.zip; mv "android-ndk-$NDK" ndk; rm ndk.zip
RUN set -eux; \
    meson setup native mesa --prefix=/opt/mesa-native -Dbuildtype=release \
      -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=intel -Dopengl=false -Degl=disabled -Dgbm=disabled -Dglx=disabled \
      -Dgles1=disabled -Dgles2=disabled -Dllvm=enabled -Dshared-llvm=enabled \
      -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
      -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled; \
    ninja -C native; \
    meson install -C native; \
    rm -rf native
COPY build/android-x86_64.ini /w/
RUN set -eux; \
    export PATH=/opt/mesa-native/bin:$PATH; \
    sed -i "s/@API@/$API/g" android-x86_64.ini; \
    meson setup cross mesa --cross-file android-x86_64.ini -Dbuildtype=release -Dstrip=true -Ddefault_library=static \
      -Dplatforms=android -Dplatform-sdk-version=$API -Dandroid-stub=true -Dandroid-libbacktrace=disabled -Dandroid-strict=false \
      -Dgallium-drivers= -Dvulkan-drivers=amd,intel -Dopengl=false -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dgles1=disabled -Dgles2=disabled \
      -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dallow-fallback-for=libdrm \
      -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dvideo-codecs=; \
    ninja -C cross src/amd/vulkan/libvulkan_radeon.so src/intel/vulkan/libvulkan_intel.so; \
    mkdir /out; \
    cp cross/src/amd/vulkan/libvulkan_radeon.so /out/vulkan.radeon.so; \
    cp cross/src/intel/vulkan/libvulkan_intel.so /out/vulkan.intel.so; \
    for f in /out/*.so; do \
      if ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf -d "$f" | grep NEEDED | grep -v -E '\[lib(c|m|dl|z|log|sync|hardware|nativewindow)\.so\]'; then exit 1; fi; \
    done

FROM scratch AS vulkan-mesa
COPY --from=mesa-build /out/ /

FROM scratch AS vulkan-stock
COPY --from=redroid /vendor/lib64/hw/vulkan.radeon.so /vendor/lib64/hw/vulkan.intel.so /

FROM vulkan-${VULKAN} AS vulkan

FROM ${DEBIAN_IMAGE} AS camera-build
ARG NDK=r28c
ARG NDK_SHA1=a7b54a5de87fecd125a17d54f73c446199e72a64
ARG BUILD_TOOLS_ZIP=build-tools_r35_linux.zip
ARG BUILD_TOOLS_SHA256=bd3a4966912eb8b30ed0d00b0cda6b6543b949d5ffe00bea54c04c81e1561d88
ARG AOSP_TAG=android-15.0.0_r1
ARG INTERFACES_COMMIT=488942f82bd1bc9ad1cb65a02c71421dc3a6a3d6
ARG MEDIA_COMMIT=0287f16fc74c607c04a9570ee3ae914e1285cd2c
ARG NATIVE_COMMIT=f7274fca5e36082674740bc6c976f73c4578d009
ARG CORE_COMMIT=fc7bc8c4bfb4c6095e34ea784509dae56f25b486
ARG STB_COMMIT=003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
ARG STB_SHA256=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl unzip git && rm -rf /var/lib/apt/lists/*
WORKDIR /w
RUN set -eux; \
    curl -fsSLo ndk.zip "https://dl.google.com/android/repository/android-ndk-$NDK-linux.zip"; \
    echo "$NDK_SHA1  ndk.zip" | sha1sum -c -; \
    unzip -q ndk.zip; mv "android-ndk-$NDK" ndk; rm ndk.zip; \
    curl -fsSLo bt.zip "https://dl.google.com/android/repository/$BUILD_TOOLS_ZIP"; \
    echo "$BUILD_TOOLS_SHA256  bt.zip" | sha256sum -c -; \
    unzip -q bt.zip 'android-15/aidl' 'android-15/lib64/*'; mv android-15 build-tools; rm bt.zip; \
    git clone -q --depth 1 --filter=blob:none --sparse --branch "$AOSP_TAG" https://android.googlesource.com/platform/hardware/interfaces hardware-interfaces; \
    test "$(git -C hardware-interfaces rev-parse HEAD)" = "$INTERFACES_COMMIT"; \
    git -C hardware-interfaces sparse-checkout set camera/provider/aidl camera/device/aidl camera/common/aidl camera/metadata/aidl \
      graphics/common/aidl common/aidl common/fmq/aidl; \
    git clone -q --depth 1 --filter=blob:none --sparse --branch "$AOSP_TAG" https://android.googlesource.com/platform/system/media media; \
    test "$(git -C media rev-parse HEAD)" = "$MEDIA_COMMIT"; \
    git -C media sparse-checkout set camera private/camera; \
    git clone -q --depth 1 --filter=blob:none --sparse --branch "$AOSP_TAG" https://android.googlesource.com/platform/frameworks/native native; \
    test "$(git -C native rev-parse HEAD)" = "$NATIVE_COMMIT"; \
    git -C native sparse-checkout set libs/binder/ndk/include_cpp libs/binder/ndk/include_platform libs/binder/ndk/include_ndk libs/nativewindow/include; \
    git clone -q --depth 1 --filter=blob:none --sparse --branch "$AOSP_TAG" https://android.googlesource.com/platform/system/core core; \
    test "$(git -C core rev-parse HEAD)" = "$CORE_COMMIT"; \
    git -C core sparse-checkout set libcutils/include/cutils; \
    mkdir -p src; \
    curl -fsSLo src/stb_image_write.h "https://raw.githubusercontent.com/nothings/stb/$STB_COMMIT/stb_image_write.h"; \
    echo "$STB_SHA256  src/stb_image_write.h" | sha256sum -c -
COPY --from=redroid /system/lib64/libbinder_ndk.so /system/lib64/libnativewindow.so /system/lib64/liblog.so /w/lib/
COPY build/camera/hal.cpp /w/src/
COPY build/camera/include/ /w/src/include/
COPY build/camera/build.sh /w/
RUN sh /w/build.sh

FROM scratch AS camera-on
COPY --from=camera-build /out/bin/ /bin/
COPY build/camera/android.hardware.camera.provider-holtdroid.rc /etc/init/
COPY build/camera/android.hardware.camera.provider-holtdroid.xml /etc/vintf/manifest/
COPY build/camera/android.hardware.camera.xml /etc/permissions/

FROM scratch AS camera-off

FROM camera-${CAMERA} AS camera

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS bluetooth-off-build
COPY --from=redroid /vendor/etc/permissions/handheld_core_hardware.xml /in/
RUN set -eux; \
    mkdir /out; \
    grep -qx '    <feature name="android.hardware.bluetooth" />' /in/handheld_core_hardware.xml; \
    grep -vx '    <feature name="android.hardware.bluetooth" />' /in/handheld_core_hardware.xml > /out/handheld_core_hardware.xml; \
    if grep -q 'android.hardware.bluetooth' /out/handheld_core_hardware.xml; then exit 1; fi

FROM scratch AS bluetooth-off
COPY --from=bluetooth-off-build /out/ /

FROM scratch AS bluetooth-on

FROM bluetooth-${BLUETOOTH} AS bluetooth

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS ndk
ARG SYSTEM_IMAGE_ZIP
ARG SYSTEM_IMAGE_SHA1
ARG BERBERIS=0
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl unzip python3 e2fsprogs fdisk && rm -rf /var/lib/apt/lists/*
COPY build/lpunpack.py /usr/local/bin/lpunpack
WORKDIR /dl
RUN set -eux; \
    curl -fsSLo sys.zip "https://dl.google.com/android/repository/sys-img/google_apis/$SYSTEM_IMAGE_ZIP"; \
    echo "$SYSTEM_IMAGE_SHA1  sys.zip" | sha1sum -c -; \
    unzip -q sys.zip x86_64/system.img x86_64/vendor.img; \
    rm sys.zip; \
    start=$(sfdisk -d x86_64/vendor.img | sed -n 's/.*start= *\([0-9]*\).*/\1/p' | head -1); \
    dd if=x86_64/vendor.img of=vendor.ext4 bs=512 skip="$start" status=none; \
    rm x86_64/vendor.img; \
    mkdir -p /emu/lib64 /emulib; \
    for f in bin/hw/android.hardware.wifi@1.0-service bin/hw/android.hardware.wifi-service bin/hw/wpa_supplicant bin/mac80211_create_radios \
      etc/init/android.hardware.wifi@1.0-service.rc etc/init/android.hardware.wifi-service.rc etc/init/hw/init.ranchu.rc etc/wifi/wpa_supplicant.conf \
      etc/vintf/manifest/android.hardware.wifi@1.0-service.xml etc/vintf/manifest/android.hardware.wifi-service.xml \
      etc/vintf/manifest/android.hardware.wifi.supplicant.xml etc/vintf/manifest/manifest.xml lib64/libwifi-hal.so; do \
      mkdir -p "/emu/$(dirname $f)"; echo "dump -p /$f /emu/$f"; \
    done > cmds; \
    echo "rdump /lib64 /emulib" >> cmds; \
    debugfs -f cmds vendor.ext4 >/dev/null 2>&1; \
    rm vendor.ext4 cmds; \
    test -f /emu/bin/hw/wpa_supplicant; \
    test -f /emu/lib64/libwifi-hal.so; \
    test -f /emulib/lib64/libwifi-hal.so; \
    lpunpack x86_64/system.img system system.ext4; \
    rm -r x86_64; \
    mkdir -p /out/system/lib64 /out/system/bin /out/system/etc; \
    libs=$(debugfs -R 'ls -p /system/lib64' system.ext4 2>/dev/null | awk -F/ '$6 ~ /^lib(ndk_translation|berberis).*\.so$/ {print $6}'); \
    test -n "$libs"; \
    { for l in $libs; do echo "dump -p /system/lib64/$l /out/system/lib64/$l"; done; \
      echo "rdump /system/lib64/arm64 /out/system/lib64"; \
      echo "rdump /system/bin/arm64 /out/system/bin"; \
      echo "dump -p /system/etc/ld.config.arm64.txt /out/system/etc/ld.config.arm64.txt"; \
      echo "cat /system/build.prop"; \
    } > cmds; \
    debugfs -f cmds system.ext4 | { grep '^ro.ndk_translation.version=' || true; } > /ndk_translation.prop; \
    rm system.ext4; \
    test -f /out/system/lib64/libndk_translation.so; \
    [ "$BERBERIS" != 1 ] || test -f /out/system/lib64/libberberis_exec_region.so; \
    [ "$BERBERIS" = 1 ] || ! ls /out/system/lib64/libberberis* 2>/dev/null; \
    test -f /out/system/lib64/arm64/libc.so; \
    test -f /out/system/bin/arm64/linker64

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS wifi
RUN apt-get update && apt-get install -y --no-install-recommends binutils && rm -rf /var/lib/apt/lists/*
COPY --from=ndk /emu/ /emu/
COPY --from=ndk /emulib/lib64/ /emu/lib64/
COPY --from=redroid /vendor/lib64/ /redroid/lib64/
COPY vendor/etc/init/holtdroid-wifi.rc /in/
RUN set -eux; \
    mkdir -p /out/vendor/lib64; \
    cd /emu; \
    for f in bin/hw/android.hardware.wifi@1.0-service bin/hw/android.hardware.wifi-service bin/hw/wpa_supplicant bin/mac80211_create_radios \
      etc/init/android.hardware.wifi@1.0-service.rc etc/init/android.hardware.wifi-service.rc etc/wifi/wpa_supplicant.conf \
      etc/vintf/manifest/android.hardware.wifi@1.0-service.xml etc/vintf/manifest/android.hardware.wifi-service.xml \
      etc/vintf/manifest/android.hardware.wifi.supplicant.xml lib64/libwifi-hal.so; do \
      [ ! -f "$f" ] || { mkdir -p "/out/vendor/$(dirname $f)"; cp "$f" "/out/vendor/$f"; }; \
    done; \
    if [ ! -f etc/vintf/manifest/android.hardware.wifi.supplicant.xml ]; then \
      grep -q '<name>android.hardware.wifi.supplicant</name>' etc/vintf/manifest/manifest.xml; \
      cp etc/vintf/manifest/manifest.xml /out/vendor/etc/vintf/manifest/android.hardware.wifi.supplicant.xml; \
    fi; \
    test -f /out/vendor/bin/hw/android.hardware.wifi@1.0-service -o -f /out/vendor/bin/hw/android.hardware.wifi-service; \
    added=1; while [ $added = 1 ]; do added=0; \
      for b in $(find /out/vendor -type f \( -path '*/bin/*' -o -name '*.so' \)); do \
        for l in $(readelf -d "$b" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do \
          [ ! -e "/redroid/lib64/$l" ] && [ ! -e "/out/vendor/lib64/$l" ] && [ -e "lib64/$l" ] || continue; \
          cp "lib64/$l" /out/vendor/lib64/; added=1; \
        done; \
      done; \
    done; \
    mkdir -p /out/vendor/etc/init; \
    { cat /in/holtdroid-wifi.rc; echo; \
      awk '/^service wpa_supplicant /{p=1} p&&/^$/{exit} p' etc/init/hw/init.ranchu.rc; \
    } > /out/vendor/etc/init/holtdroid-wifi.rc; \
    grep -q '^service wpa_supplicant /vendor/bin/hw/wpa_supplicant ' /out/vendor/etc/init/holtdroid-wifi.rc; \
    grep -q 'socket wpa_wlan0' /out/vendor/etc/init/holtdroid-wifi.rc; \
    chmod 755 /out/vendor/bin/* /out/vendor/bin/hw/*; \
    find /out/vendor -type f ! -path '*/bin/*' -exec chmod 644 {} +

FROM ${ALPINE_IMAGE} AS net
RUN set -eux; \
    apk add --no-cache hostapd nftables; \
    mkdir -p /out/lib; \
    for b in /usr/sbin/hostapd /usr/sbin/nft; do \
      cp "$b" /out/; \
      for l in $(ldd "$b" | awk '/=>/ {print $3}'); do cp -L "$l" /out/lib/; done; \
    done; \
    cp -L /lib/ld-musl-x86_64.so.1 /out/lib/

FROM ${DEBIAN_IMAGE} AS rro
ARG AAPT2_VERSION=8.13.2-14304508
ARG AAPT2_SHA256=839609d6d776d6dd60a02aa577d97193ce3e650cf1deaabf062321e23bbd6bf6
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl unzip e2fsprogs apksigner && rm -rf /var/lib/apt/lists/*
WORKDIR /w
RUN set -eux; \
    curl -fsSLo aapt2.jar "https://dl.google.com/android/maven2/com/android/tools/build/aapt2/$AAPT2_VERSION/aapt2-$AAPT2_VERSION-linux.jar"; \
    echo "$AAPT2_SHA256  aapt2.jar" | sha256sum -c -; \
    unzip -q aapt2.jar aapt2 -d /usr/local/bin; \
    rm aapt2.jar; \
    chmod 755 /usr/local/bin/aapt2
COPY --from=redroid /system/framework/framework-res.apk /in/
COPY --from=redroid /system/apex/com.android.tethering* /in/tethering/
COPY build/rro/ /rro/
RUN set -eux; \
    keytool -genkeypair -keystore k.jks -storepass holtdroid -keypass holtdroid -alias rro -dname CN=holtdroid-rro -keyalg RSA -validity 36500 >/dev/null; \
    build() { \
      aapt2 compile --dir "${4:-/rro/res}" -o res.zip; \
      aapt2 link -o unsigned.apk --manifest "$1" -I /in/framework-res.apk $3 res.zip; \
      mkdir -p "/out/system/product/overlay/$2"; \
      apksigner sign --v4-signing-enabled false --ks k.jks --ks-pass pass:holtdroid --out "/out/system/product/overlay/$2/$2.apk" unsigned.apk; \
      rm res.zip unsigned.apk; \
    }; \
    build /rro/android.xml HoltdroidNoEthernet ""; \
    aapt2 dump resources /in/framework-res.apk | grep -q 'xml/config_webview_packages'; \
    build /rro/webview.xml HoltdroidWebView "" /rro/webview-res; \
    build /rro/wallpaper.xml HoltdroidWallpaper "" /rro/wallpaper-res; \
    build /rro/launcher.xml HoltdroidLauncher "" /rro/launcher-res; \
    scr=$(find /in/tethering -path '*/priv-app/ServiceConnectivityResources*/*.apk' | head -1); \
    if [ -z "$scr" ] && ls /in/tethering/*.apex >/dev/null 2>&1; then \
      unzip -q /in/tethering/*.apex apex_payload.img; \
      dir=$(debugfs -R 'ls /priv-app' apex_payload.img 2>/dev/null | tr -s ' ' '\n' | grep '^ServiceConnectivityResources' | head -1); \
      debugfs -R "dump /priv-app/$dir/ServiceConnectivityResources.apk scr.apk" apex_payload.img; \
      rm apex_payload.img; scr=scr.apk; \
    fi; \
    if [ -n "$scr" ] && aapt2 dump resources "$scr" | grep -q 'string/config_ethernet_iface_regex'; then \
      aapt2 dump overlayable "$scr" | grep -q 'name="ServiceConnectivityResourcesConfig"'; \
      build /rro/connectivity.xml HoltdroidNoEthernetConnectivity "-I $scr"; \
    fi; \
    find /out -name '*.apk' -exec chmod 644 {} +

FROM --platform=$BUILDPLATFORM ${GO_IMAGE} AS init
WORKDIR /src
COPY go.mod ./
COPY cmd ./cmd
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags=-s -o /out/holtdroid-redroid-init ./cmd/holtdroid-redroid-init

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS vendor
ARG GRALLOC=cros
ARG CODEC2=on
ARG CAMERA=off
ARG CAMERA_LAZY=off
COPY --from=redroid /vendor/bin/gpu_config.sh /vendor/build.prop /in/
COPY --from=redroid /odm/etc/build.prop /in/odm.prop
COPY --from=ndk /ndk_translation.prop /in/
RUN set -eux; \
    mkdir -p /out/vendor/bin; \
    grep -q '^    setprop ro.hardware.gralloc gbm$' /in/gpu_config.sh; \
    sed "s/^    setprop ro.hardware.gralloc gbm\$/    setprop ro.hardware.gralloc $GRALLOC/" /in/gpu_config.sh > /out/vendor/bin/gpu_config.sh; \
    grep -q "^    setprop ro.hardware.gralloc $GRALLOC\$" /out/vendor/bin/gpu_config.sh; \
    grep -qx 'ro.dalvik.vm.native.bridge=libnb.so' /in/build.prop; \
    grep -qx 'ro.enable.native.bridge.exec=1' /in/build.prop; \
    grep -qx 'ro.dalvik.vm.isa.arm64=x86_64' /in/build.prop; \
    sed -e 's/^ro.dalvik.vm.native.bridge=libnb.so$/ro.dalvik.vm.native.bridge=libndk_translation.so/' \
        -e 's/^ro.enable.native.bridge.exec=1$/ro.enable.native.bridge.exec=0/' \
        -e 's/^\(ro.vendor.product.cpu.abilist\(32\)\?=.*\),armeabi-v7a,armeabi$/\1/' \
        -e '/^ro.dalvik.vm.isa.arm=x86$/d' /in/build.prop > /out/vendor/build.prop; \
    ! grep -E '^ro.vendor.product.cpu.abilist(32)?=.*armeabi|^ro.dalvik.vm.isa.arm=' /out/vendor/build.prop; \
    mkdir -p /out/odm/etc; \
    sed 's/^\(ro.odm.product.cpu.abilist\(32\)\?=.*\),armeabi-v7a,armeabi$/\1/' /in/odm.prop > /out/odm/etc/build.prop; \
    ! grep -E '^ro.odm.product.cpu.abilist(32)?=.*armeabi' /out/odm/etc/build.prop; \
    cat /in/ndk_translation.prop >> /out/vendor/build.prop; \
    if [ "$CODEC2" = on ] && ! grep -qx 'ro.boot.use_redroid_c2=1' /out/vendor/build.prop; then echo 'ro.boot.use_redroid_c2=1' >> /out/vendor/build.prop; fi; \
    echo 'service.adb.root=1' >> /out/vendor/build.prop; \
    [ "$CAMERA" != on ] || [ "$CAMERA_LAZY" != on ] || echo 'ro.camera.enableLazyHal=true' >> /out/vendor/build.prop

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS rc
COPY --from=redroid /system/etc/init/hw/init.rc /vendor/etc/init/redroid.common.rc /in/
RUN set -eux; \
    mkdir -p /out; \
    grep -q '^    write /proc/sys/kernel/sysrq 0$' /in/init.rc; \
    grep -q '^    write /proc/sys/kernel/modprobe' /in/init.rc; \
    grep -q '^    write /proc/sys/vm/mmap_min_addr 32768$' /in/init.rc; \
    grep -q '^    write /proc/sys/kernel/pid_max' /in/redroid.common.rc; \
    for f in init.rc redroid.common.rc; do \
      sed -E '/^\s*write \/proc\/sys\/(kernel|vm|fs|abi|debug|dev|user)\//d' "/in/$f" > "/out/$f"; \
      ! grep -E 'write /proc/sys/' "/out/$f" | grep -v '/proc/sys/net/'; \
    done

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS webview
RUN apt-get update && apt-get install -y --no-install-recommends apksigner && rm -rf /var/lib/apt/lists/*
COPY --from=redroid /system/product/app/webview/webview.apk /in/webview.apk
COPY build/chromium-debug.keystore /in/
RUN set -eux; \
    mkdir -p /out; \
    apksigner sign --ks /in/chromium-debug.keystore --ks-pass pass:chromium --ks-key-alias chromiumdebugkey \
      --out /out/webview.apk /in/webview.apk; \
    apksigner verify --print-certs /out/webview.apk | grep -q 'SHA-256 digest: 32a2fc74d731105859e5a85df16d95f102d85b22099b8064c5d8915c61dad1e0'

FROM ${REDROID_IMAGE} AS assembled
COPY --from=ndk /out/ /
COPY --from=rc --chown=0:0 --chmod=644 /out/init.rc /system/etc/init/hw/init.rc
COPY --from=rc --chown=0:0 --chmod=644 /out/redroid.common.rc /vendor/etc/init/redroid.common.rc
COPY --from=vendor --chown=0:2000 --chmod=755 /out/vendor/bin/gpu_config.sh /vendor/bin/gpu_config.sh
COPY --from=vendor --chown=0:0 --chmod=600 /out/vendor/build.prop /vendor/build.prop
COPY --from=vendor --chown=0:0 --chmod=600 /out/odm/etc/build.prop /odm/etc/build.prop
COPY --chown=0:2000 --chmod=755 vendor/bin/redroid.c2.sh /vendor/bin/redroid.c2.sh
COPY --chown=0:0 --chmod=644 vendor/etc/init/holtdroid-c2.rc /vendor/etc/init/holtdroid-c2.rc
COPY --from=webview --chown=0:0 --chmod=644 /out/webview.apk /system/product/app/webview/webview.apk
COPY --from=vulkan --chown=0:0 --chmod=644 / /vendor/lib64/hw/
COPY --from=bluetooth --chown=0:0 --chmod=644 / /vendor/etc/permissions/
COPY --from=camera --chown=0:0 / /vendor/
COPY --from=wifi --chown=0:0 /out/vendor/ /vendor/
COPY --chown=0:2000 --chmod=755 vendor/bin/holtdroid-wifi.sh /vendor/bin/holtdroid-wifi.sh
COPY --from=net --chown=0:0 /out/ /vendor/holtdroid-net/
COPY --from=rro --chown=0:0 /out/system/ /system/
COPY --from=init /out/holtdroid-redroid-init /holtdroid-redroid-init
ENTRYPOINT ["/holtdroid-redroid-init"]
CMD ["qemu=1", "androidboot.hardware=redroid", "androidboot.redroid_gpu_mode=host", "androidboot.use_memfd=true"]

FROM ${DEBIAN_IMAGE} AS kernel-compat-build
ARG NDK=r28c
ARG NDK_SHA1=a7b54a5de87fecd125a17d54f73c446199e72a64
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl unzip && rm -rf /var/lib/apt/lists/*
WORKDIR /w
RUN set -eux; \
    curl -fsSLo ndk.zip "https://dl.google.com/android/repository/android-ndk-$NDK-linux.zip"; \
    echo "$NDK_SHA1  ndk.zip" | sha1sum -c -; \
    unzip -q ndk.zip; mv "android-ndk-$NDK" ndk; rm ndk.zip
COPY build/kernel-compat/uname.c build/kernel-compat/ashmem.c build/kernel-compat/build.sh /w/
RUN sh /w/build.sh

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS kernel-compat-rc
COPY --from=assembled /system/etc/init/ /in/system/etc/init/
COPY --from=assembled /system/system_ext/etc/init/ /in/system/system_ext/etc/init/
COPY --from=assembled /vendor/etc/init/ /in/vendor/etc/init/
COPY --from=assembled /system/bin/ /bin-in/bin/
COPY build/kernel-compat/holtdroid-netbpfload.rc build/kernel-compat/holtdroid-ashmem.rc /in/system/etc/init/
COPY build/kernel-compat/patch-rc.sh /
RUN sh /patch-rc.sh

FROM scratch AS kernel-compat-on
COPY --from=kernel-compat-build /out/ /
COPY --from=kernel-compat-rc /in/ /
COPY build/kernel-compat/holtdroid-netbpfload.sh /system/bin/

FROM scratch AS kernel-compat-off

FROM kernel-compat-${KERNEL_COMPAT} AS kernel-compat

FROM assembled
COPY --from=kernel-compat --chown=0:0 / /
