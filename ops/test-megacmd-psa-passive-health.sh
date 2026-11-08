#!/usr/bin/env bash
# Regression for the SELinux-confined Quadlet's *non-spawning* health probe.
# Uses only disposable, network-disabled state. No production MEGA credentials.
set -euo pipefail
umask 077
repo=$(cd "$(dirname "$0")/.." && pwd)
unit="$repo/ops/prototypes/megacmd-psa-confined.container"
image=localhost/megacmd-psa-alma10-runtime:2.7
podman image exists "$image" || { echo "Missing runtime image" >&2; exit 2; }
health=$(sed -n 's/^HealthCmd=//p' "$unit")
[[ "$health" == "/opt/megacmd-psa-full-release/mega-exec completion version" ]] || { echo "Unexpected non-autospawning health command" >&2; exit 3; }
[[ "$health" != *"mega-exec version"* ]] || { echo "Health command must not use auto-starting version" >&2; exit 4; }
scratch=$(mktemp -d /tmp/mpsa-health-XXXXXXXX)
chmod 0700 "$scratch"
stale="$scratch/stale"
running="$scratch/running"
mkdir -m 0700 "$stale" "$running"
name1="psa-health-negative-$$"
name2="psa-health-positive-$$"
cleanup() {
    set +e
    for name in "$name1" "$name2"; do
        if podman container exists "$name"; then
            if [[ "$name" == "$name2" ]]; then
                timeout --kill-after=2s 10s podman exec "$name" /opt/megacmd-psa-full-release/mega-exec exit >/dev/null 2>&1
            fi
            podman stop --time 8 "$name" >/dev/null 2>&1
            podman rm -f "$name" >/dev/null 2>&1
        fi
    done
    rm -rf -- "$scratch"
}
trap cleanup EXIT
for name in "$name1" "$name2"; do
    if podman container exists "$name"; then echo "Refusing existing test container $name" >&2; exit 5; fi
done
podman_base() {
    podman run --rm --pull never --network none --read-only --cap-drop all \
        --security-opt no-new-privileges --pids-limit 512 \
        --memory 1536m --memory-swap 1536m \
        --tmpfs /tmp:rw,nosuid,nodev,size=128m \
        --tmpfs /run:rw,nosuid,nodev,size=16m \
        --health-cmd "$health" --health-interval 45s --health-timeout 8s "$@"
}
# Negative: no socket, must be reported unhealthy, with no server startup.
podman_base -d --name "$name1" -v "$stale:/home/almalinux/.megaCmd:Z" \
    --entrypoint /usr/bin/python3 "$image" -u -c \
    'import signal,time,sys; signal.signal(signal.SIGTERM, lambda *_: sys.exit(0)); time.sleep(50)' >/dev/null
if podman healthcheck run "$name1" >"$scratch/negative.out" 2>&1; then
    echo "FAIL: missing socket was reported healthy" >&2; exit 6
fi
test ! -e "$stale/megacmd.socket"
[[ "$(podman inspect "$name1" --format '{{.State.Running}}')" == true ]]
echo "PASS: missing socket is unhealthy; completion does not spawn a server"
# Negative fixture only sleeps, no database: remove it directly, without
# waiting for SIGTERM. This is NOT testing a MEGAcmd shutdown.
podman rm -f "$name1" >/dev/null
# Positive: real Release daemon, no account or outbound network.
podman_base -d --name "$name2" -v "$running:/home/almalinux/.megaCmd:Z" \
    "$image" >/dev/null
for i in $(seq 1 60); do
    [[ -S "$running/megacmd.socket" ]] && break
    if [[ "$(podman inspect "$name2" --format '{{.State.Running}}' 2>/dev/null || echo false)" != true ]]; then
        echo "FAIL: server exited without socket" >&2; exit 7
    fi
    sleep 0.3
done
test -S "$running/megacmd.socket"
podman healthcheck run "$name2" >/dev/null 2>&1
echo "PASS: live server answers non-autospawning completion readiness"
timeout --kill-after=3s 15s podman exec "$name2" \
    /opt/megacmd-psa-full-release/mega-exec version >"$scratch/version.out" 2>&1
grep -Fq 2.7.0 "$scratch/version.out"
echo "PASS: patched Release daemon answered version"
timeout --kill-after=3s 15s podman exec "$name2" \
    /opt/megacmd-psa-full-release/mega-exec exit >"$scratch/exit.out" 2>&1
for i in $(seq 1 50); do
    [[ "$(podman inspect "$name2" --format '{{.State.Running}}' 2>/dev/null || echo false)" != true ]] && break
    sleep 0.2
done
[[ "$(podman inspect "$name2" --format '{{.State.Running}}' 2>/dev/null || echo false)" != true ]] || {
    echo "FAIL: graceful shutdown timed out" >&2; exit 8
}
echo "PASS: patched Release daemon exited cleanly"
echo "PASS: passive health regression completed with temporary files only"