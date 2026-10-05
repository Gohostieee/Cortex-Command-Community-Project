#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "$0")/../.." && pwd)
output_root=${1:-"$task_root/build-mp/dedicated-linux"}
[[ -x "$output_root/game/CortexCommand" && -x "$output_root/broker/cc-room-service" ]] || { echo 'Build the Linux game and broker before packaging.' >&2; exit 1; }
stage=$(mktemp -d "$output_root/package.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
mkdir -p "$stage/runtime"
mkdir -p "$stage/runtime/Mods" "$stage/runtime/ScreenShots"
cp "$output_root/game/CortexCommand" "$output_root/broker/cc-room-service" "$output_root/broker/relay-tests" "$stage/runtime/"
cp -a "$task_root/Data" "$task_root/Licences" "$task_root/LICENSE" "$stage/runtime/"
cp "$task_root/external/lib/linux/x86_64/libfmod.so.13.20" "$stage/runtime/"
ln -s libfmod.so.13.20 "$stage/runtime/libfmod.so.13"
ln -s libfmod.so.13.20 "$stage/runtime/libfmod.so"
cp "$task_root/Services/DedicatedServer/Start.sh" "$stage/runtime/cc-start"
sed -i 's/\r$//' "$stage/runtime/cc-start"
chmod 755 "$stage/runtime/cc-start"
revision=$(git -C "$task_root" rev-parse HEAD 2>/dev/null || cat "$task_root/SOURCE-REVISION.txt" 2>/dev/null || echo unknown)
printf '{"platform":"ubuntu-24.04-x86_64","sourceRevision":"%s","schema":1}\n' "$revision" > "$stage/runtime/release.json"
(cd "$stage/runtime" && find Data -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) > "$stage/runtime/data.sha256"
# Include the exact release source for reproducible builds and license compliance.
tar -C "$task_root" --exclude='*/.git' --exclude='*.pdb' --exclude='*.obj' \
    -czf "$stage/runtime/source.tar.gz" Source external Services/RoomService Services/DedicatedServer Tests Resources \
    meson.build meson_options.txt LICENSE Licences
archive="$output_root/cc-dedicated-ubuntu24-x86_64.tar.gz"
tar -C "$stage/runtime" -czf "$archive" .
sha256sum "$archive" > "$archive.sha256"
printf 'DEDICATED_ARCHIVE=%s\n' "$archive"
printf 'DEDICATED_SHA256=%s\n' "$(sha256sum "$archive" | cut -d ' ' -f1)"
