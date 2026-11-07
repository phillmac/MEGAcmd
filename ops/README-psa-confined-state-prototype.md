# Experimental confined MEGAcmd 2.7 state cutover

**Isolated full-daemon and rollback acceptance passed, but REVIEW-ONLY for production. Nothing here authorises deploying or restarting the live OVH jump-box MEGAcmd daemon.** Do not put any production MEGA token, state database, session or credential into GitHub, ChatGPT or an off-host image.

The jump box currently runs MEGAcmd **2.5.2** on **AlmaLinux 10.2**, SELinux **enforcing**. Its `~/.megaCmd` directory is **1.4 GiB**, with a ~1.24 GiB SQLite main DB, ~60 MiB main WAL, and ~4 MiB transfer WAL. These files are actively open. The current daemon and the existing original directory must remain unchanged until a separately approved maintenance window.

## Why an alternative is needed

The initial validated service ([`ops/megacmd-psa.container`](../megacmd-psa.container)) binds the **existing** `~/.megaCmd` folder and sets `SecurityLabelDisable=true`. It works in an isolated enforcing guest, but deliberately removes process SELinux separation.

Podman's `:Z` bind-mount suffix preserves container `container_t` confinement and assigns a **private** `container_file_t` MCS label to mounted state. Applying `:Z` to the **live** `~/.megaCmd` is unsafe for an unapproved cutover because it recursively alters the original files' host labels. A rollback must be possible without reversing unwanted label changes or recovering files modified in place by 2.7.

**Proposed alternative:** clone the stopped 2.5.2 state into a **versioned** directory, relabel only that clone through Podman `:Z`, and expose its Unix socket to existing host clients through a single symlink at the original expected socket pathname. On rollback, remove only the symlink and restart 2.5.2 using the original untouched databases.

## Tested in disposable state on the actual SELinux-enforcing jump box

The test never opened or mounted the production `~/.megaCmd` directory:

- Rootless Podman ran a small **SELinux-confined** Python container (`container_t`) with a temporary owned directory mounted `:Z` (private `container_file_t` MCS label).
- A host-side Python client connected to its Unix socket through a symlink in a **separate** mock home directory. The original mock home retained its ordinary user-home SELinux label.
- The container and host were both able to use the test socket. After stopping, `restorecon -RF` on a different scratch-only test restored the original SELinux labels exactly.
- The real production `~/.megaCmd` remained labelled `user_home_t` before and after both tests.

These demonstrate kernel socket-path and SELinux-label behaviour, **not** full 2.7 MEGAcmd compatibility with its existing 2.5.2 cache. The actual patched daemon must still be tested with this exact `:Z` versioned-state/symlink arrangement before production adoption.

## Actual patched-server, legacy-client and rollback acceptance (7 Nov 2026)

The above kernel-level result was followed by a **second, full-application test** in a separate ephemeral AlmaLinux 10.2 KVM guest, with SELinux **enforcing** and no external network route. The guest had a dummy original home directory, an empty versioned state clone, and an old-socket-path symlink to the clone. No production session/cache, password or MEGA account was copied into it.

- Real MEGAcmd **2.7.0** using `ops/prototypes/megacmd-psa-confined.container` started through `systemctl --user start megacmd-psa-confined.service`, retained `container_t:s0:c221,c643` confinement, and privately labelled **only the cloned state** `container_file_t:s0:c221,c643`. Original dummy `~/.megaCmd` stayed `user_home_t:s0`, while the socket symlink was host-visible. The 2.7 client `version` and Podman health check both passed.
- Copied the jump box's **exact 2.5.2 `mega-exec` binary** (SHA-256 `51d9e05b29ae55dd9a0ced17fcc9903275bc2680a9aea587ae68f91757c6526e`) into the guest. Its public runtime libraries were supplied separately. **Host-side 2.5.2 client `version` and `help` succeeded against the confined 2.7 server through the old-home socket symlink**, confirming protocol/socket compatibility for those commands.
- Stopped the 2.7 Quadlet gracefully, verified its service was inactive, removed only the symlink after checking its exact expected destination, and confirmed the original dummy cache sentinel SHA-256 and `user_home_t` label were unchanged.
- Copied the jump box's **exact original 2.5.2 server executable** (SHA-256 `27f4f0b06f04745990a7292821767964d70a28a2f931af112c5860312a000a06`) into that same guest. Starting it against the original dummy state recreated a native socket; the original 2.5.2 client successfully issued `version` (reported `2.5.2.1`) and `help`.
- All operations stayed inside the disposable guest and ephemeral local build directories. The real 2.5.2 jump-box daemon, cache, socket, archive processes and `dnsonly-eu1` were not modified.

