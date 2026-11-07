# MEGAcmd 2.7 PSA mitigation — AlmaLinux 10.2 rootless service

**Status: validated under a real systemd user manager inside a disposable AlmaLinux 10.2 VM with SELinux enforcing. Not installed, started, or enabled on the OVH production jump box.**

Canonical source: this repository, branch `fix/psa-async-timeout`, based on upstream MEGAcmd `2.7.0_Linux`. The patched binary is validated as a full-media Release build, with Debug-only fault injection absent.

## Purpose and compatibility

The jump box is **AlmaLinux 10.2**, running its original `megacmd-2.5.2-1.1` RPM. The old hung PID has already gone; restarting the live daemon is not necessary to clear it.

The rootless Podman runtime image combines the full-feature patched 2.7 Release binary and its vcpkg FFmpeg runtime libraries with AlmaLinux 10.2 and the `fuse-libs` package. It runs **without an elevated host container daemon**.

Tracked files:

- `build-with-docker/Dockerfile.psa-full-release` — complete, media-enabled 2.7 build (no test hooks).
- `build-with-docker/Dockerfile.psa-alma10-runtime` — AlmaLinux 10.2 runtime, pinned by the expected MEGAcmd server SHA-256.
- `ops/megacmd-psa.container` — rootless Quadlet source. It has **no [Install]** section.
- `ops/megacmd-psa-graceful-stop` — fixed-target host-side graceful shutdown using `mega-exec exit`.
- `ops/test-megacmd-psa-runtime.sh` — offline, disposable integration smoke test.

## Build and offline verification on .eu

```bash
cd ~/worktrees/megacmd-psa-fix

# Prerequisite: build the full-feature Release image first, without credentials:
podman build -f build-with-docker/Dockerfile.psa-full-release \
    --target build --build-arg BUILD_CORES=2 \
    --memory=12g --memory-swap=12g --layers \
    -t localhost/megacmd-psa-full-release-linked:2.7 .

# Package the same binaries in the AlmaLinux 10.2 runtime:
podman build -f build-with-docker/Dockerfile.psa-alma10-runtime \
    --memory=2g --memory-swap=2g --layers \
    -t localhost/megacmd-psa-alma10-runtime:2.7 .

# Inspect the generated service without installing it:
QUADLET_UNIT_DIRS="$PWD/ops" /usr/libexec/podman/quadlet -user -dryrun

# Repeated safe tests. They create private temporary state directories,
# disable networking and clean up all test containers:
bash ops/test-megacmd-psa-runtime.sh
```

The runtime Dockerfile refuses an unexpected server binary. Validated full-media server SHA-256: `76ae6af668ee6735731d3a1af2c56fd080b08cca498bd3510b543c274ec2942c`.

The Quadlet intentionally uses `HOME=/home/almalinux` and binds host `%h/.megaCmd` to that same location inside the container. Linux `mega-exec` uses `$HOME/.megaCmd/megacmd.socket`, so this arrangement preserves visibility of the Unix socket to **existing host-side clients**. The image contains **no session credentials**.

## Deliberate security and operational choices

- **No automatic installation or enablement.** The checked-in Quadlet has no [Install]/WantedBy section. Never copy it onto the jump box while the old daemon is still active.
- **Rootless only.** Container UID 0 maps to the calling unprivileged host account, which can access its own `~/.megaCmd` files. No host root privileges are requested.
- Read-only root filesystem, temporary `/tmp` and `/run`, no added capabilities, no-new-privileges, 4 GB RAM and 512-thread limits.
- **SELinux tradeoff:** production `~/.megaCmd` must not be relabelled `:Z`/`:z` because that could prevent rollback to the host-native daemon. Quadlet uses `SecurityLabelDisable=true` as an explicit, **reduced SELinux isolation** tradeoff. Revisit a separate labelled volume and host-client socket sharing before adopting this in production.
- Network is **not disabled in the proposed service** (the SDK needs outbound MEGA access). Offline tests use `--network none` and an ephemeral home.
- The version-only health check runs every 60 seconds. It reports unhealthy status but does **not** automatically kill/restart the server, which might interrupt transfers.
- **Graceful stop:** systemd invokes the installed `megacmd-psa-graceful-stop` helper, which sends `mega-exec exit` and waits until the process stops before Quadlet's standard container cleanup. The helper never sends a kill signal. Manual review is needed for a failed stop; systemd/Podman's normal service timeout is still a last-resort boundary.
- Logs may include sensitive local paths; the raw MEGA session must never be committed to Git or shipped with the container image.

## Cutover prerequisites — do not execute without separate approval

1. Identify and pause the actual Library, OpenArt and backup producers. Verify their authoritative journals and MEGA remote status; an absent archive lock is not proof of completion.
2. Confirm a trusted **independent jump-box recovery path** and ownership of the original 2.5.2 RPM and executable hash. Plan a maintenance window.
3. Verify no MEGAcmd clients are active; stop the old daemon by an approved graceful method. **Only then** take a consistent, permission-preserving snapshot of `~/.megaCmd`, including SQLite WAL files.
4. Transfer the verified image by an approved private route to the jump box, load it with rootless Podman, verify image and binary hashes, and ensure runtime dependencies and host client compatibility.
5. From a reviewed checkout, install the helper and Quadlet under the **target account** (not as root), only after confirming the old daemon has stopped:
   ```bash
   install -d -m 0700 ~/.local/libexec ~/.config/containers/systemd
   install -m 0755 ops/megacmd-psa-graceful-stop ~/.local/libexec/megacmd-psa-graceful-stop
   install -m 0644 ops/megacmd-psa.container ~/.config/containers/systemd/megacmd-psa.container
   systemctl --user daemon-reload
   systemctl --user start megacmd-psa.service
   ```
6. Verify `/proc/<server-pid>/exe`, local `version`, repeated `help`/socket checks, account state, then bounded remote read-only operations. Reconcile uploads before resuming producers.
7. Keep the old RPM and rollback state intact. If the new process cannot start or remain responsive, use the reviewed rollback plan in Dotfiles rather than wiping or downgrading an evolved cache.

**The commands in step 5 have NOT been run on the jump box.** They were executed exclusively in an isolated, credential-free AlmaLinux 10.2 KVM guest. The real `systemd --user` manager (with disposable test account and lingering enabled) generated and started the Quadlet while SELinux was **enforcing**; the guest exposed the Unix socket, answered `mega-exec version`, passed the Podman health check, and `systemctl --user stop` terminated it gracefully in approximately three seconds, without a forced kill. No auto-enable target exists. The VM does not establish compatibility with production MEGA sessions or transfers. The .eu remote shell also has a working user bus when the missing `XDG_RUNTIME_DIR` and `DBUS_SESSION_BUS_ADDRESS` variables are supplied.

## Known open gates

The full user-manager activation/stop gate and **anonymous real SDK `gpsa` HTTP stall/drop/late-response tests** have passed on the disposable VM; no MEGA account was used. Still outstanding: **authenticated** session/transfer testing, review of the SELinux-label-disabling tradeoff, and a documented, validated cache/queue cutover with rollback. The PSA mitigation is not a general-purpose SDK deadlock fix. The separate `psa --discard` synchronous request and orphaned archive `mega-exec` clients are not resolved by this change.
