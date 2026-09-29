#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "rgb_led.h"
#include "wifi_config.h"

WebServer server(80);

uint8_t curR, curG, curB;

void applyColor(uint8_t r, uint8_t g, uint8_t b) {
    curR = r;
    curG = g;
    curB = b;
    setLed(r, g, b);
}

// Each preset: data-hex is what the LED gets, background is how it looks on screen
// (a WS2812 renders e.g. orange/yellow differently from a monitor).
const char *pageHtml = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Đèn RGB ESP32-S3</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin: 24px 16px; }
    .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(96px, 1fr)); gap: 10px; max-width: 520px; margin: 0 auto; }
    button { font-size: 17px; padding: 16px 8px; border: none; border-radius: 10px; color: #fff; text-shadow: 0 1px 2px #0008; }
    .picker { margin: 28px auto 8px; font-size: 18px; }
    input[type=color] { width: 120px; height: 56px; border: none; vertical-align: middle; }
    #cur { font-family: monospace; }
  </style>
</head>
<body>
  <h1>Đèn RGB ESP32-S3</h1>
  <div class="grid">
    <button data-hex="ff0000" style="background:#e53935">Đỏ</button>
    <button data-hex="ff4000" style="background:#fb8c00">Cam</button>
    <button data-hex="ffb000" style="background:#fdd835">Vàng</button>
    <button data-hex="00ff00" style="background:#43a047">Xanh lá</button>
    <button data-hex="00ffff" style="background:#00acc1">Xanh ngọc</button>
    <button data-hex="0000ff" style="background:#1e88e5">Xanh dương</button>
    <button data-hex="8000ff" style="background:#8e24aa">Tím</button>
    <button data-hex="ff0060" style="background:#ec407a">Hồng</button>
    <button data-hex="ffffff" style="background:#bdbdbd">Trắng</button>
    <button data-hex="000000" style="background:#424242">Tắt</button>
  </div>
  <div class="picker">Màu tùy chọn: <input type="color" id="picker"> <span id="cur"></span></div>
<script>
  const picker = document.getElementById('picker'), cur = document.getElementById('cur');
  function show(hex) { picker.value = '#' + hex; cur.textContent = '#' + hex; }
  // Send one request at a time; while dragging the picker, only the latest color is queued.
  let busy = false, queued = null;
  function setColor(hex) {
    if (busy) { queued = hex; return; }
    busy = true;
    fetch('/set?hex=' + hex).then(r => { if (r.ok) show(hex); }).finally(() => {
      busy = false;
      if (queued) { const q = queued; queued = null; setColor(q); }
    });
  }
  document.querySelectorAll('button').forEach(b => b.onclick = () => setColor(b.dataset.hex));
  picker.addEventListener('input', e => setColor(e.target.value.slice(1)));
  fetch('/color').then(r => r.text()).then(show);
</script>
</body>
</html>
)rawliteral";

bool isHexColor(const String &s) {
    if (s.length() != 6) return false;
    for (char c : s) {
        if (!isxdigit((unsigned char)c)) return false;
    }
    return true;
}

const char *wifiStatusName(wl_status_t s) {
    switch (s) {
        case WL_IDLE_STATUS:     return "IDLE";
        case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (SSID not found)";
        case WL_CONNECTED:       return "CONNECTED";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED (wrong password?)";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED:    return "DISCONNECTED";
        default:                 return "UNKNOWN";
    }
}

void connectWiFi() {
    WiFi.mode(WIFI_STA);
    while (true) {
        Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
            setLed(255, 255, 0); // Yellow blink = connecting
            delay(250);
            setLed(0, 0, 0);
            delay(250);
            Serial.printf("  status: %s\n", wifiStatusName(WiFi.status()));
        }
        if (WiFi.status() == WL_CONNECTED) return;
        Serial.printf("WiFi failed: %s. Retrying...\n", wifiStatusName(WiFi.status()));
        setLed(255, 0, 0); // Red = failed, retrying
        WiFi.disconnect();
        delay(2000);
    }
}

void setup() {
    Serial.begin(115200);
    initLed();
    for (int i = 0; i < 2; i++) {
        Serial.printf("LED driver on GPIO%d: %s\n", LED_PINS[i], ledRmt[i] ? "ok" : "FAILED");
    }
    connectWiFi();
    applyColor(0, 255, 0); // Green = connected

    server.on("/", []() { server.send(200, "text/html; charset=utf-8", pageHtml); });
    server.on("/set", []() {
        String hex = server.arg("hex");
        if (!isHexColor(hex)) {
            server.send(400, "text/plain", "hex must be 6 hex digits, e.g. /set?hex=ff8000");
            return;
        }
        uint32_t v = strtoul(hex.c_str(), nullptr, 16);
        applyColor(v >> 16, (v >> 8) & 0xFF, v & 0xFF);
        Serial.printf("LED set to #%s\n", hex.c_str());
        server.send(200, "text/plain", "ok");
    });
    server.on("/color", []() {
        char buf[7];
        snprintf(buf, sizeof(buf), "%02x%02x%02x", curR, curG, curB);
        server.send(200, "text/plain", buf);
    });
    server.begin();
}

void loop() {
    server.handleClient();

    // Re-print the URL periodically so it is visible whenever the monitor is opened.
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 5000) {
        lastPrint = millis();
        Serial.printf("WiFi %s | Open http://%s/ in your browser\n",
            wifiStatusName(WiFi.status()), WiFi.localIP().toString().c_str());
    }
}
