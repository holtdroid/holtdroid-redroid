# holtdroid-redroid

[redroid](https://github.com/remote-android/redroid-doc) (Android in a
container) prepared for [Holtdroid](https://github.com/holtdroid/holtdroid)
sessions, one image per Android version:

| Tag | redroid base | ARM64 translation from |
|-----|--------------|------------------------|
| `15` | `redroid/redroid:15.0.0_64only-latest` | emulator image `x86_64-35_r09` (`libndk_translation` + berberis) |
| `14` | `redroid/redroid:14.0.0_64only-latest` | `x86_64-34_r14` (`libndk_translation`) |
| `13` | `redroid/redroid:13.0.0_64only-latest` | `x86_64-33_r15` (`libndk_translation`; r16 and later ship no translation) |
| `12` | `redroid/redroid:12.0.0_64only-latest` | `x86_64-32_r08` (`libndk_translation`) |
| `11` | `redroid/redroid:11.0.0-latest` (32-bit ABIs included) | `x86_64-30_r16` (`libndk_translation`; the image has no `ro.ndk_translation.version`) |

Each is published as `ghcr.io/holtdroid/holtdroid-redroid:<version>` and
`:<version>-<commit sha>`; `:main` is the same as `:15`. The pins (base image
digest, system image zip and SHA-1) live in `versions/<version>.env` and are
passed to the Dockerfile as build args: `make image ANDROID=14`.

What the image changes on top of redroid:

- **adbd runs as root from boot** (`service.adb.root=1` in the vendor
  build.prop), so Holtdroid's `adb root` is a no-op instead of a restart.
  On Android 11 the restart crash-looped on `Could not install *smartsocket*
  listener: Address already in use` (the old listener's TIME_WAIT; that
  adbd sets no `SO_REUSEADDR`).

- **Video encoders without DMA heaps.** Stock redroid disables Codec2 unless
  the kernel has `/dev/dma_heap/system` or `/dev/ion`, so scrcpy finds no
  encoder. This image always enables Codec2, allocates linear blocks as
  gralloc BLOBs (`debug.stagefright.c2-poolmask=851968`) and uses the
  minigbm gralloc (`cros`), because the default `gbm` one crashes on BLOBs.
  Encoders are software (`c2.android.avc.encoder`, HEVC, AV1).
- **ARM64 translation.** `libndk_translation` from the Google APIs emulator
  system image of the same API level, so APKs with only `arm64-v8a`
  libraries run. Executing ARM binaries as separate processes
  (`ro.enable.native.bridge.exec`) is off: binfmt_misc is not namespaced and
  registering it would change the host.
- **Privileged pods in Kubernetes.** The entrypoint
  `/holtdroid-redroid-init` runs before Android's `/init`:
  - it moves itself into a child cgroup, unshares the cgroup namespace and
    remounts `/sys/fs/cgroup`. Without it Android's init can't create
    process groups and netd can't attach its BPF programs next to systemd's.
    Without a host cgroup namespace (plain `docker run`) this step does
    nothing;
  - it keeps Android off the node's sysctls. A privileged container writes
    straight into the host's `/proc/sys`, and Android's init.rc disables
    `kernel.modprobe` and `kernel.sysrq`, lowers `vm.mmap_min_addr`, retunes
    hung-task, scheduler and VM watermark settings, while `netbpfload` sets
    `kernel.unprivileged_bpf_disabled` to 0 and reboots Android when that
    write fails. The build strips every `write /proc/sys/...` outside
    `/proc/sys/net` from `init.rc` and `redroid.common.rc` (the `rc` stage,
    guarded by greps), and the wrapper forks a guard that waits for init's
    own procfs mount on `/proc` and bind-mounts a plain file over each
    remaining target (`unprivileged_bpf_disabled`, `modprobe`, `sysrq`, the
    hung-task, `sched_schedstats`, `perf_event_paranoid` and watermark
    sysctls, `mmap_min_addr`), so the binaries' writes land in a file instead
    of the kernel. A read-only bind under the wrapper's `/proc/sys` would be
    hidden by that fresh procfs, and a read-only `/proc/sys/kernel` makes
    init abort: it insists on raising `kptr_restrict` (and `mmap_rnd_bits`),
    the two sysctls that stay real because raising them only hardens the
    node. Verified on a k3s node by diffing `sysctl -a` around a boot: only
    `kernel.kptr_restrict` 1→2 remains.

The image is `linux/amd64` only.

## Running it

The host kernel needs binder (`CONFIG_ANDROID_BINDERFS`); ashmem isn't
needed (`androidboot.use_memfd=true`, the default arguments). For GPU
rendering pass the render node:

```sh
docker run -d --privileged -p 5555:5555 ghcr.io/holtdroid/holtdroid-redroid:15 \
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