**Result:** the preferred architecture can keep SELinux process separation enabled and preserve the original cache intact, with tested host-side old-client compatibility and old-server rollback on **empty synthetic state**. It still needs a separately approved, quiesced clone/import test for the actual 1.4 GiB 2.5.2 cache, transfer-journal consistency, and a production rollback window.

## Production on-host maintenance trial, 7–8 November 2026 AEDT

**Outcome: production remains on 2.5.2; no permanent 2.7 migration was made.** Owner-approved maintenance paused the Library picker, tunnel and backup timer, gracefully stopped the old daemon, and copied the **quiesced** ~1.35 GB `~/.megaCmd` to an owner-private **on-host** cold snapshot and a separate clone. Content comparisons passed, no session material left the jump box. All attempts were rolled back to 2.5.2 with authenticated directory listing and the Library producers restored.

The trials identified several independent operational hazards, not evidence that the PSA source fix itself was incorrect:

1. **Readonly-root clone:** the immutable snapshot root was chmod `0500`. `rsync -a` propagated that mode onto the trial clone; the new MEGAcmd server could not write its socket/cache. A clone root must be explicitly `chmod 0700` after copying, while the snapshot remains readonly. Compare identical file content but allow this one intentional root-directory mode difference.
2. **Large-cache startup:** a network-disabled test of the real cloned 2.5.2 cache initially took more than a brief client timeout while processing the ~1.24 GB SQLite main database. Its WAL grew to roughly **820 MiB** before settling, and the full 2.7 server eventually answered `version` and exited cleanly. Startup checks must have bounded but realistic multi-minute patience, WAL and free-space limits, and must never classify an initial timeout alone as corruption.
3. **Native old-client auto-start:** source `MegaCmdShellCommunicationsPosix::createSocket()` automatically forks `mega-cmd-server` if the first `mega-exec` socket connection fails. During a live handoff, an ordinary `/usr/bin/mega-exec version` probe could launch **another native 2.5.2 daemon** before the 2.7 socket is ready. **Never use the native client directly during the transition.** The tracked [`ops/megacmd-psa-safe-legacy-client`](../megacmd-psa-safe-legacy-client) prepares a checksum-pinned relocated client, an empty search PATH and an inert fallback server. It connects only if a raw Unix-socket probe succeeds; it accepts only read-only commands. This design was verified in isolation: an absent socket failed without launching a native daemon, and the relocated exact 2.5 client contacted the 2.7 server on the cloned cache without starting another server.
4. **Unix socket pathname length:** an initial *test-only* temporary home had a 120-byte socket pathname, exceeding Linux's 108-byte `sockaddr_un.sun_path` allocation. Shortening the temporary alias to 42 bytes made the offline legacy-client-to-2.7 clone test pass. Assert pathname length during test setup; the actual original and clone socket paths are shorter.
5. **Zombie process false-positive:** the large SDK startup can create a transient *defunct* `mega-cmd-server` child **inside the same container**, including from the initial health probe. A guard comparing the unfiltered `ps -C mega-cmd-server` PID list with Podman's main PID falsely interpreted that zombie as a second native daemon, triggering an unnecessary rollback. Filter `STAT` beginning with `Z`, and compare only the **live** server PIDs to Podman's recorded main PID and cgroup; stop if a genuinely independent live daemon is discovered.
6. **Health check must not spawn a daemon:** the earlier `HealthCmd=mega-exec version` invokes the same potentially auto-starting client while the SDK is still loading. The revised confined Quadlet has a **non-spawning** Python Unix socket connect probe with a 2-second timeout. This is only a listener-readiness check, **not proof that command petitions are being processed**. After it passes, run a bounded safe legacy-client version request for readiness.
7. **Graceful stop during SDK initialization can time out:** one early rollback was requested before the 2.7 command dispatcher was ready, and the service's `ExecStop` could not obtain an `exit` acknowledgment within the 80-second systemd stop budget. Systemd eventually removed the test container. Allow the startup grace interval when resources are healthy rather than falsely aborting it; if a real fault requires rollback, inspect for **no live 2.7 container/server** before starting 2.5.2. The original snapshot must remain intact.
8. **Fail-safe double-rollback:** a temporary watchdog searched for an exact native process *argv* string and missed a restored `/usr/bin/mega-cmd-server` (with absolute path), mistakenly attempting a second rollback. Use `/proc/<pid>/exe` to identify the original binary, ignore zombies, require exactly one live native process, and make the watchdog idempotent. The corrected script was tested as a no-op against the successfully restored 2.5.2 daemon.
9. **Control-plane recovery:** direct SSH from .eu to the jump box was not working, although the existing authorised desktop-command path was healthy. A temporary reverse SSH recovery tunnel failed to complete its handshake and was removed along with its restricted temporary key. A more reliable independent recovery path should be validated before another risky cutover.

