# MEGAcmd 2.7 hobby jump-box host-path cutover

**Operationally validated 8 November 2026 on the OVH AlmaLinux 10.2 jump box.** The binary is the patched full-feature MEGAcmd 2.7 image from `fix/psa-async-timeout`, digest `30a9dd610b790408519029299aec8ec544f5134ff9f6b36ac07164576ce03809`.

This is the **host-compatible rootless Podman** deployment. It supersedes the previous, strictly SELinux-confined clone prototype **for workloads where the MEGAcmd server processes file paths supplied by existing native host clients**.

## Why the previous confined variant broke host transfers

The old 2.5.2 host clients (`mega-ls`, `mega-put`, `mega-get`, etc.) send absolute source or destination paths through the Unix socket. The **server opens and writes those paths**, rather than the client streaming the file bytes.

The first 2.7 container only mounted the versioned `.megaCmd` clone. Therefore:

- `mega-get /remote/backup /tmp/<host-temporary-directory>` reported download success, but the target temporary **directory didn't exist inside the container**. The server created a 160,090-byte file in its private `/tmp`; the host temporary directory remained empty. The ChatGPT Library backup script failed its mandatory SHA-256 readback.
- The local `~/.local/state/chatgpt-library-backups` path used by future `mega-put` calls was not mounted either, so new uploads would have failed for the same reason.
- Read-only `version`, `whoami`, `ls` and transfers worked, hiding the filesystem problem.

The actual correction is [`prototypes/megacmd-psa-hostpaths.container`](prototypes/megacmd-psa-hostpaths.container), **installed under the existing filename** `megacmd-psa-confined.container` to preserve service name and Library dependencies. It maps:

```text
/home/almalinux (host)                   -> /home/almalinux (container), rw
/tmp (host)                              -> /tmp (container), rw
~/.local/state/megacmd-psa-27/.megaCmd   -> /home/almalinux/.megaCmd, rw (nested override)
```

Podman supports nested bind mounts, tested here: the mounted `.megaCmd` inode inside the container matched the **versioned 2.7 clone**, not the original 2.5.2 state. The original root still has its `user_home_t` SELinux label, and a socket symlink redirects old host clients to the clone.

**Security tradeoff:** `SecurityLabelDisable=true` intentionally removes container SELinux label separation in order to mount the host user's home and shared `/tmp` *without recursively relabelling either*. The rootless container still has a read-only image, all capabilities dropped, `NoNewPrivileges=true`, a PID/memory limit and no automatic image pulling. The MEGAcmd process can now access the same user home files that the previous **native 2.5.2 server** could. The old strictly confined `:Z` variant remains available for workloads that never pass host filesystem paths to MEGAcmd; do not accidentally switch back without fixing file-sharing.

## Live validation — successful

- Corrected candidate SHA-256 before comment-only cleanup: `a194f1d2eb4fcffd9346ca4bd72394a4eb05d5b4386b86be8bd05d11b66fce26` (see installed jump-box configuration for latest exact hash).
- A disposable rootless Podman test with host home/`/tmp` and nested cloned state confirmed bidirectional host↔container access and identical clone directory inode, with the original state never relabelled.
- The patched service was stopped gracefully and restarted with the corrected binds. `mega-exec completion version` returned ready, legacy host `mega-exec version` reported **2.7.0.0**, `whoami` retained the original authenticated production session, container health passed.
- The previously failing **ChatGPT Library state-backup service succeeded**; it retrieved the remote backup into the host's temporary directory and completed SHA-256 readback verification.
- A separate **64 KiB** random file was uploaded from a host `/tmp` path using the ordinary `mega-mkdir` / `mega-put` client and downloaded back to another host directory using `mega-get`; SHA-256 matched and the remote test folder was deleted.
- The Library picker, tunnel and backup timer were restored. A picker restart with the new guarded readiness `ExecStartPre` passed, with no competing native MEGAcmd process.
- A checksum-only comparison of the original stopped 2.5.2 `~/.megaCmd` with its **fresh cold snapshot** (excluding runtime socket and lock) passed after the 2.7 trial, demonstrating the original state was unchanged. The cold snapshot and a separate earlier snapshot were retained for rollback.

## Boot and producer ordering

The user systemd manager has `Linger=yes`. The Quadlet generator handles boot enablement from the additional [`production-dropins/megacmd-psa-confined.container.d/20-autostart.conf`](production-dropins/megacmd-psa-confined.container.d/20-autostart.conf):

```ini
[Install]
WantedBy=default.target
```

Podman Quadlet generator creates `default.target.wants/megacmd-psa-confined.service`; **do not use `systemctl enable`** on generated Quadlet units.

The existing picker and backup services have separate versioned systemd drop-ins under [`production-dropins/`](production-dropins/) requiring/ordering after `megacmd-psa-confined.service`. Their `ExecStartPre` invokes [`megacmd-psa-wait-ready`](megacmd-psa-wait-ready), a bounded, **non-autospawning** client completion check (rather than an accepting Unix socket). The helper also refuses an unexpected socket alias or competing original 2.5.2 server. The tunnel already depends on the picker.

On 8 November, the Quadlet startup symlink, `Linger=yes`, generated dependency properties and a live picker restart were verified. **A full host reboot has not been used for acceptance testing.** Avoid automatic restart loops while the old 2.5.2 binary remains available as a manual rollback.

## Rollback

The earlier original `~/.megaCmd`, offline cold snapshot and original 2.5.2 RPM remain on the jump box. To roll back **only after checking for pending transfers**:

1. Pause the Library picker/tunnel/backup timer; make sure nothing is writing to the 2.7 clone. Record the authoritative remote journal/object changes made since cutover.
2. Stop `megacmd-psa-confined.service` gracefully, and verify no 2.7 process/WAL writers remain. A broken command dispatcher may require a bounded forced stop of the *clone only*.
3. Remove only the old-home `megacmd.socket` symlink if its exact target is `~/.local/state/megacmd-psa-27/.megaCmd/megacmd.socket`; **never** modify the original 2.5.2 state database or apply `:Z` to it.
4. Remove the 2.7 Quadlet boot enablement drop-in and the Library/backup dependency drop-ins before restarting the original 2.5.2 daemon. Verify single-daemon identity and authenticated listing before restoring producers.
5. Reconcile any remote writes by 2.7 before trusting the older 2.5.2 cache; the old state was preserved, not kept in sync with new transfers.

**Do not ship production MEGA session files, cached SDK databases, raw logs, private SSH keys or OVH API credentials to Git.** The secure on-host maintenance materials remain private.
