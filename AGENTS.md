# holtdroid-redroid — instructions for coding agents

Single source of rules for AI coding agents and human contributors.
`CLAUDE.md` only imports it.

The redroid image Holtdroid runs its sessions on. Read `README.md` first:
it says what the image changes on top of redroid and why. Decisions about
how Holtdroid uses it live in Holtdroid's `design/decisions.md`.

## Layout

```
Dockerfile                        all patches to redroid, as build stages
cmd/holtdroid-redroid-init/       the entrypoint: cgroup isolation, then exec /init
build/lpunpack.py                 extracts a partition from an Android super image
vendor/bin/redroid.c2.sh          replaces redroid's Codec2 switch
```

## Rules

- Patch upstream files in a build stage with `sed`, guarded by a `grep` that
  fails the build when upstream changes, instead of committing copies.
- Pin every base image by digest and every download by checksum.
- Changing what the image needs from the host or the Pod spec means
  changing Holtdroid's provider and docs in the same step.
- Verify on a real node (Holtdroid's scratch namespace recipe), not only by
  building: boot, `scrcpy-server list_encoders`, an arm64-only APK, WebGL.
- Comments: almost none.
- Commits: Conventional Commits, one line, no body, no trailers.