The final attempted live clone was stopped and **2.5.2 restored**, with a verified matching cold snapshot, authenticated listing and all three Library/tunnel/backup producer units active. Its temporary systemd rollback timer was disarmed. Subsequent development and tests must be isolated from production until the above safeguards are implemented and validated.

## Reviewed unit

[`ops/prototypes/megacmd-psa-confined.container`](prototypes/megacmd-psa-confined.container) is intentionally outside the default Quadlet directory and has no [Install] section. It uses the same verified AlmaLinux 10.2 runtime image and helper as the first service, but it mounts a different state directory and **does not disable SELinux**.

Run a dry-run of the generator, without installing:

```bash
QUADLET_UNIT_DIRS="$PWD/ops/prototypes" /usr/libexec/podman/quadlet -user -dryrun
```

## Planned controlled cutover — do not run without maintenance approval

1. Verify owner-controlled alternative recovery SSH, the unchanged original 2.5.2 RPM and executable SHA-256, and an on-host secure backup destination. **Pause** all MEGA-dependent jobs with their own after-current semantics; capture authoritative Library, OpenArt, backups and other transfer journals and archive IDs. Do not assume the absence of processes proves completion.
2. Have no active MEGAcmd transfers or clients. Stop 2.5.2 gracefully, and verify its process, socket listener and WAL writers have exited. Preserve its original `~/.megaCmd` contents and labels for rollback. Do not copy a live SQLite/WAL tree and call it consistent.
3. On the **same jump box only**, create the versioned destination with mode 0700: `$HOME/.local/state/megacmd-psa-27/.megaCmd`. Clone the **quiesced** original files with metadata preserved (e.g. `rsync -a` excluding the runtime `megacmd.socket` and `lockMCMD`). Never stage these state files in the Git repository, a public container build context, or a foreign-region server.
4. Preserve the original state as a rollback point. Ensure the old socket listener is gone. If a stale socket pathname remains, preserve or remove **only that socket** under the approved plan, without touching databases.
5. Create `~/.megaCmd/megacmd.socket` as a symlink pointing to `~/.local/state/megacmd-psa-27/.megaCmd/megacmd.socket` on the *same host*. Before doing so, require `test ! -e ~/.megaCmd/megacmd.socket && test ! -L ~/.megaCmd/megacmd.socket`; don't overwrite an existing socket or symlink.
6. Verify the cloned state ownership and 0700 permissions, then install the reviewed confined Quadlet and graceful-stop helper **without [Install]**. `systemctl --user daemon-reload`, then manually start the service. Podman privately relabels only the versioned state clone.
7. Verify host-local `mega-exec` can connect through the symlink, the server's hash and process identity, logged-in status, bounded directory listing, and reconciliation of the transfer journal before allowing any queue producer to resume.
8. If rollback is needed, gracefully stop the new service and verify the process and copied-state WAL writers are gone. Remove the symlink **only if it still points to the expected versioned socket**. Restart the original 2.5.2 binary with the original unmodified state. Do not automatically roll back a cloud mutation after transfers have been committed under 2.7—reconcile new remote objects/versions and source journals first.

**No automatic restart or downgrade** is configured. Reverting file labels on the versioned clone is not required for the original binary; its old state was not relabelled. Retain the clone separately until migration evidence and remote consistency are reviewed.

## Security and remaining risks

- Unlike `SecurityLabelDisable=true`, this variant retains container `container_t` and a privately labelled state directory. A rootless container still runs as the host's unprivileged account, with a read-only image, no added capabilities, and a controlled mount.
- `:Z` makes a private MCS-labeled clone; host unconfined processes could access its data with normal filesystem permissions, as confirmed for test socket paths. This is not a substitute for protecting the clone with 0700 permissions.
- The original daemon may auto-launch if a host client sees a missing socket; all producers must be paused before removing/redirecting the socket. Test the symlink with real `mega-exec` in a disposable enforcing guest first.
- The original MEGAcmd cache can be version-sensitive; copying its SQLite and WAL files offline does not prove 2.7 can safely import them. Do not silently start 2.7 on the only production copy.
- The ordinary rollback starting state is frozen at cutover. Remote changes made during the new server's operation require separate journal/remote reconciliation; stale local DB alone is not authoritative.
- This does not fix unrelated blocking SDK operations or the separate synchronous `psa --discard` command.

**Status:** real 2.7 server under SELinux confinement, legacy 2.5.2 host client IPC through a socket alias, and 2.5.2 server rollback on synthetic state all passed in a disposable enforcing guest. **Production's actual 2.5.2 session/cache and outstanding transfer journals remain untested and must not be upgraded without a quiesced backup, reconciliation and explicit maintenance approval.**
