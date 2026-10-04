#!/bin/bash
# Build the device firmware and flash it over WiFi, no USB cable needed:
#   tools/ota_upload.sh [board address, default esp32.local]
# The key comes from include/wifi_config.h (OTA_KEY), which stays out of git.
set -e
cd "$(dirname "$0")/.."
HOST="${1:-esp32.local}"
KEY=$(sed -n 's/.*OTA_KEY = "\(.*\)".*/\1/p' include/wifi_config.h)
[ -n "$KEY" ] || { echo "OTA_KEY missing from include/wifi_config.h" >&2; exit 1; }
export PATH="$HOME/.platformio/penv/bin:$PATH"
pio run -e device
echo "Uploading to $HOST..."
curl -sS --max-time 180 -F "firmware=@.pio/build/device/firmware.bin" "http://$HOST/ota?key=$KEY"
echo
