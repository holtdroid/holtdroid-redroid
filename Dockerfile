# redroid 15 for Holtdroid sessions: software video encoders without DMA
# heaps, ARM64 translation from the Android 15 emulator system image, and an
# init wrapper for privileged pods that share the host's cgroup namespace.
# linux/amd64 only.

ARG REDROID_IMAGE=docker.io/redroid/redroid:15.0.0_64only-latest@sha256:b51bde9cef80f7bd7581148192f2b2f4d41f23c6344cfe88eceeb8ddd67490ee
ARG GO_IMAGE=docker.io/library/golang:1.27.2-bookworm@sha256:5cf287a799e6b94384bad13d16b14904c531f51ba65792237e122ce42b392f61
ARG DEBIAN_IMAGE=docker.io/library/debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f

FROM ${REDROID_IMAGE} AS redroid

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS ndk
ARG SYSTEM_IMAGE_ZIP=x86_64-35_r09.zip
ARG SYSTEM_IMAGE_SHA1=0103e6dab21290c4b9d16550a3ce99476f884eef
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
    } > cmds; \
    debugfs -f cmds system.ext4; \
    rm system.ext4; \
    test -f /out/system/lib64/libndk_translation.so; \
    test -f /out/system/lib64/libberberis_exec_region.so; \
    test -f /out/system/lib64/arm64/libc.so; \
    test -f /out/system/bin/arm64/linker64

FROM --platform=$BUILDPLATFORM ${GO_IMAGE} AS init
WORKDIR /src
COPY go.mod ./
COPY cmd ./cmd
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags=-s -o /out/holtdroid-redroid-init ./cmd/holtdroid-redroid-init

FROM --platform=$BUILDPLATFORM ${DEBIAN_IMAGE} AS vendor
COPY --from=redroid /vendor/bin/gpu_config.sh /vendor/build.prop /in/
RUN set -eux; \
    mkdir -p /out/vendor/bin; \
    grep -q '^    setprop ro.hardware.gralloc gbm$' /in/gpu_config.sh; \
    sed 's/^    setprop ro.hardware.gralloc gbm$/    setprop ro.hardware.gralloc cros/' /in/gpu_config.sh > /out/vendor/bin/gpu_config.sh; \
    grep -qx 'ro.dalvik.vm.native.bridge=libnb.so' /in/build.prop; \
    grep -qx 'ro.enable.native.bridge.exec=1' /in/build.prop; \
    sed -e 's/^ro.dalvik.vm.native.bridge=libnb.so$/ro.dalvik.vm.native.bridge=libndk_translation.so/' \
        -e 's/^ro.enable.native.bridge.exec=1$/ro.enable.native.bridge.exec=0/' /in/build.prop > /out/vendor/build.prop; \
    echo 'ro.ndk_translation.version=0.2.3' >> /out/vendor/build.prop

FROM ${REDROID_IMAGE}
COPY --from=ndk /out/ /
COPY --from=vendor --chown=0:2000 --chmod=755 /out/vendor/bin/gpu_config.sh /vendor/bin/gpu_config.sh
COPY --from=vendor --chown=0:0 --chmod=600 /out/vendor/build.prop /vendor/build.prop
COPY --chown=0:2000 --chmod=755 vendor/bin/redroid.c2.sh /vendor/bin/redroid.c2.sh
COPY --from=init /out/holtdroid-redroid-init /holtdroid-redroid-init
ENTRYPOINT ["/holtdroid-redroid-init"]
CMD ["qemu=1", "androidboot.hardware=redroid", "androidboot.redroid_gpu_mode=host", "androidboot.use_memfd=true"]
