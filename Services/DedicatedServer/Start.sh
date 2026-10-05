#!/usr/bin/env bash
set -euo pipefail
game_root=$(cd "$(dirname "$0")" && pwd)
state_root=${CC_STATE_DIRECTORY:-/var/lib/cc-dedicated}
service_port=${CC_SERVICE_PORT:-8001}
game_port_base=${CC_GAME_PORT_BASE:-8100}
max_hosted=${CC_MAX_HOSTED:-2}
hosted_address=${CC_HOSTED_ADDRESS:?Set CC_HOSTED_ADDRESS to the publicly reachable worker address.}
mkdir -p "$state_root"
export LD_LIBRARY_PATH="$game_root${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_AUDIODRIVER=dummy
export CC_DEDICATED_STATE_ROOT="$state_root"
cd "$game_root"
exec "$game_root/cc-room-service" --port "$service_port" --max-rooms 16 --max-mbps 200 \
    --game-executable "$game_root/CortexCommand" --game-directory "$game_root" \
    --hosted-address "$hosted_address" --game-port-base "$game_port_base" --max-hosted "$max_hosted" \
    --state-directory "$state_root/rooms" --metrics-file "$state_root/metrics.json"
