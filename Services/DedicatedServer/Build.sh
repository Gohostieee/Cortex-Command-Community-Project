#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "$0")/../.." && pwd)
output_root=${1:-"$task_root/build-mp/dedicated-linux"}
jobs=${CC_BUILD_JOBS:-4}
if [[ ! $jobs =~ ^[1-9][0-9]*$ || $jobs -gt 32 ]]; then
    echo 'CC_BUILD_JOBS must be an integer between 1 and 32.' >&2
    exit 1
fi
if [[ $(uname -m) != x86_64 ]]; then
    echo 'Build on x86_64 for the supported AWS worker fleet.' >&2
    exit 1
fi
source /etc/os-release
if [[ ${ID:-} != ubuntu || ${VERSION_ID:-} != 24.04 ]]; then
    echo 'Build on Ubuntu 24.04 to match the runtime ABI.' >&2
    exit 1
fi
case "$task_root" in /mnt/[a-z]/*) echo 'Copy the source into the Linux filesystem before building; Windows Git link files require Linux repair.' >&2; exit 1;; esac
if [[ -x /opt/cc-dedicated-toolchain/bin/meson ]]; then
    export PATH="/opt/cc-dedicated-toolchain/bin:$PATH"
fi
mkdir -p "$output_root"
output_root=$(cd "$output_root" && pwd)
# Source archives made from Windows can carry CRLF shell scripts. Some bundled
# configure probes execute those scripts and otherwise silently pick a wrong ABI.
while IFS= read -r -d '' script; do
    if LC_ALL=C grep -q $'\r$' "$script"; then sed -i 's/\r$//' "$script"; fi
done < <(find "$task_root/external" "$task_root/Resources" "$task_root/Services/DedicatedServer" -type f -name '*.sh' -print0)
# Windows Git checkouts may materialize these links as short text files.
# Repair them only in this Linux source copy, never in the Windows checkout.
for library in libfmod.so libfmod.so.13; do
    library_path="$task_root/external/lib/linux/x86_64/$library"
    if [[ ! -L $library_path && $(wc -c < "$library_path") -lt 128 ]]; then
        link_target=$(cat "$library_path")
        if [[ $link_target != libfmod.so.13.20 ]]; then
            echo 'Unexpected FMOD link target.' >&2
            exit 1
        fi
        rm -- "$library_path"
        ln -s -- "$link_target" "$library_path"
    fi
done
# Invalidate compiler inputs by content. Copying a Windows source snapshot with
# preserved timestamps can otherwise leave an older object newer than new text.
python3 - "$task_root" "$output_root" <<'PY'
import hashlib, json, pathlib, sys
root, output = map(pathlib.Path, sys.argv[1:])
previous_path = output / 'built-inputs.json'
previous = json.loads(previous_path.read_text()) if previous_path.exists() else {}
paths = [p for area in ('Source', 'Services/RoomService', 'Tests') for p in (root / area).rglob('*') if p.is_file()]
paths += [root / 'meson.build', root / 'meson_options.txt']
current = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
changed = 0
for path in paths:
    relative = path.relative_to(root).as_posix()
    if current[relative] != previous.get(relative):
        path.touch()
        changed += 1
(output / 'building-inputs.json').write_text(json.dumps(current, sort_keys=True))
print(f'Content checked {len(current)} build inputs; refreshed {changed} changed input timestamps.')
PY
if [[ ! -f "$output_root/game/build.ninja" ]]; then
    meson setup "$output_root/game" "$task_root" --buildtype=release -Dinstall_data=false -Dinstall_runner=false -Dtracy_enable=true -Dtracy:tracy_enable=true -Dtracy:on_demand=true
else
    meson setup --reconfigure "$output_root/game" "$task_root" -Dtracy_enable=true -Dtracy:tracy_enable=true -Dtracy:on_demand=true
fi
meson compile -C "$output_root/game" -j "$jobs" CortexCommand multiplayer-tests
meson test -C "$output_root/game" --print-errorlogs
cmake -S "$task_root/Services/RoomService" -B "$output_root/broker" -DCMAKE_BUILD_TYPE=Release
cmake --build "$output_root/broker" -j "$jobs"
wait_broker() {
    local metrics=$1
    for attempt in {1..200}; do
        if ! kill -0 "$broker_pid" 2>/dev/null; then echo 'Broker exited before UDP readiness; do not accept negative network tests as successful rejection.' >&2; return 1; fi
        if [[ -s $metrics ]]; then return 0; fi
        sleep 0.01
    done
    echo 'Broker did not publish readiness metrics.' >&2
    return 1
}
test_port=${CC_BROKER_TEST_PORT:-38997}
rm -f "$output_root/broker-test-metrics.json"
"$output_root/broker/cc-room-service" --bind 127.0.0.1 --port "$test_port" --max-rooms 8 --metrics-file "$output_root/broker-test-metrics.json" > "$output_root/broker-tests.log" 2>&1 &
broker_pid=$!
trap 'kill "$broker_pid" 2>/dev/null || true; wait "$broker_pid" 2>/dev/null || true' EXIT
wait_broker "$output_root/broker-test-metrics.json"
"$output_root/broker/relay-tests" "$test_port"
kill "$broker_pid"
wait "$broker_pid" || true
trap - EXIT
hosted_test_port=${CC_HOSTED_BROKER_TEST_PORT:-38996}
hosted_state=$(mktemp -d "$output_root/hosted-test.XXXXXX")
"$output_root/broker/cc-room-service" --bind 127.0.0.1 --port "$hosted_test_port" --max-rooms 8 \
    --game-executable "$output_root/broker/relay-tests" --game-directory "$task_root" \
    --hosted-address 127.0.0.1 --game-port-base 39010 --max-hosted 1 --state-directory "$hosted_state" --metrics-file "$hosted_state/broker-metrics.json" \
    > "$output_root/hosted-broker-tests.log" 2>&1 &
broker_pid=$!
trap 'kill "$broker_pid" 2>/dev/null || true; wait "$broker_pid" 2>/dev/null || true; rm -rf -- "$hosted_state"' EXIT
wait_broker "$hosted_state/broker-metrics.json"
"$output_root/broker/relay-tests" --hosted "$hosted_test_port" 127.0.0.1 "$hosted_state"
kill "$broker_pid"
wait "$broker_pid" || true
rm -rf -- "$hosted_state"
trap - EXIT
unavailable_test_port=${CC_UNAVAILABLE_BROKER_TEST_PORT:-38995}
unavailable_state=$(mktemp -d "$output_root/unavailable-test.XXXXXX")
printf 'Invalid native executable fixture\n' > "$unavailable_state/cannot-launch"
chmod 755 "$unavailable_state/cannot-launch"
"$output_root/broker/cc-room-service" --bind 127.0.0.1 --port "$unavailable_test_port" --max-rooms 8 \
    --game-executable "$unavailable_state/cannot-launch" --game-directory "$task_root" \
    --hosted-address 127.0.0.1 --game-port-base 39011 --max-hosted 1 --state-directory "$unavailable_state" --metrics-file "$unavailable_state/broker-metrics.json" \
    > "$output_root/unavailable-broker-tests.log" 2>&1 &
broker_pid=$!
trap 'kill "$broker_pid" 2>/dev/null || true; wait "$broker_pid" 2>/dev/null || true; rm -rf -- "$unavailable_state"' EXIT
wait_broker "$unavailable_state/broker-metrics.json"
"$output_root/broker/relay-tests" --hosted-unavailable "$unavailable_test_port" 127.0.0.1 "$unavailable_state"
kill "$broker_pid"
wait "$broker_pid" || true
rm -rf -- "$unavailable_state"
trap - EXIT
startup_test_port=${CC_STARTUP_BROKER_TEST_PORT:-38993}
startup_state=$(mktemp -d "$output_root/startup-test.XXXXXX")
"$output_root/broker/cc-room-service" --bind 127.0.0.1 --port "$startup_test_port" --max-rooms 8 \
    --game-executable "$output_root/broker/relay-tests" --game-directory "$task_root" \
    --hosted-address 127.0.0.1 --game-port-base 39013 --max-hosted 1 --startup-seconds 2 --state-directory "$startup_state" --metrics-file "$startup_state/broker-metrics.json" \
    > "$output_root/startup-broker-tests.log" 2>&1 &
broker_pid=$!
trap 'kill "$broker_pid" 2>/dev/null || true; wait "$broker_pid" 2>/dev/null || true; rm -rf -- "$startup_state"' EXIT
wait_broker "$startup_state/broker-metrics.json"
"$output_root/broker/relay-tests" --hosted-startup-timeout "$startup_test_port" 127.0.0.1 "$startup_state"
kill "$broker_pid"
wait "$broker_pid" || true
rm -rf -- "$startup_state"
trap - EXIT
python3 - "$task_root" "$output_root" <<'PY'
import hashlib, json, os, pathlib, sys
root, output = map(pathlib.Path, sys.argv[1:])
paths = [p for area in ('Source', 'Services/RoomService', 'Tests') for p in (root / area).rglob('*') if p.is_file()]
paths += [root / 'meson.build', root / 'meson_options.txt']
current = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
snapshot = output / 'building-inputs.json'
if current != json.loads(snapshot.read_text()):
    raise SystemExit('Build inputs changed while compiling/testing; refuse to package a mixed release.')
os.replace(snapshot, output / 'built-inputs.json')
print('Final source digests match the pre-build snapshot.')
PY
bash "$task_root/Services/DedicatedServer/Package.sh" "$output_root"
