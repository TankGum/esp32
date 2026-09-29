#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <algorithm>
#include <vector>
#include "json_escape.h"
#include "rgb_led.h"

#define BOOT_BUTTON 0 // active low

const char *SETUP_SSID = "ESP32-Setup";
const char *HOSTNAME = "esp32"; // http://esp32.local/ on networks/devices that support mDNS
const IPAddress AP_IP(192, 168, 4, 1);
const unsigned long CONNECT_TIMEOUT_MS = 15000;
const unsigned long AP_GRACE_MS = 60000;              // keep the setup WiFi up so the phone can read the result
const unsigned long SETUP_RETRY_MS = 5UL * 60 * 1000; // nobody configuring for 5 min: reboot and retry saved WiFi
const unsigned long FORGET_HOLD_MS = 3000;            // hold BOOT this long to forget the saved WiFi

WebServer server(80);
DNSServer dns;
Preferences prefs;

bool setupMode = false;
unsigned long lastSetupClientMs = 0;
String savedSsid, savedPass;

// A connection attempt runs from loop() so the web server keeps answering /result meanwhile.
enum class Attempt { Idle, Pending, Trying, Ok, Fail };
Attempt attempt = Attempt::Idle;
String attemptSsid, attemptPass, attemptError;
unsigned long attemptMs = 0; // when the attempt was requested / started / succeeded
volatile uint8_t lastDisconnectReason = 0;
bool apGrace = false;        // setup succeeded; setup AP turns off after AP_GRACE_MS
bool greenLedPending = false; // switched networks in normal mode; LED goes off 3 s after success
unsigned long restartRequestedMs = 0;

