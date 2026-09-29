#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BleKeyboard.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include "rgb_led.h"
#include "wifi_config.h"

#define BOOT_BUTTON 0 // the board's BOOT button, active low

BleKeyboard bleKeyboard("ESP32 Remote", "ESP32-S3", 100);
WebServer server(80);

// BleKeyboard::isConnected() turns true on the raw link, before pairing/encryption finishes,
// but the phone only accepts HID reports over an encrypted (bonded) link. Track that separately.
volatile bool authDone = false;
volatile bool authOk = false;
volatile uint8_t authFailReason = 0;

class SecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t) override {}
    bool onSecurityRequest() override { return true; }
    bool onConfirmPIN(uint32_t) override { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
        authOk = cmpl.success;
        authFailReason = cmpl.fail_reason;
        authDone = true;
    }
};

// "disconnected" | "pairing" (linked, not yet encrypted) | "ready" | "auth_failed:0x..".
String linkState() {
    if (!bleKeyboard.isConnected()) return "disconnected";
    if (!authDone) return "pairing";
    if (authOk) return "ready";
    char buf[24];
    snprintf(buf, sizeof(buf), "auth_failed:0x%02x", authFailReason);
    return buf;
}

// Standard HID-device pairing (as in ESP-IDF's ble_hid_device_demo): no display/keypad, so
// "Just Works" bonding, exchanging encryption + identity keys so the board can recognize the
// phone's rotating private address on reconnect. Overrides the library's MITM request, which
// a device without IO can't satisfy anyway.
void configurePairing() {
    BLESecurity *sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    sec->setCapability(ESP_IO_CAP_NONE);
    sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
}

void removeAllBonds() {
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) return;
    esp_ble_bond_dev_t *list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * n);
    if (!list) return;
    esp_ble_get_bond_device_list(&n, list);
    for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
    free(list);
}

// Returns true if HID reports can be delivered; otherwise sends the reason to the web client.
bool requireReady() {
    String s = linkState();
    if (s == "ready") return true;
    const char *why = s == "disconnected" ? "Điện thoại chưa kết nối Bluetooth"
                    : s == "pairing"      ? "Đã kết nối nhưng chưa ghép nối bảo mật xong, đợi vài giây"
                                          : "Ghép nối bảo mật thất bại: trên điện thoại hãy Quên thiết bị ESP32 Remote rồi ghép lại";
    server.send(409, "text/plain; charset=utf-8", why);
    return false;
}

// Phone camera apps take a photo on "volume up", which is what makes this a selfie remote.
struct MediaAction {
    const char *name;
    const uint8_t *key;
};
const MediaAction ACTIONS[] = {
    {"camera", KEY_MEDIA_VOLUME_UP},
    {"playpause", KEY_MEDIA_PLAY_PAUSE},
    {"next", KEY_MEDIA_NEXT_TRACK},
    {"prev", KEY_MEDIA_PREVIOUS_TRACK},
    {"volup", KEY_MEDIA_VOLUME_UP},
    {"voldown", KEY_MEDIA_VOLUME_DOWN},
    {"mute", KEY_MEDIA_MUTE},
};

const char *pageHtml = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Remote Bluetooth ESP32</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin: 24px 16px; }
    .wrap { max-width: 480px; margin: 0 auto; }
    #st { font-weight: bold; padding: 8px; border-radius: 8px; background: #eee; }
    #st.connected { background: #c8e6c9; color: #1b5e20; }
    #st.disconnected { background: #ffcdd2; color: #b71c1c; }
    button { font-size: 17px; padding: 16px 8px; border: none; border-radius: 10px; background: #1e88e5; color: #fff; }
    .big { width: 100%; font-size: 24px; padding: 24px; margin: 16px 0; background: #e53935; }
    .grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }
    textarea { width: 100%; box-sizing: border-box; font-size: 16px; padding: 8px; margin: 8px 0; }
    #send { width: 100%; background: #43a047; }
    #msg { min-height: 1.2em; color: #555; }
    .hint { color: #777; font-size: 14px; text-align: left; }
  </style>
</head>
<body>
<div class="wrap">
  <h1>Remote Bluetooth</h1>
  <p id="st">Đang kiểm tra...</p>
  <button class="big" data-k="camera">Chụp ảnh</button>
  <div class="grid">
    <button data-k="prev">Bài trước</button>
    <button data-k="playpause">Phát / Dừng</button>
    <button data-k="next">Bài tiếp</button>
    <button data-k="voldown">Âm lượng -</button>
    <button data-k="mute">Tắt tiếng</button>
    <button data-k="volup">Âm lượng +</button>
  </div>
  <h2>Gõ chữ sang điện thoại</h2>
  <textarea id="txt" rows="3" placeholder="Mở ô nhập chữ trên điện thoại trước. Chữ có dấu sẽ tự thành không dấu."></textarea>
  <button id="send">Gửi</button>
  <p id="msg"></p>
  <p class="hint">Ghép nối: vào Cài đặt &gt; Bluetooth trên điện thoại, chọn "ESP32 Remote".<br>
  Chụp ảnh: mở app Camera rồi bấm nút trên, hoặc nút BOOT trên board.<br>
  Ghép nối bị lỗi: bấm nút dưới, rồi trên điện thoại Quên thiết bị ESP32 Remote và ghép lại.</p>
  <button id="unpair" style="background:#757575">Xóa ghép nối trên board</button>
</div>
<script>
  const st = document.getElementById('st'), msg = document.getElementById('msg'), txt = document.getElementById('txt');
  function post(url, data) {
    fetch(url, { method: 'POST', body: new URLSearchParams(data) })
      .then(r => r.text()).then(t => { msg.textContent = t; });
  }
  document.querySelectorAll('button[data-k]').forEach(b => b.onclick = () => post('/key', { k: b.dataset.k }));
  // A BLE keyboard can only type ASCII: strip Vietnamese diacritics before sending.
  document.getElementById('send').onclick = () => {
    const plain = txt.value.normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/đ/g, 'd').replace(/Đ/g, 'D');
    post('/type', { text: plain });
  };
  document.getElementById('unpair').onclick = () => {
    if (confirm('Xóa ghép nối Bluetooth trên board?')) post('/unpair', {});
  };
  function poll() {
    fetch('/status').then(r => r.text()).then(s => {
      const ready = s === 'ready';
      st.className = ready ? 'connected' : 'disconnected';
      st.textContent = ready ? 'Điện thoại: đã kết nối, sẵn sàng'
        : s === 'pairing' ? 'Điện thoại: đã kết nối, đang ghép nối bảo mật...'
        : s.startsWith('auth_failed') ? 'Ghép nối bảo mật thất bại (' + s.split(':')[1] + '): Quên thiết bị ESP32 Remote trên điện thoại rồi ghép lại'
        : 'Điện thoại: chưa kết nối Bluetooth';
    });
  }
  poll(); setInterval(poll, 2000);
</script>
</body>
</html>
)rawliteral";

