#!/usr/bin/env bash
# Offline, disposable simulation of the service's Podman settings.
# NEVER mount the real HOME/.megaCmd. No credentials or network access.
set -euo pipefail
IMAGE=localhost/megacmd-psa-alma10-runtime:2.7
REPO=$(cd "$(dirname "$0")/.." && pwd)
name="megacmd-psa-smoke-$$"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/megacmd-psa-offline.XXXXXXXX")
chmod 700 "$scratch"
state="$scratch/state"
mkdir -m 700 "$state"
finish() {
    podman stop --time 12 "$name" >/dev/null 2>&1 || :
    podman rm -f "$name" >/dev/null 2>&1 || :
    rm -rf -- "$scratch"
}
trap finish EXIT
test "$state" != "$HOME/.megaCmd"
podman image exists "$IMAGE"
command -v /usr/libexec/podman/quadlet >/dev/null
# Dry-run the real unit from the repository; never install it.
QUADLET_UNIT_DIRS="$REPO/ops" /usr/libexec/podman/quadlet -user -dryrun >"$scratch/quadlet.out" 2>&1
grep -q 'ExecStart=.*megacmd-psa-alma10-runtime:2.7' "$scratch/quadlet.out"
if grep -q 'WantedBy=default.target' "$scratch/quadlet.out"; then
    echo "ERROR: unexpected autostart target" >&2; exit 2
fi
# All runtime restrictions come from the Quadlet; only state location, name,
# and network are deliberately overridden for this disposable run.
podman run -d --name "$name" --replace --pull never --network none \
  --memory=2g --memory-swap=2g --pids-limit=512 --read-only \
  --security-opt=no-new-privileges --security-opt label=disable \
  --cap-drop=all --tmpfs /tmp:rw,nosuid,nodev,size=256m \
  --tmpfs /run:rw,nosuid,nodev,size=16m \
  --stop-signal SIGTERM --stop-timeout 12 \
  --health-cmd '/opt/megacmd-psa-full-release/mega-exec version' \
  --health-interval 30s --health-timeout 8s --health-retries 3 \
  -v "$state:/home/almalinux/.megaCmd:rw" \
  -e HOME=/home/almalinux --workdir /home/almalinux \
  "$IMAGE" >/dev/null
socket="$state/megacmd.socket"
for i in $(seq 1 60); do
    if test -S "$socket"; then break; fi
    if ! podman inspect "$name" --format '{{.State.Running}}' | grep -q true; then
        echo 'ERROR: test daemon exited' >&2; podman logs "$name" >&2; exit 3
    fi
    sleep .2
done
test -S "$socket" || { echo 'ERROR: no host-visible socket' >&2; exit 4; }
echo "PASS: host sees socket via temporary bind mount"
# A separate container, not podman exec inside the server, uses the shared
# socket to verify that clients could reach it through the host directory.
for command in version help; do
  output=$(timeout --kill-after=2s 9s podman run --rm --network none \
    --read-only --memory=512m --memory-swap=512m --pids-limit=48 \
    --security-opt=no-new-privileges --security-opt label=disable \
    --cap-drop=all -v "$state:/home/almalinux/.megaCmd:ro" \
    -e HOME=/home/almalinux --entrypoint /opt/megacmd-psa-full-release/mega-exec \
    "$IMAGE" "$command")
  if test "$command" = version; then grep -q 'MEGAcmd version: 2.7.0' <<<"$output"; fi
  echo "PASS: independent client container: $command"
done
# Test the health-check command itself.
podman healthcheck run "$name" >/dev/null
echo "PASS: podman healthcheck"
# Test the exact graceful-stop helper against a one-off disposable name.
# The tracked helper never accepts container names as CLI input.
helper="$scratch/graceful-stop"
sed "s/name=megacmd-psa-27/name=$name/" "$REPO/ops/megacmd-psa-graceful-stop" >"$helper"
chmod 0700 "$helper"
"$helper" >"$scratch/stop.out" 2>&1 || { cat "$scratch/stop.out"; exit 6; }
cat "$scratch/stop.out"
if test "$(podman inspect --format '{{.State.Running}}' "$name" 2>/dev/null || echo false)" = true; then
    echo "ERROR: MEGAcmd process still running after graceful helper" >&2
    exit 7
fi
# Quadlet's generated ExecStop removes the stopped Podman metadata with -i.
podman rm -f -i "$name" >/dev/null
echo "PASS: server exited gracefully and Podman metadata was removed"
echo "PASS: no production state, network or credentials used"
