# holtdroid-redroid

[redroid](https://github.com/remote-android/redroid-doc) 15 (Android in a
container) prepared for [Holtdroid](https://github.com/holtdroid/holtdroid)
sessions:

- **Video encoders without DMA heaps.** Stock redroid disables Codec2 unless
  the kernel has `/dev/dma_heap/system` or `/dev/ion`, so scrcpy finds no
  encoder. This image always enables Codec2, allocates linear blocks as
  gralloc BLOBs (`debug.stagefright.c2-poolmask=851968`) and uses the
  minigbm gralloc (`cros`), because the default `gbm` one crashes on BLOBs.
  Encoders are software (`c2.android.avc.encoder`, HEVC, AV1).
- **ARM64 translation.** `libndk_translation` (with berberis) from the
  Android 15 emulator system image (`x86_64-35_r09`), so APKs with only
  `arm64-v8a` libraries run. Executing ARM binaries as separate processes
  (`ro.enable.native.bridge.exec`) is off: binfmt_misc is not namespaced and
  registering it would change the host.
- **Privileged pods in Kubernetes.** The entrypoint
  `/holtdroid-redroid-init` moves itself into a child cgroup, unshares the
  cgroup namespace and remounts `/sys/fs/cgroup` before it execs `/init`.
  Without it Android's init can't create process groups and netd can't
  attach its BPF programs next to systemd's. Without a host cgroup
  namespace (plain `docker run`) it does nothing.

The image is `linux/amd64` only and published as
`ghcr.io/holtdroid/holtdroid-redroid:<commit sha>` and `:main`.

## Running it

The host kernel needs binder (`CONFIG_ANDROID_BINDERFS`); ashmem isn't
needed (`androidboot.use_memfd=true`, the default arguments). For GPU
rendering pass the render node:

```sh
docker run -d --privileged -p 5555:5555 ghcr.io/holtdroid/holtdroid-redroid:main \
  qemu=1 androidboot.hardware=redroid androidboot.use_memfd=true \
  androidboot.redroid_gpu_mode=host androidboot.redroid_gpu_node=/dev/dri/renderD128 \
  androidboot.redroid_width=720 androidboot.redroid_height=1600 \
  androidboot.redroid_dpi=320 androidboot.redroid_fps=60
```

In Kubernetes (containerd) the Pod also needs
`securityContext.supplementalGroupsPolicy: Strict` and the container
`runAsUser: 0`, `runAsGroup: 0` and `privileged: true`: Android's `/etc` is
an absolute symlink, which containerd refuses to resolve when it adds
supplemental groups from `/etc/group`. Mount an `emptyDir` (or a volume) on
`/data`.

## Known limits

- No WebGPU in WebView: Chromium reports `Vulkan not supported with in
  process gpu` (the same on the Android emulator). WebGL 1 and 2 run on the
  host GPU through Mesa.
- No hardware video encoders; a constantly changing 720p screen costs about
  one CPU core to encode.
- Android 16 needs ashmem, which mainline kernels no longer have.

## Licenses

The build scripts are Apache-2.0. The image contains redroid (Apache-2.0,
AOSP) and `libndk_translation` from the Android emulator system image,
which is distributed under the Android Software Development Kit License
Agreement.
