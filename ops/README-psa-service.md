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

The runtime Dockerfile refuses an unexpected server binary. Validated full-media server SHA-256: `36e5d29e7c257c86454c7940d3c7ffddfe3e3b787a45c776d6290344e850d97c`.

The Quadlet intentionally uses `HOME=/home/almalinux` and binds host `%h/.megaCmd` to that same location inside the container. Linux `mega-exec` uses `$HOME/.megaCmd/megacmd.socket`, so this arrangement preserves visibility of the Unix socket to **existing host-side clients**. The image contains **no session credentials**.

The matching source tree at commit `03817503` completed real SDK HTTP `gpsa` stall, disconnect/retry and late-response acceptance testing in the guest. The **full media-enabled Release build** from that source retained no test-only instrumentation and passed shared-library inspection. Its bundled image was loaded into the guest, and the real systemd user service start/health/stop test passed again under SELinux enforcing. New runtime image SHA-256/ID: `30a9dd610b790408519029299aec8ec544f5134ff9f6b36ac07164576ce03809`.

## Authenticated disposable-account acceptance (7 November 2026)

Performed in a **separate** temporary AlmaLinux 10.2 guest, using an explicitly created empty test account. The account email, password, MFA and session tokens are **not in this repository**. The user authenticated through a private interactive SSH terminal, not through ChatGPT tool arguments, environment variables, shell history or source control.

- The guest used a fresh 24 GiB sparse overlay, SELinux **enforcing**, a rootless patched 2.7.0 Quadlet, private host-loopback SSH access, a guest-local nftables outbound restriction, **no swap**, and a **512 MiB tmpfs at `~/.megaCmd`**. A clean QCOW2 snapshot was taken **before** the account was authenticated. The tmpfs is intentionally destroyed on VM reboot.
- Authenticated `whoami` matched the dedicated test account without printing credentials; remote `ls /` passed.
- **Normal transfer:** a 64 KiB random file was uploaded to a uniquely named test folder, downloaded and verified by SHA-256. The remote folder and both local test files were removed.
- **Network outage:** public HTTPS was dropped by a guest-only nftables table. Six local `version` and three `psa` client requests remained responsive. A root-owned systemd fallback timer was scheduled before injecting the fault; the table was removed manually and the timer cancelled after successful restoration. MEGA's live `gpsa` endpoint and authenticated remote listing worked again.
- **Service restart with live session:** a normal `systemctl --user restart` changed the server PID but preserved the session in tmpfs. Authenticated `whoami`, remote listing and container health checks passed on the replacement process.
- **Queued transfer during an outage:** a 256 KiB random file was queued while HTTPS was blocked. After reconnecting, the upload appeared, downloading it reproduced the SHA-256, and the unique test folder and both local files were removed. No duplicate test file was observed.
- All transfer operations used bounded commands; no production session, remote archive, Library or OpenArt task was accessed.

These results verify small-object authentication, command responsiveness, queued-upload recovery and session reuse across a **service restart**. They do **not** demonstrate uninterrupted large transfers, arbitrary network partitions, rollover during a server reboot, quota-exhaustion handling, or backward compatibility with an existing production 2.5.2 cache and transfer journal. The proposed runtime still has the explicit SELinux isolation tradeoff described below.

## Recommended confined alternative (tested on synthetic state)

The original Quadlet above disables SELinux container labels because it mounts the **live** `~/.megaCmd` directory. A more secure candidate is [`ops/prototypes/megacmd-psa-confined.container`](prototypes/megacmd-psa-confined.container), described in [`ops/README-psa-confined-state-prototype.md`](README-psa-confined-state-prototype.md). It binds a **versioned clone** of the old state under `%h/.local/state/megacmd-psa-27/.megaCmd:Z` and retains `container_t` confinement. A single, controlled symlink at the old host `megacmd.socket` pathname connects legacy host-side clients to that new state.

The real patched 2.7 server, exact 2.5.2 host client binary, and rollback to the exact 2.5.2 server binary were tested in an isolated SELinux-enforcing AlmaLinux guest, with synthetic state only. **Prefer this confined candidate for production review**; do not relabel or clone the live `~/.megaCmd` until it has been quiesced and the maintenance plan approved. The current Quadlet stays as an experimental compatibility fallback, not the default security recommendation.

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

The real user-manager activation/stop gate, accountless SDK HTTP stall/drop/late-response tests and **small authenticated transfer/network-recovery tests** have passed in disposable VMs. Remaining production gates: review the SELinux-label-disabling tradeoff; reconcile existing production backup/Library/OpenArt authoritative journals; rehearse a live-cache and old-RPM rollback; obtain explicit maintenance approval. The PSA mitigation is not a general-purpose SDK deadlock fix. The separate `psa --discard` synchronous request and orphaned archive `mega-exec` clients remain out of scope.
