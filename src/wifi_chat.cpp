#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "json_escape.h"
#include "rgb_led.h"
#include "wifi_config.h"

const char *AP_SSID = "ESP32-Chat";
const IPAddress AP_IP(192, 168, 4, 1);

WebServer server(80);
DNSServer dns;

struct Message {
    uint32_t id;
    String name;
    String text;
    unsigned long ms;
};
const int MAX_MESSAGES = 50;
Message messages[MAX_MESSAGES]; // ring buffer, slot = (id - 1) % MAX_MESSAGES
uint32_t nextId = 1;

void addMessage(const String &name, const String &text) {
    messages[(nextId - 1) % MAX_MESSAGES] = {nextId, name, text, millis()};
    nextId++;
}

const char *pageHtml = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Phòng chat ESP32</title>
  <style>
    body { font-family: sans-serif; margin: 0; display: flex; flex-direction: column; height: 100vh; height: 100dvh; }
    header { background: #1e88e5; color: #fff; padding: 12px 16px; }
    header h1 { font-size: 20px; margin: 0; }
    header small { opacity: .85; }
    #log { flex: 1; overflow-y: auto; padding: 12px 16px; background: #f5f5f5; }
    .m { background: #fff; border-radius: 10px; padding: 8px 12px; margin: 6px 0; max-width: 85%; box-shadow: 0 1px 1px #0001; overflow-wrap: anywhere; }
    .m.me { background: #c8e6c9; margin-left: auto; }
    .n { font-weight: bold; font-size: 13px; color: #1565c0; }
    .t { font-size: 11px; color: #999; margin-left: 6px; }
    form { display: flex; gap: 6px; padding: 8px; border-top: 1px solid #ddd; background: #fff; }
    input, button { font-size: 16px; padding: 10px; border: 1px solid #ccc; border-radius: 8px; min-width: 0; }
    #name { width: 28%; }
    #text { flex: 1; }
    button { background: #1e88e5; color: #fff; border: none; }
  </style>
</head>
<body>
  <header><h1>Phòng chat ESP32</h1><small id="info">Không cần internet</small></header>
  <div id="log"></div>
  <form id="f">
    <input id="name" maxlength="20" placeholder="Tên">
    <input id="text" maxlength="200" placeholder="Nhập tin nhắn..." autocomplete="off">
    <button>Gửi</button>
  </form>
<script>
  const log = document.getElementById('log'), nameEl = document.getElementById('name'),
        textEl = document.getElementById('text'), info = document.getElementById('info');
  let last = 0;
  try { nameEl.value = localStorage.getItem('name') || ''; } catch (e) {}
  if (!nameEl.value) nameEl.value = 'Khách ' + Math.floor(Math.random() * 90 + 10);
  // Messages come from strangers: build nodes with textContent, never innerHTML.
  function add(m) {
    const d = document.createElement('div'), n = document.createElement('span'),
          t = document.createElement('span'), b = document.createElement('div');
    d.className = 'm' + (m.name === nameEl.value ? ' me' : '');
    n.className = 'n'; n.textContent = m.name;
    t.className = 't';
    t.textContent = new Date(Date.now() - m.ago * 1000).toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit' });
    b.textContent = m.text;
    d.append(n, t, b);
    log.append(d);
  }
  function poll() {
    fetch('/messages?since=' + last).then(r => r.json()).then(d => {
      const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 30;
      d.messages.forEach(m => { add(m); last = m.id; });
      info.textContent = d.online + ' thiết bị đang kết nối WiFi ESP32-Chat · không cần internet';
      if (d.messages.length && atBottom) log.scrollTop = log.scrollHeight;
    }).catch(() => {});
  }
  document.getElementById('f').onsubmit = e => {
    e.preventDefault();
    const text = textEl.value.trim();
    if (!text) return;
    try { localStorage.setItem('name', nameEl.value); } catch (e) {}
    fetch('/send', { method: 'POST', body: new URLSearchParams({ name: nameEl.value, text }) })
      .then(r => { if (r.ok) { textEl.value = ''; poll(); } else r.text().then(alert); });
  };
  poll(); setInterval(poll, 1500);
</script>
</body>
</html>
)rawliteral";

void sendText(int code, const char *text) {
    server.send(code, "text/plain; charset=utf-8", text);
}

// Phones probe fixed URLs (generate_204, hotspot-detect.html, ...) to detect a login page.
// DNS resolves every name to us, and anything we don't serve is redirected to the chat,
// which makes the phone pop up the chat page right after joining the WiFi.
void redirectToPortal() {
    server.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
    server.send(302, "text/plain", "");
}

void handleMessages() {
    uint32_t since = server.arg("since").toInt();
    uint32_t oldest = nextId > MAX_MESSAGES ? nextId - MAX_MESSAGES : 1;
    unsigned long now = millis();
    String json = "{\"online\":" + String(WiFi.softAPgetStationNum()) + ",\"messages\":[";
    bool first = true;
    for (uint32_t id = max(since + 1, oldest); id < nextId; id++) {
        const Message &m = messages[(id - 1) % MAX_MESSAGES];
        if (!first) json += ",";
        first = false;
        json += "{\"id\":" + String(m.id) + ",\"name\":\"" + jsonEscape(m.name) + "\",\"text\":\"" +
                jsonEscape(m.text) + "\",\"ago\":" + String((now - m.ms) / 1000) + "}";
    }
    json += "]}";
    server.send(200, "application/json; charset=utf-8", json);
}

void handleSend() {
    String name = server.arg("name");
    String text = server.arg("text");
    name.trim();
    text.trim();
    // Limits are in bytes (a Vietnamese letter is 2-3 bytes in UTF-8); reject rather than cut mid-character.
    if (text.isEmpty()) return sendText(400, "Tin nhắn trống");
    if (text.length() > 600) return sendText(400, "Tin nhắn quá dài");
    if (name.isEmpty()) name = "Khách";
    if (name.length() > 60) return sendText(400, "Tên quá dài");
    addMessage(name, text);
    sendText(200, "ok");
}

void setup() {
    Serial.begin(115200);
    initLed();
    setLed(0, 0, 0); // this demo doesn't use the LED; clear whatever the last demo left on

    // Join the home WiFi first (for LAN access), then start our AP on the same channel:
    // the radio can only be on one channel, so a later STA connect would move the AP.
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Joining \"%s\" (up to 10 s)...\n", WIFI_SSID);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
    int channel = WiFi.status() == WL_CONNECTED ? WiFi.channel() : 1;

    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, nullptr, channel, 0, 8);
    dns.start(53, "*", AP_IP);

    addMessage("ESP32", "Chào mừng đến phòng chat! Mọi người đang kết nối WiFi ESP32-Chat đều thấy tin nhắn ở đây.");

    server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", pageHtml); });
    server.on("/messages", HTTP_GET, handleMessages);
    server.on("/send", HTTP_POST, handleSend);
    server.onNotFound(redirectToPortal);
    server.begin();
}

void loop() {
    dns.processNextRequest();
    server.handleClient();

    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 5000) {
        lastPrint = millis();
        Serial.printf("AP \"%s\" ch%d: %d clients at http://%s/ | LAN: %s | messages: %u\n", AP_SSID,
            WiFi.channel(), WiFi.softAPgetStationNum(), AP_IP.toString().c_str(),
            WiFi.status() == WL_CONNECTED ? ("http://" + WiFi.localIP().toString() + "/").c_str() : "not connected",
            nextId - 1);
    }
}
