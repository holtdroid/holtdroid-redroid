# holtdroid-redroid

[redroid](https://github.com/remote-android/redroid-doc) (Android in a
container) prepared for [Holtdroid](https://github.com/holtdroid/holtdroid)
sessions, one image per Android version:

| Tag | redroid base | ARM64 translation from |
|-----|--------------|------------------------|
| `15` | `redroid/redroid:15.0.0-latest` | emulator image `x86_64-35_r09` (`libndk_translation` + berberis) |
| `14` | `redroid/redroid:14.0.0-latest` | `x86_64-34_r14` (`libndk_translation`) |
| `13` | `redroid/redroid:13.0.0-latest` | `x86_64-33_r15` (`libndk_translation`; r16 and later ship no translation) |
| `12` | `redroid/redroid:12.0.0-latest` | `x86_64-32_r08` (`libndk_translation`) |
| `11` | `redroid/redroid:11.0.0-latest` | `x86_64-30_r16` (`libndk_translation`; the image has no `ro.ndk_translation.version`) |

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
  Android 11 keeps `gbm` (`GRALLOC=gbm` in `versions/11.env`): it has no
  Codec2 video encoders, only `OMX.google.h264.encoder` in a 32-bit
  `media.codec`, which can't lock screen buffers through `cros`, so scrcpy
  got its codec header and then no frames.
  Encoders are software (`c2.android.avc.encoder`, HEVC, AV1). redroid's
  `_64only` bases switch Codec2 on in their vendor `build.prop`
  (`ro.boot.use_redroid_c2=1`) and the ones with 32-bit ABIs don't, so the
  image adds it (`CODEC2=off` in `versions/11.env` keeps Android 11 on OMX).
  The Android 12 base also lacks `redroid.c2.rc`, which runs the switch, so
  the image brings its own (`vendor/etc/init/holtdroid-c2.rc`).
- **ARM64 translation.** `libndk_translation` from the Google APIs emulator
  system image of the same API level, so APKs with only `arm64-v8a`
  libraries run. Executing ARM binaries as separate processes
  (`ro.enable.native.bridge.exec`) is off: binfmt_misc is not namespaced and
  registering it would change the host.
- **Google WebView as a provider.** A static overlay
  (`build/rro/webview.xml`) lists `com.google.android.webview` in
  `config_webview_packages` after `com.android.webview`, not available by
  default: the built-in WebView stays the default, and Holtdroid installs
  Google's WebView from its catalog and switches to it with
  `cmd webviewupdate set-webview-implementation`, which Android refuses for
  a package missing from the list.
- **32-bit x86 apps.** Every version is built on redroid's base with 32-bit
  ABIs (`zygote64_32`), not the `_64only` one: Google Chrome's x86 builds
  run their browser in 32 bits (`use32bitAbi`) and abort on a 64-bit-only
  Android ("WebView cannot be started with a browser process type"). The
  emulator images carry no 32-bit ARM translation, so `armeabi-v7a` and
  `armeabi` are dropped from the vendor and ODM ABI lists, and
  `ro.dalvik.vm.isa.arm` with them: on Android 12 to 15 an app with only
  32-bit ARM libraries fails to install instead of crashing. Android 11's
  system `build.prop` sets the ABI list itself and still names them.
- **The built-in WebView is re-signed with Chromium's public debug key**
  (`build/chromium-debug.keystore`, from
  `chromium/src/build/android/chromium-debug.keystore`, alias
  `chromiumdebugkey`, password `chromium`). Chromium's snapshot builds of
  `SystemWebView.apk` (`com.android.webview`) are signed with that key, so
  Holtdroid installs any of them as an update of the built-in WebView, and
  `pm uninstall com.android.webview` goes back to the factory one. redroid
  ships it signed with AOSP's test key, which no snapshot matches. The
  APK's contents are unchanged.
- **Newer Vulkan drivers** (`VULKAN=mesa`, only in `versions/15.env` so
  far). redroid ships Mesa 24.0.8 built with `android-strict`, which on its
  API level advertises Vulkan 1.1 and hides `VK_KHR_pipeline_library`,
  `dynamic_rendering`, `synchronization2` and more. Chromium 157 and later
  need them (ANGLE and Skia run on Vulkan, with no GL fallback) and abort
  with "GPU process isn't usable". The `mesa-build` stage cross-compiles
  Mesa (`MESA_VERSION`) with the Android NDK and `-Dandroid-strict=false`:
  first `mesa_clc` and the Intel shader precompiler for the build machine,
  then radv and anv for `x86_64-linux-android30`, with libdrm and expat
  linked in statically so the drivers need only Android's own libraries
  (the build fails otherwise). They replace `vulkan.radeon.so` and
  `vulkan.intel.so`; EGL and GLES stay redroid's. Verified on Intel UHD 630
  (Chromium 158 and the built-in WebView 128 render WebGL at 60 fps on
  Vulkan 1.4); AMD (radv) is built the same way.
- **No Bluetooth on Android 13 and 14** (`BLUETOOTH=off` in their
  `versions/*.env`). redroid's simulated Bluetooth HAL
  (`android.hardware.bluetooth@1.1-service.sim`, rootcanal) aborts as soon
  as the stack resets the controller: a CFI check in
  `LinkLayerController::Reset` on 13, `Invalid address` on 14. Android
  enables Bluetooth at boot, so `com.android.bluetooth` and the HAL crash a
  few times each and Bluetooth ends up off and unusable anyway. The image
  drops the `android.hardware.bluetooth` feature from
  `handheld_core_hardware.xml`, so system_server starts no Bluetooth service
  ("Bluetooth Hardware Not Present") and apps see a device without
  Bluetooth. The idle HAL stays. Android 11, 12 and 15 keep a working one.
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

- **A real Wi-Fi** when the host has `mac80211_hwsim` loaded with
  `radios=0` (Holtdroid decision #105). The Wi-Fi HAL, `libwifi-hal.so`,
  `wpa_supplicant` (and its init stanza) and `mac80211_create_radios` come
  from the emulator vendor image of the same system image zip, with the
  libraries they need that redroid's vendor partition lacks.
  `vendor/bin/holtdroid-wifi.sh` runs at `post-fs-data`: it creates two
  radios in the container's network namespace, moves the second into a
  private router namespace and runs hostapd (open `HoltdroidWifi`) and
  dnsmasq there, NATing over a veth (`wifiup`) into the container's
  namespace. A second veth, `eth1`, is a wired uplink through the same
  router (Holtdroid uses it as mobile data, `mobile0` is its router end,
  down by default). hostapd and nft are Alpine's with their musl
  libraries in `/vendor/holtdroid-net`: Android's hostapd is a HAL on
  14+, and its iptables needs legacy xtables, which mainline kernels may
  lack. A static overlay sets `config_ethernet_iface_regex` to `eth1`, so
  Android ignores the container's `eth0` (with Ethernet up it never joins
  Wi-Fi). Without the module there are no radios and `mobile0` stays up,
  so Android has `eth1` as plain Ethernet; `vendor.holtdroid.wifi=1` says the Wi-Fi is up and
  `/data/vendor/wifi/router.pid` holds a process in the router namespace.
  Radios are scoped to the network namespace that created them and go
  away with it.
- **A plain home screen**: a static overlay replaces the framework's
  `default_wallpaper` with Holtdroid's logo on a dark background, so the
  home and lock screens show it, and another one turns Launcher3's
  `search_container_workspace` into an empty view, so the first home
  screen has no search bar. Holtdroid's agent clears the home screen and
  pins the app under test on it (Holtdroid decision #126).

The image is `linux/amd64` only.

## Running it

The host kernel needs binder (`CONFIG_ANDROID_BINDERFS`); for Wi-Fi,
`modprobe mac80211_hwsim radios=0` on the host first. ashmem isn't
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