const char *pageHtml = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Cài đặt WiFi ESP32</title>
  <style>
    body { font-family: sans-serif; margin: 0; background: #f5f5f5; }
    .wrap { max-width: 480px; margin: 0 auto; padding: 16px; }
    h1 { font-size: 22px; }
    h2 { font-size: 17px; margin: 20px 0 8px; display: flex; justify-content: space-between; align-items: center; }
    .card { background: #fff; border-radius: 10px; padding: 12px 14px; box-shadow: 0 1px 2px #0002; overflow-wrap: anywhere; }
    .net { display: flex; justify-content: space-between; gap: 8px; padding: 12px 14px; background: #fff; border-radius: 8px;
           margin: 6px 0; cursor: pointer; box-shadow: 0 1px 1px #0001; overflow-wrap: anywhere; }
    .net.sel { outline: 2px solid #1e88e5; }
    .meta { color: #777; font-size: 13px; }
    input { width: 100%; box-sizing: border-box; font-size: 16px; padding: 10px; margin: 6px 0; border: 1px solid #ccc; border-radius: 8px; }
    button { font-size: 16px; padding: 12px; border: none; border-radius: 8px; background: #1e88e5; color: #fff; }
    .full { width: 100%; margin-top: 6px; }
    .small { padding: 6px 10px; font-size: 14px; background: #757575; }
    #result { margin-top: 12px; }
    .ok { background: #c8e6c9; color: #1b5e20; }
    .fail { background: #ffcdd2; color: #b71c1c; }
    .trying { background: #fff3c4; }
  </style>
</head>
<body>
<div class="wrap">
  <h1>Cài đặt WiFi cho ESP32</h1>
  <div id="status" class="card">Đang tải...</div>
  <h2>Chọn mạng WiFi <button id="rescan" class="small">Quét lại</button></h2>
  <div id="list" class="meta">Đang quét mạng xung quanh...</div>
  <form id="f">
    <input id="ssid" placeholder="Tên WiFi" autocomplete="off">
    <input id="pass" type="password" placeholder="Mật khẩu" autocomplete="off">
    <label class="meta"><input type="checkbox" id="show" style="width:auto"> Hiện mật khẩu</label>
    <button class="full">Kết nối</button>
  </form>
  <div id="result" class="card" hidden></div>
  <p><button id="forget" class="small" hidden>Quên WiFi đã lưu</button></p>
  <p class="meta">Mẹo: giữ nút BOOT trên board 3 giây để quên WiFi và vào lại chế độ cài đặt.</p>
</div>
<script>
  const $ = id => document.getElementById(id);
  let info = {};
  function strength(rssi) { return rssi >= -60 ? 'Sóng mạnh' : rssi >= -75 ? 'Sóng trung bình' : 'Sóng yếu'; }
  function loadInfo() {
    return fetch('/info').then(r => r.json()).then(d => {
      info = d;
      $('status').textContent = d.connected
        ? 'Đang kết nối: ' + d.ssid + ' · IP ' + d.ip + ' · ' + strength(d.rssi) + ' (' + d.rssi + ' dBm)'
        : d.saved ? 'Chưa kết nối được WiFi đã lưu (' + d.saved + '). Chọn lại mạng bên dưới.'
        : 'Board chưa có WiFi. Chọn mạng bên dưới để cài đặt.';
      $('forget').hidden = !d.saved;
    }).catch(() => {});
  }
  // SSIDs come from anyone nearby: build nodes with textContent, never innerHTML.
  function scan() {
    $('list').textContent = 'Đang quét mạng xung quanh...';
    fetch('/scan').then(r => r.ok ? r.json() : Promise.reject()).then(nets => {
      $('list').textContent = nets.length ? '' : 'Không thấy mạng nào. Bấm Quét lại.';
      nets.forEach(n => {
        const d = document.createElement('div'), a = document.createElement('span'), b = document.createElement('span');
        d.className = 'net';
        a.textContent = n.ssid;
        b.className = 'meta';
        b.textContent = strength(n.rssi) + (n.secure ? ' · có mật khẩu' : ' · mở');
        d.append(a, b);
        d.onclick = () => {
          document.querySelectorAll('.net').forEach(x => x.classList.remove('sel'));
          d.classList.add('sel');
          $('ssid').value = n.ssid;
          $('pass').value = '';
          $('pass').hidden = !n.secure;
          if (n.secure) $('pass').focus();
        };
        $('list').append(d);
      });
    }).catch(() => { $('list').textContent = 'Quét thất bại. Bấm Quét lại.'; });
  }
  function showResult(cls, text) { const r = $('result'); r.hidden = false; r.className = 'card ' + cls; r.textContent = text; }
  function pollResult() {
    fetch('/result').then(r => r.json()).then(d => {
      if (d.state === 'trying' || d.state === 'pending') {
        showResult('trying', 'Đang thử kết nối "' + d.ssid + '"...');
        setTimeout(pollResult, 1000);
      } else if (d.state === 'ok') {
        showResult('ok', info.mode === 'setup'
          ? 'Kết nối thành công "' + d.ssid + '"! Địa chỉ của board: http://' + d.ip + '/ (hoặc http://' + d.hostname +
            '.local/). WiFi ESP32-Setup sẽ tắt sau khoảng 1 phút, hãy chuyển điện thoại về mạng ' + d.ssid + '.'
          : 'Đã chuyển sang "' + d.ssid + '". Địa chỉ của board: http://' + d.ip + '/');
        loadInfo();
      } else if (d.state === 'fail') {
        showResult('fail', 'Không kết nối được "' + d.ssid + '": ' + d.error);
        loadInfo();
      }
    }).catch(() => {
      // Expected while the board switches networks or the setup WiFi changes channel.
      showResult('trying', 'Đang thử kết nối... (tạm mất liên lạc với board, đang chờ)');
      setTimeout(pollResult, 1500);
    });
  }
  $('f').onsubmit = e => {
    e.preventDefault();
    const body = new URLSearchParams({ ssid: $('ssid').value, pass: $('pass').hidden ? '' : $('pass').value });
    fetch('/connect', { method: 'POST', body }).then(r => r.text().then(t => {
      if (r.ok) { showResult('trying', t); setTimeout(pollResult, 1000); } else showResult('fail', t);
    }));
  };
  $('show').onchange = () => { $('pass').type = $('show').checked ? 'text' : 'password'; };
  $('rescan').onclick = scan;
  $('forget').onclick = () => {
    if (confirm('Quên WiFi đã lưu và khởi động lại vào chế độ cài đặt?'))
      fetch('/forget', { method: 'POST' }).then(r => r.text()).then(t => showResult('trying', t));
  };
  loadInfo();
  scan();
</script>
</body>
</html>
)rawliteral";

void loadCreds() {
    prefs.begin("wifi", true);
    savedSsid = prefs.getString("ssid", "");
    savedPass = prefs.getString("pass", "");
    prefs.end();
}

void saveCreds(const String &ssid, const String &pass) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    savedSsid = ssid;
    savedPass = pass;
}

void forgetCreds() {
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    savedSsid = "";
    savedPass = "";
}

bool isWrongPasswordReason(uint8_t r) {
    return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_AUTH_EXPIRE ||
           r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

String describeFailure(uint8_t reason) {
    if (reason == WIFI_REASON_NO_AP_FOUND) return "Không tìm thấy mạng này (sai tên hoặc ở quá xa)";
    if (isWrongPasswordReason(reason)) return "Sai mật khẩu";
    if (reason == 0) return "Hết thời gian chờ, không kết nối được";
    return "Không kết nối được (mã lỗi " + String(reason) + ")";
}

void sendText(int code, const char *text) {
    server.send(code, "text/plain; charset=utf-8", text);
}

bool connectSaved() {
    Serial.printf("Connecting to saved WiFi \"%s\"...\n", savedSsid.c_str());
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT_MS) delay(100);
    return WiFi.status() == WL_CONNECTED;
}

void startNormalMode() {
    setupMode = false;
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    setLed(0, 0, 0);
    Serial.printf("Connected to \"%s\": http://%s/ or http://%s.local/\n", WiFi.SSID().c_str(),
        WiFi.localIP().toString().c_str(), HOSTNAME);
}

void startSetupMode() {
    setupMode = true;
    WiFi.disconnect(); // stop retrying the saved network: its scans would disturb the AP
    delay(100);

    // Start the AP on the channel of the network the user will most likely pick (the saved one,
    // else the strongest): joining a network on another channel forces the AP to hop channels,
    // which briefly drops the phone from the setup WiFi.
    int n = WiFi.scanNetworks();
    int channel = 1, best = -1000;
    for (int i = 0; i < n; i++) {
        int score = (savedSsid.length() && WiFi.SSID(i) == savedSsid) ? 1000 : WiFi.RSSI(i);
        if (score > best) {
            best = score;
            channel = WiFi.channel(i);
        }
    }
    WiFi.scanDelete();

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(SETUP_SSID, nullptr, channel);
    dns.start(53, "*", AP_IP);
    lastSetupClientMs = millis();
    setLed(0, 0, 64); // blue = waiting for WiFi setup
    Serial.printf("Setup mode: join WiFi \"%s\" (channel %d), then open http://%s/\n", SETUP_SSID, channel,
        AP_IP.toString().c_str());
}

void beginAttempt() {
    lastDisconnectReason = 0;
    WiFi.disconnect();
    WiFi.begin(attemptSsid.c_str(), attemptPass.c_str());
    attemptMs = millis();
    attempt = Attempt::Trying;
    setLed(64, 48, 0); // yellow = trying
    Serial.printf("Trying WiFi \"%s\"...\n", attemptSsid.c_str());
}

void updateAttempt() {
    if (attempt == Attempt::Pending && millis() - attemptMs > 200) {
        beginAttempt(); // delayed so the /connect reply reaches a LAN client before we disconnect
        return;
    }
    if (attempt != Attempt::Trying) return;

    unsigned long elapsed = millis() - attemptMs;
    if (WiFi.status() == WL_CONNECTED) {
        saveCreds(attemptSsid, attemptPass);
        attempt = Attempt::Ok;
        attemptMs = millis();
        setLed(0, 64, 0); // green = connected
        Serial.printf("Connected to \"%s\" as %s, saved\n", attemptSsid.c_str(), WiFi.localIP().toString().c_str());
        if (setupMode) apGrace = true; else greenLedPending = true;
        return;
    }

    uint8_t reason = lastDisconnectReason;
    bool definite = reason == WIFI_REASON_NO_AP_FOUND || isWrongPasswordReason(reason);
    if (elapsed > CONNECT_TIMEOUT_MS || (definite && elapsed > 6000)) {
        attemptError = describeFailure(reason);
        attempt = Attempt::Fail;
        Serial.printf("WiFi \"%s\" failed, reason %u\n", attemptSsid.c_str(), reason);
        WiFi.disconnect();
        if (setupMode) {
            setLed(0, 0, 64);
        } else if (savedSsid.length()) {
            // Don't strand a working device on a typo: go back to the network that worked.
            WiFi.begin(savedSsid.c_str(), savedPass.c_str());
            setLed(0, 0, 0);
        }
    }
}

void handleInfo() {
    bool connected = WiFi.status() == WL_CONNECTED;
    String json = String("{\"mode\":\"") + (setupMode ? "setup" : "normal") + "\"" +
        ",\"connected\":" + (connected ? "true" : "false") +
        ",\"ssid\":\"" + jsonEscape(connected ? WiFi.SSID() : String("")) + "\"" +
        ",\"ip\":\"" + (connected ? WiFi.localIP().toString() : String("")) + "\"" +
        ",\"rssi\":" + String(connected ? (int)WiFi.RSSI() : 0) +
        ",\"saved\":\"" + jsonEscape(savedSsid) + "\"}";
    server.send(200, "application/json; charset=utf-8", json);
}

void handleScan() {
    if (attempt == Attempt::Trying || attempt == Attempt::Pending) return sendText(409, "Đang thử kết nối, đợi chút");
    int n = WiFi.scanNetworks();
    std::vector<int> order;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i).length()) order.push_back(i); // skip hidden networks
    }
    std::sort(order.begin(), order.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });

    std::vector<String> seen; // the same SSID from several access points: list it once, strongest first
    String json = "[";
    for (int i : order) {
        String ssid = WiFi.SSID(i);
        if (std::find(seen.begin(), seen.end(), ssid) != seen.end()) continue;
        seen.push_back(ssid);
        if (json.length() > 1) json += ",";
        json += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String((int)WiFi.RSSI(i)) +
                ",\"secure\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    server.send(200, "application/json; charset=utf-8", json);
}

void handleConnect() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    if (ssid.isEmpty() || ssid.length() > 32) return sendText(400, "Tên WiFi không hợp lệ");
    if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63))
        return sendText(400, "Mật khẩu WiFi phải từ 8 đến 63 ký tự");
    if (attempt == Attempt::Trying || attempt == Attempt::Pending) return sendText(409, "Đang thử kết nối, đợi chút");
    attemptSsid = ssid;
    attemptPass = pass;
    attemptError = "";
    attempt = Attempt::Pending;
    attemptMs = millis();
    sendText(200, "Đang thử kết nối...");
}

void handleResult() {
    const char *state = attempt == Attempt::Pending ? "pending"
                      : attempt == Attempt::Trying  ? "trying"
                      : attempt == Attempt::Ok      ? "ok"
                      : attempt == Attempt::Fail    ? "fail"
                                                    : "idle";
    String json = String("{\"state\":\"") + state + "\"" +
        ",\"ssid\":\"" + jsonEscape(attemptSsid) + "\"" +
        ",\"ip\":\"" + (attempt == Attempt::Ok ? WiFi.localIP().toString() : String("")) + "\"" +
        ",\"error\":\"" + jsonEscape(attemptError) + "\"" +
        ",\"hostname\":\"" + HOSTNAME + "\"}";
    server.send(200, "application/json; charset=utf-8", json);
}

void setup() {
    Serial.begin(115200);
    pinMode(BOOT_BUTTON, INPUT_PULLUP);
    initLed();
    setLed(0, 0, 0);

    WiFi.persistent(false); // credentials live in our own Preferences, not the WiFi driver's storage
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
        uint8_t r = info.wifi_sta_disconnected.reason;
        if (r != WIFI_REASON_ASSOC_LEAVE) lastDisconnectReason = r; // ASSOC_LEAVE = our own disconnect()
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.setHostname(HOSTNAME);
    WiFi.mode(WIFI_STA);

    loadCreds();
    if (savedSsid.length() && connectSaved()) {
        startNormalMode();
    } else {
        startSetupMode();
    }

    server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", pageHtml); });
    server.on("/info", HTTP_GET, handleInfo);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/connect", HTTP_POST, handleConnect);
    server.on("/result", HTTP_GET, handleResult);
    server.on("/forget", HTTP_POST, []() {
        forgetCreds();
        sendText(200, "Đã quên WiFi. Board đang khởi động lại vào chế độ cài đặt: kết nối WiFi ESP32-Setup để cài lại.");
        restartRequestedMs = millis();
    });
    // In setup mode every unknown URL (the phone's connectivity checks) goes to the setup page,
    // which makes the phone pop it up right after joining ESP32-Setup.
    server.onNotFound([]() {
        if (!setupMode) return sendText(404, "Not found");
        server.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
        server.send(302, "text/plain", "");
    });
    server.begin();
}

void loop() {
    if (setupMode) dns.processNextRequest();
    server.handleClient();
    updateAttempt();

    if (restartRequestedMs && millis() - restartRequestedMs > 1000) ESP.restart();

    // After a successful setup, leave the setup WiFi up a bit so the phone can read the result.
    if (apGrace && millis() - attemptMs > AP_GRACE_MS) {
        apGrace = false;
        dns.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        startNormalMode();
    }
    if (greenLedPending && millis() - attemptMs > 3000) {
        greenLedPending = false;
        setLed(0, 0, 0);
    }

    // Saved WiFi was unreachable at boot (e.g. router off) and nobody is configuring: retry it.
    if (setupMode && !apGrace) {
        if (WiFi.softAPgetStationNum() > 0) lastSetupClientMs = millis();
        if (savedSsid.length() && attempt != Attempt::Trying && attempt != Attempt::Pending &&
            millis() - lastSetupClientMs > SETUP_RETRY_MS) {
            Serial.println("No one configuring for 5 min, restarting to retry saved WiFi");
            ESP.restart();
        }
    }

    // Hold BOOT 3 s: forget WiFi and restart into setup mode.
    static unsigned long pressedSinceMs = 0;
    if (digitalRead(BOOT_BUTTON) == LOW) {
        if (!pressedSinceMs) pressedSinceMs = millis();
        if (millis() - pressedSinceMs > FORGET_HOLD_MS) {
            Serial.println("BOOT held 3 s: forgetting WiFi, restarting into setup mode");
            forgetCreds();
            setLed(64, 0, 0);
            delay(500);
            ESP.restart();
        }
    } else {
        pressedSinceMs = 0;
    }

    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 5000) {
        lastPrint = millis();
        if (setupMode) {
            Serial.printf("Setup mode: %d device(s) on \"%s\", saved WiFi: \"%s\"\n", WiFi.softAPgetStationNum(),
                SETUP_SSID, savedSsid.c_str());
        } else {
            Serial.printf("Normal mode: \"%s\" http://%s/ (%d dBm)\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
        }
    }
}
