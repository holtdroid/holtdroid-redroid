# redroid for Holtdroid sessions: software video encoders without DMA heaps,
# ARM64 translation from the matching Android emulator system image, and an
# init wrapper for privileged pods, and the built-in WebView re-signed with
# Chromium's public debug key so Chromium snapshot builds install over it.
# One build per Android version, pinned in
# versions/<version>.env. linux/amd64 only.

ARG REDROID_IMAGE
ARG GO_IMAGE=docker.io/library/golang:1.27.2-bookworm@sha256:5cf287a799e6b94384bad13d16b14904c531f51ba65792237e122ce42b392f61
ARG DEBIAN_IMAGE=docker.io/library/debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f

FROM ${REDROID_IMAGE} AS redroid

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS ndk
ARG SYSTEM_IMAGE_ZIP
ARG SYSTEM_IMAGE_SHA1
ARG BERBERIS=0
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl unzip python3 e2fsprogs && rm -rf /var/lib/apt/lists/*
COPY build/lpunpack.py /usr/local/bin/lpunpack
WORKDIR /dl
RUN set -eux; \
    curl -fsSLo sys.zip "https://dl.google.com/android/repository/sys-img/google_apis/$SYSTEM_IMAGE_ZIP"; \
    echo "$SYSTEM_IMAGE_SHA1  sys.zip" | sha1sum -c -; \
    unzip -q sys.zip x86_64/system.img; \
    rm sys.zip; \
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

FROM --platform=$BUILDPLATFORM ${GO_IMAGE} AS init
WORKDIR /src
COPY go.mod ./
COPY cmd ./cmd
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags=-s -o /out/holtdroid-redroid-init ./cmd/holtdroid-redroid-init

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS vendor
COPY --from=redroid /vendor/bin/gpu_config.sh /vendor/build.prop /in/
COPY --from=ndk /ndk_translation.prop /in/
RUN set -eux; \
    mkdir -p /out/vendor/bin; \
    grep -q '^    setprop ro.hardware.gralloc gbm$' /in/gpu_config.sh; \
    sed 's/^    setprop ro.hardware.gralloc gbm$/    setprop ro.hardware.gralloc cros/' /in/gpu_config.sh > /out/vendor/bin/gpu_config.sh; \
    grep -qx 'ro.dalvik.vm.native.bridge=libnb.so' /in/build.prop; \
    grep -qx 'ro.enable.native.bridge.exec=1' /in/build.prop; \
    grep -qx 'ro.dalvik.vm.isa.arm64=x86_64' /in/build.prop; \
    sed -e 's/^ro.dalvik.vm.native.bridge=libnb.so$/ro.dalvik.vm.native.bridge=libndk_translation.so/' \
        -e 's/^ro.enable.native.bridge.exec=1$/ro.enable.native.bridge.exec=0/' /in/build.prop > /out/vendor/build.prop; \
    cat /in/ndk_translation.prop >> /out/vendor/build.prop; \
    echo 'service.adb.root=1' >> /out/vendor/build.prop

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

FROM ${REDROID_IMAGE}
COPY --from=ndk /out/ /
COPY --from=rc --chown=0:0 --chmod=644 /out/init.rc /system/etc/init/hw/init.rc
COPY --from=rc --chown=0:0 --chmod=644 /out/redroid.common.rc /vendor/etc/init/redroid.common.rc
COPY --from=vendor --chown=0:2000 --chmod=755 /out/vendor/bin/gpu_config.sh /vendor/bin/gpu_config.sh
COPY --from=vendor --chown=0:0 --chmod=600 /out/vendor/build.prop /vendor/build.prop
COPY --chown=0:2000 --chmod=755 vendor/bin/redroid.c2.sh /vendor/bin/redroid.c2.sh
COPY --from=webview --chown=0:0 --chmod=644 /out/webview.apk /system/product/app/webview/webview.apk
COPY --from=init /out/holtdroid-redroid-init /holtdroid-redroid-init
ENTRYPOINT ["/holtdroid-redroid-init"]
CMD ["qemu=1", "androidboot.hardware=redroid", "androidboot.redroid_gpu_mode=host", "androidboot.use_memfd=true"]
