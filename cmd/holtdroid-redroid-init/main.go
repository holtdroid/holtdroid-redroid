package main

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
)

const (
	cgroupRoot = "/sys/fs/cgroup"
	procSys    = "/proc/sys"
	procSysNet = "/proc/sys/net"
	procFlags  = syscall.MS_NOSUID | syscall.MS_NODEV | syscall.MS_NOEXEC
	// virtualBlock holds the loop devices' queue settings.
	virtualBlock = "/sys/devices/virtual/block"
)

// guarded are the node-wide sysctls Android's binaries write (the rc files'
// writes are removed at build time): netbpfload relaxes unprivileged BPF and
// exits, rebooting Android, when the write fails, so each gets a plain file
// bind-mounted over it that takes the write instead of the kernel.
// kptr_restrict and mmap_rnd_bits stay real: init aborts unless it can raise
// them, and raising them only hardens the node.
var guarded = []string{
	"/proc/sys/kernel/unprivileged_bpf_disabled", "/proc/sys/kernel/modprobe", "/proc/sys/kernel/sysrq",
	"/proc/sys/kernel/hung_task_timeout_secs", "/proc/sys/kernel/hung_task_check_count", "/proc/sys/kernel/hung_task_warnings",
	"/proc/sys/kernel/sched_schedstats", "/proc/sys/kernel/perf_event_paranoid",
	"/proc/sys/vm/watermark_scale_factor", "/proc/sys/vm/watermark_boost_factor", "/proc/sys/vm/mmap_min_addr",
}

const decoyDir = "/dev/.holtdroid-sysctl"

func main() {
	if len(os.Args) == 2 && os.Args[1] == "--guard" {
		if err := guard(); err != nil {
			fmt.Fprintln(os.Stderr, "holtdroid-redroid-init guard:", err)
			os.Exit(1)
		}
		return
	}
	runtime.LockOSThread()
	if err := isolateCgroup(); err != nil {
		fmt.Fprintln(os.Stderr, "holtdroid-redroid-init:", err)
		os.Exit(1)
	}
	if err := protectSysctls(); err != nil {
		fmt.Fprintln(os.Stderr, "holtdroid-redroid-init:", err)
		os.Exit(1)
	}
	if err := exec.Command("/proc/self/exe", "--guard").Start(); err != nil {
		fmt.Fprintln(os.Stderr, "holtdroid-redroid-init: start guard:", err)
		os.Exit(1)
	}
	args := append([]string{"/init"}, os.Args[1:]...)
	err := syscall.Exec("/init", args, os.Environ())
	fmt.Fprintln(os.Stderr, "holtdroid-redroid-init: exec /init:", err)
	os.Exit(1)
}

func isolateCgroup() error {
	own, err := ownCgroup()
	if err != nil {
		return err
	}
	if own == "/" {
		if err := writableBlockQueues(); err != nil {
			return err
		}
		if err := mountCgroup(); err != nil {
			return err
		}
	}
	child := filepath.Join(cgroupRoot, own, "android")
	if err := os.Mkdir(child, 0o755); err != nil && !errors.Is(err, fs.ErrExist) {
		return err
	}
	if err := os.WriteFile(filepath.Join(child, "cgroup.procs"), []byte("0"), 0); err != nil {
		return fmt.Errorf("move into %s: %w", child, err)
	}
	if err := syscall.Unshare(syscall.CLONE_NEWCGROUP); err != nil {
		return fmt.Errorf("unshare cgroup namespace: %w", err)
	}
	return mountCgroup()
}

// writableBlockQueues lets apexd tune its loop devices' queues in an
// unprivileged container: sysfs's superblock becomes writable, /sys itself
// stays a read-only mount and only a bind of the virtual block devices is
// writable.
func writableBlockQueues() error {
	steps := []struct {
		target string
		flags  uintptr
	}{
		{virtualBlock, syscall.MS_BIND},
		{"/sys", syscall.MS_REMOUNT | procFlags},
		{"/sys", syscall.MS_BIND | syscall.MS_REMOUNT | syscall.MS_RDONLY | procFlags},
		{virtualBlock, syscall.MS_BIND | syscall.MS_REMOUNT | procFlags},
	}
	for _, s := range steps {
		if err := syscall.Mount(s.target, s.target, "", s.flags, ""); err != nil {
			return fmt.Errorf("mount %s (flags %#x): %w", s.target, s.flags, err)
		}
	}
	return nil
}

func mountCgroup() error {
	if err := syscall.Unmount(cgroupRoot, syscall.MNT_DETACH); err != nil {
		return fmt.Errorf("unmount %s: %w", cgroupRoot, err)
	}
	if err := syscall.Mount("cgroup2", cgroupRoot, "cgroup2", 0, ""); err != nil {
		return fmt.Errorf("mount %s: %w", cgroupRoot, err)
	}
	return nil
}

// protectSysctls makes /proc/sys read-only except /proc/sys/net in the
// wrapper's own view. Android's init mounts a fresh procfs over /proc, which
// hides these binds, so guard repeats them on top of that mount.
func protectSysctls() error {
	steps := []struct {
		target string
		flags  uintptr
	}{
		{procSys, syscall.MS_BIND},
		{procSys, syscall.MS_BIND | syscall.MS_REMOUNT | syscall.MS_RDONLY | procFlags},
		{procSysNet, syscall.MS_BIND},
		{procSysNet, syscall.MS_BIND | syscall.MS_REMOUNT | procFlags},
	}
	for _, s := range steps {
		if err := syscall.Mount(s.target, s.target, "", s.flags, ""); err != nil {
			return fmt.Errorf("mount %s (flags %#x): %w", s.target, s.flags, err)
		}
	}
	return nil
}

// guard waits for init's own procfs mount on /proc and then bind-mounts the
// guarded sysctls read-only on top of it; the container's mounts propagate
// to the mount namespaces init creates later.
func guard() error {
	initial, err := procMounts()
	if err != nil {
		return err
	}
	deadline := time.Now().Add(2 * time.Minute)
	for {
		cur, err := procMounts()
		if err != nil {
			return err
		}
		if cur > initial {
			break
		}
		if time.Now().After(deadline) {
			return errors.New("init did not mount /proc within 2 minutes")
		}
		time.Sleep(2 * time.Millisecond)
	}
	if err := os.MkdirAll(decoyDir, 0o700); err != nil {
		return err
	}
	for _, file := range guarded {
		cur, err := os.ReadFile(file)
		if err != nil {
			continue
		}
		decoy := filepath.Join(decoyDir, filepath.Base(file))
		if err := os.WriteFile(decoy, cur, 0o600); err != nil {
			return err
		}
		if err := syscall.Mount(decoy, file, "", syscall.MS_BIND, ""); err != nil {
			return fmt.Errorf("bind %s over %s: %w", decoy, file, err)
		}
	}
	return nil
}

func procMounts() (int, error) {
	b, err := os.ReadFile("/proc/self/mountinfo")
	if err != nil {
		return 0, err
	}
	n := 0
	for line := range strings.SplitSeq(string(b), "\n") {
		if f := strings.Fields(line); len(f) > 4 && f[4] == "/proc" {
			n++
		}
	}
	return n, nil
}

func ownCgroup() (string, error) {
	b, err := os.ReadFile("/proc/self/cgroup")
	if err != nil {
		return "", err
	}
	for line := range strings.SplitSeq(string(b), "\n") {
		if path, ok := strings.CutPrefix(line, "0::"); ok {
			return path, nil
		}
	}
	return "", errors.New("no cgroup v2 entry in /proc/self/cgroup")
}
