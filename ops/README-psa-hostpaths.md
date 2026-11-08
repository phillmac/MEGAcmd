# MEGAcmd 2.7 hobby jump-box host-path cutover

**Current 9 November 2026 recovery state:** The user retired the stale 2.5.2 rollback cache and its remaining cold snapshot, freeing **2,738,847,886 bytes (~2.55 GiB)** of historical data. The original `~/.megaCmd` directory is still preserved as a host-client socket/config shim, containing only `megacmd.cfg` and the `megacmd.socket` symlink pointing to the active versioned 2.7 state. The current 2.7 cache, saved session and remote data were not touched; local dispatcher and authenticated host listing passed afterward. The private `rollback-original.sh` and `ensure-original-and-producers.sh` on the jump box now **refuse** with exit 96. **Never run the old 2.5 rollback sequence below:** the old authenticated SDK state is no longer available. Recovery is now via a fresh 2.7 cache rebuild, reauthentication and journal reconciliation.


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

## Recovery after the obsolete 2.5 rollback was retired

The old SDK cache/session and separate cold snapshot were intentionally removed on 9 November. Do not use the historic 2.5.2 scripts for recovery; they are guarded to refuse execution. A downgrade with stale metadata would risk diverging from newer cloud operations.

If the **active 2.7 cache itself** becomes unusable, pause the Library picker, tunnel and archive/backup producers, inspect transfer state, and stop `megacmd-psa-confined.service` gracefully. Preserve any useful diagnostic evidence on-host. Move aside the defective versioned 2.7 cache; create an empty 0700 cache directory with the correct owner and make sure the existing host `~/.megaCmd/megacmd.socket` symlink targets its socket. Start the same patched 2.7 image and reauthenticate interactively if needed. Allow metadata to download from the cloud. Before resuming Library/backup jobs, verify authenticated listing, current remote objects, upload queue and the independent Library/OpenArt completion journals.

A fresh 2.7 cache rebuild has not been tested on the production account. It may require credentials/MFA/recovery key, substantial API traffic and regeneration of lost transfer/resume or sync state. MEGA's cloud content is authoritative, while independently maintained Library/OpenArt/backup journals are also important: **never delete those journals with the MEGAcmd cache.**

## Historical 2.5 rollback details

The 8 November cutover used a distinct on-host 2.5 cold snapshot and the original RPM for rollback. That historical proof of the original cache being preserved at cutover was accurate **at that time**. On 9 November, after the live 2.7 service had continued processing cloud changes, the user explicitly retired that option and the old data were deleted. The original 2.5 binary may remain installed but is no longer a turnkey authenticated rollback.