void sendText(int code, const char *text) {
    server.send(code, "text/plain; charset=utf-8", text);
}

void connectWiFi() {
    WiFi.mode(WIFI_STA);
    while (true) {
        Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
        if (WiFi.status() == WL_CONNECTED) return;
        Serial.printf("WiFi failed (status %d), retrying...\n", WiFi.status());
        WiFi.disconnect();
        delay(2000);
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(BOOT_BUTTON, INPUT_PULLUP);

    // A WS2812 keeps its last color across a chip reset; this demo doesn't use it, so turn it off.
    initLed();
    setLed(0, 0, 0);

    bleKeyboard.begin();
    configurePairing();
    BLEDevice::setSecurityCallbacks(new SecurityCallbacks());
    connectWiFi();

    server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", pageHtml); });
    server.on("/status", HTTP_GET, []() { sendText(200, linkState().c_str()); });
    server.on("/bonds", HTTP_GET, []() { sendText(200, String(esp_ble_get_bond_device_num()).c_str()); });
    server.on("/unpair", HTTP_POST, []() {
        removeAllBonds();
        Serial.println("Removed all BLE bonds");
        sendText(200, "Đã xóa ghép nối trên board. Trên điện thoại hãy Quên thiết bị ESP32 Remote rồi ghép lại.");
    });
    server.on("/key", HTTP_POST, []() {
        String k = server.arg("k");
        for (const MediaAction &a : ACTIONS) {
            if (k != a.name) continue;
            if (!requireReady()) return;
            bleKeyboard.write(a.key);
            Serial.printf("Sent key: %s\n", a.name);
            return sendText(200, "Đã gửi");
        }
        sendText(400, "Nút không hợp lệ");
    });
    server.on("/type", HTTP_POST, []() {
        String text;
        for (char c : server.arg("text")) {
            if (c == '\n' || (c >= 32 && c < 127)) text += c; // ASCII only: other bytes would be sent as raw keycodes
        }
        if (text.isEmpty()) return sendText(400, "Chưa có chữ để gửi");
        if (text.length() > 200) return sendText(400, "Tối đa 200 ký tự");
        if (!requireReady()) return;
        bleKeyboard.print(text);
        Serial.printf("Typed %u chars\n", text.length());
        sendText(200, "Đã gõ sang điện thoại");
    });
    server.begin();
}

void loop() {
    server.handleClient();

    // A new connection must authenticate again: forget the previous result once the link drops.
    static bool wasLinked = false;
    bool linked = bleKeyboard.isConnected();
    if (!linked && wasLinked) authDone = false;
    wasLinked = linked;

    // BOOT button = take a photo. Ignore bounces for 300 ms after a press.
    static bool wasPressed = false;
    static unsigned long lastPressMs = 0;
    bool pressed = digitalRead(BOOT_BUTTON) == LOW;
    if (pressed && !wasPressed && millis() - lastPressMs > 300) {
        lastPressMs = millis();
        if (linkState() == "ready") {
            bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
            Serial.println("BOOT button: sent camera shutter");
        } else {
            Serial.printf("BOOT button pressed, but phone link is %s\n", linkState().c_str());
        }
    }
    wasPressed = pressed;

    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 5000) {
        lastPrint = millis();
        Serial.printf("Phone link: %s | Web remote: http://%s/\n",
            linkState().c_str(), WiFi.localIP().toString().c_str());
    }
}
