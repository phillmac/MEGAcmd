# MEGAcmd PSA — real MEGA SDK transport fault regression

**Scope:** test-only source code on the `fix/psa-async-timeout` branch. Not a production network feature, and no real MEGA login or credentials are used.

## Why this differs from the previous synthetic test

The existing `PsaNeverCompletesDoesNotBlockPetitions` regression occupies the PSA request gate without entering the SDK. That proves dispatcher and teardown behaviour but does **not** exercise the real `MegaApi::getPSA()` operation.

`PsaActualSdkTransportCanStallWithoutBlockingPetitions` calls the **real SDK method** (test-flagged anonymous execution) and directs the SDK HTTP POST to a loopback-only Python server. Its `gpsa` command must be observed by the server. The mock never carries user credentials or account data.

The test-only `PSA_ALLOW_ANONYMOUS_SDK_TRANSPORT` flag and the `MEGACMD_TEST_GPSA_API_URL` argument are restricted to the Debug integration executable. The ordinary Release server continues requiring a logged-in user before checking PSA. The URL must begin with `http://127.0.0.1:`.

## Reproduce inside a disposable AlmaLinux VM

1. Build the MEGAcmd Debug integration target with `ENABLE_MEGACMD_TESTS=ON`. Use an ephemeral `HOME`, not `~/.megaCmd` from a real account. Copy only `mega-cmd-tests-integration` to the test VM with its runtime dependencies.
2. Start the Python 3 standard-library mock in the VM:

   ```bash
   mkdir -p -m 0700 ~/psa-tests
   python3 -u gpsa_loopback_fault_server.py \
      --mode stall --port 18765 --events ~/psa-tests/gpsa-events.jsonl
   ```

3. In another VM terminal run the complete real-SDK integration test:

   ```bash
   export HOME="$HOME/psa-tests/temporary-home"
   mkdir -p -m 0700 "$HOME"
   export MEGACMD_TEST_GPSA_API_URL=http://127.0.0.1:18765/
   export MEGACMD_TEST_SHUTDOWN_WATCHDOG_SECS=25
   timeout --kill-after=5s 65s /path/to/mega-cmd-tests-integration \
      --gtest_filter=NOINTERACTIVEBasicTest.PsaActualSdkTransportCanStallWithoutBlockingPetitions
   ```

4. Verify the mock's JSONL event log contains at least one `{"actions":["gpsa"], ...}` request. The regression also asserts **only one** executor-side SDK PSA request, despite client greetings and repeated `psa` commands.
5. Repeat with `--mode drop`: each SDK HTTP connection is closed without a response. The SDK may retry the same `gpsa` request many times. This is **not** the same as allocating additional executor listeners.
6. Repeat with `--mode late --delay 8`, `MEGACMD_TEST_GPSA_GATE_DEADLINE_MS=3000` and `MEGACMD_TEST_GPSA_EXPECT_LATE_CALLBACK=1`. The **Debug-only** gate deadline is 3 seconds, and the mock response after 8 seconds should be discarded with the test-only rejection counter incremented. The **production** gate remains 30 seconds. Responses delayed over ~20 seconds are unusable as a late-callback test because the underlying SDK retries the HTTP request first.

All network endpoints are bound to `127.0.0.1` and are inside the disposable VM. QEMU was launched with `restrict=on` and only a host-loopback SSH forward. Do not point the SDK API override at an external host or run the test with the production session.

## VM and service acceptance evidence (7 November 2026)

- QEMU/KVM disposable user-owned VM: official AlmaLinux 10.2 GenericCloud October 2026 image (SHA-256 `cbee4726fc0f5a60694bb4f289003cc221e715dfbacebcce76b54daf386d8eca`), 2 vCPUs, 4 GiB RAM, 24 GiB sparse overlay, SELinux enforcing.
- No external internet route or production credentials. Ephemeral SSH key and temporary cloud-init account.
- Stall test: actual SDK `gpsa` POST observed by mock; 12 client commands continued, request bounded to one at the executor.
- Drop test: actual SDK `gpsa` POST observed; repeated CURL error 52 (empty HTTP response), 20 transport retries, client commands remained responsive, only one executor-side request. This does **not** prove MEGA's backend would exhibit the same retries.
- Delayed response: the first attempt with a **32-second** mock delay could not exercise callback expiry; the SDK's individual HTTP request timed out and retried after approximately 20 seconds, so no callback arrived. The corrected test used `MEGACMD_TEST_PSA_GATE_DEADLINE_MS=3000` (Debug only) and `--delay 8`; a genuine SDK HTTP response arrived after the shortened delivery window, incremented the test-only late-rejection counter and **passed**. Production code retains the 30-second deadline.
- Podman installed into VM from an offline RPM repository assembled on .eu, with package signatures verified at download. The VM's **real systemd user manager** generated and started the tracked Quadlet under SELinux enforcing. Host-visible socket, `mega-exec version`, Podman health check and `systemctl --user stop` all passed; graceful shutdown took around three seconds.
- The running host `qemu:///system` guest and production jump-box MEGAcmd were not accessed or modified by these tests.

## Limits and safety

The anonymous test flag bypasses only the account guard in the *test build*, not real account authorization on MEGA's service. It is sufficient to demonstrate the SDK request/HTTP transport and dispatcher separation; it does not replace authenticated testing of transfers, login, session restoration, quotas, or recoveries.

After testing, stop and destroy the disposable VM or retain it stopped only for a separately authorised follow-up. Do not commit guest SSH private keys, raw images, credentials, cache files, or unfiltered SDK debug logs to Git. Preserve sanitised test summaries and version hashes only.
