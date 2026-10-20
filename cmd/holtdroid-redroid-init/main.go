package main

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
)

const cgroupRoot = "/sys/fs/cgroup"

func main() {
	runtime.LockOSThread()
	if err := isolateCgroup(); err != nil {
		fmt.Fprintln(os.Stderr, "holtdroid-redroid-init:", err)
		os.Exit(1)
	}
	args := append([]string{"/init"}, os.Args[1:]...)
	err := syscall.Exec("/init", args, os.Environ())
	fmt.Fprintln(os.Stderr, "holtdroid-redroid-init: exec /init:", err)
	os.Exit(1)
}

func isolateCgroup() error {
	own, err := ownCgroup()
	if err != nil || own == "/" {
		return err
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
	if err := syscall.Unmount(cgroupRoot, syscall.MNT_DETACH); err != nil {
		return fmt.Errorf("unmount %s: %w", cgroupRoot, err)
	}
	if err := syscall.Mount("cgroup2", cgroupRoot, "cgroup2", 0, ""); err != nil {
		return fmt.Errorf("mount %s: %w", cgroupRoot, err)
	}
	return nil
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
