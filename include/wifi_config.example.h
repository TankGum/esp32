#pragma once

// Copy this file to wifi_config.h (same folder) and fill in your WiFi.
// wifi_config.h is in .gitignore, so the real credentials never reach the repository.
static const char *WIFI_SSID = "your-wifi-name";
static const char *WIFI_PASSWORD = "your-wifi-password";

// Key for firmware updates over WiFi (POST /ota?key=...), used by tools/ota_upload.sh.
// Pick a long random string; while it is left as is, the board refuses updates over WiFi.
static const char *OTA_KEY = "change-me";

// Secret ntfy.sh topic for controlling the LED from the internet (curl -d red ntfy.sh/<topic>).
// Anyone who knows it can send commands, so make it long and random. Empty = feature off.
static const char *NTFY_TOPIC = "";
