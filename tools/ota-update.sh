#!/usr/bin/env bash
# ota-update.sh — wireless firmware/filesystem update for the WiFi-remote-for-Bestway-Lay-Z-SPA.
#
# Wraps espota.py (from framework-arduinoespressif8266) so users without
# PlatformIO/Arduino IDE can push updates from any LAN-reachable host with
# Python 3 (Pi, Linux box, Mac, HA shell, etc.).
#
# espota.py is NOT bundled in this repo — point ESPOTA_PATH at your copy.
# It ships with PlatformIO at:
#   ~/.platformio/packages/framework-arduinoespressif8266/tools/espota.py
# or grab it from https://github.com/esp8266/Arduino/blob/master/tools/espota.py
#
# Usage:
#   ota-update.sh -i 192.168.1.42 -f firmware.bin
#   ota-update.sh -i 192.168.1.42 -s -f littlefs.bin     # filesystem image
#   ota-update.sh -i layzspa.local -p secret -f firmware.bin
#
# Cellular link gotcha:
# espota's UDP INVITE is one packet. If your spa is on a cellular hotspot or
# a distant mesh, that packet drops ~30-40% of the time. This script retries
# the whole espota invocation up to $ESPOTA_INVITE_RETRIES times (default 5)
# until INVITE goes through. Once the TCP transfer starts, it's reliable.

set -euo pipefail

DEVICE_IP=""
PASSWORD="layzspam0dule"
IMAGE=""
SPIFFS_FLAG=""
DEVICE_PORT=8266
HOST_PORT=18266
ESPOTA_PATH="${ESPOTA_PATH:-}"
RETRIES="${ESPOTA_INVITE_RETRIES:-5}"

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-1}"
}

while (($#)); do
    case "$1" in
        -i|--ip)        DEVICE_IP="$2"; shift 2 ;;
        -p|--password)  PASSWORD="$2"; shift 2 ;;
        -f|--file)      IMAGE="$2"; shift 2 ;;
        -s|--spiffs)    SPIFFS_FLAG="-s"; shift ;;
        -P|--port)      DEVICE_PORT="$2"; shift 2 ;;
        --espota-path)  ESPOTA_PATH="$2"; shift 2 ;;
        -h|--help)      usage 0 ;;
        *)              echo "Unknown arg: $1" >&2; usage 1 ;;
    esac
done

[[ -n "$DEVICE_IP" ]] || { echo "ERROR: --ip required" >&2; exit 1; }
[[ -n "$IMAGE" && -f "$IMAGE" ]] || { echo "ERROR: --file must point to an existing image" >&2; exit 1; }

if [[ -z "$ESPOTA_PATH" ]]; then
    for c in \
        "$HOME/.platformio/packages/framework-arduinoespressif8266/tools/espota.py" \
        "$HOME/Arduino/hardware/esp8266com/esp8266/tools/espota.py" \
        "/usr/share/arduino/hardware/esp8266com/esp8266/tools/espota.py"
    do
        if [[ -f "$c" ]]; then ESPOTA_PATH="$c"; break; fi
    done
fi
[[ -n "$ESPOTA_PATH" && -f "$ESPOTA_PATH" ]] \
    || { echo "ERROR: espota.py not found. Set ESPOTA_PATH or pass --espota-path" >&2; exit 1; }

command -v python3 >/dev/null \
    || { echo "ERROR: python3 not in PATH" >&2; exit 1; }

what="firmware"
[[ "$SPIFFS_FLAG" == "-s" ]] && what="filesystem"
size=$(stat -c%s "$IMAGE" 2>/dev/null || stat -f%z "$IMAGE")  # Linux + macOS

echo "Pushing ${what} to ${DEVICE_IP}:${DEVICE_PORT} (${size} bytes, retries=${RETRIES})"

attempt=0
while (( ++attempt <= RETRIES )); do
    echo "--- attempt ${attempt}/${RETRIES} ---"
    if python3 "$ESPOTA_PATH" \
        -i "$DEVICE_IP" -p "$DEVICE_PORT" -P "$HOST_PORT" \
        -a "$PASSWORD" $SPIFFS_FLAG -f "$IMAGE" -r
    then
        echo "OK — uploaded ${what} on attempt ${attempt}"
        exit 0
    fi
    echo "espota.py failed (typically a dropped UDP INVITE — retrying)"
    sleep 2
done

echo "ERROR: gave up after ${RETRIES} attempts" >&2
exit 1
