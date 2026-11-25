#!/bin/bash
# Push a freshly built NRO straight to the console over the network, no sd card shuffling.
#
# On the console: open hbmenu (album, or title takeover) and press Y to start netloader.
# hbmenu will show "waiting for connection". Then run this script.
#
#   SWITCH_IP=192.168.0.99 ./deploy.sh                    # sends build/SwitchWave.nro
#   SWITCH_IP=192.168.0.99 ./deploy.sh path/to/other.nro  # send a different nro
#
# nxlink lives inside the toolchain image, so nothing needs installing on the mac.
# --network host is what lets the container reach the console on the lan.

set -eo pipefail

SWITCH_IP="${SWITCH_IP:?set SWITCH_IP to the console address, e.g. SWITCH_IP=192.168.0.99 ./deploy.sh}"
NRO="${1:-build/SwitchWave.nro}"

if [ ! -f "$NRO" ]; then
    echo "error: $NRO not found -- run ./build-docker.sh first" >&2
    exit 1
fi

if ! docker image inspect switchwave-builder >/dev/null 2>&1; then
    echo "error: switchwave-builder image missing -- run ./build-docker.sh first" >&2
    exit 1
fi

echo "Sending $NRO to $SWITCH_IP ..."

# nxlink returns promptly if netloader isn't listening, so surface that clearly
if ! docker run --rm --network host -v "$(pwd)":/mnt switchwave-builder \
        /opt/devkitpro/tools/bin/nxlink -a "$SWITCH_IP" "/mnt/$NRO"; then
    echo >&2
    echo "Failed to send. Check that:" >&2
    echo "  - hbmenu is open on the console and you pressed Y for netloader" >&2
    echo "  - the console is on the same network as this mac ($SWITCH_IP)" >&2
    exit 1
fi

echo "Sent. The console should be launching it now."
