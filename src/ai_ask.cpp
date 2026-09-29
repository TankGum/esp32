#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <FFat.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "json_escape.h"
#include "wifi_config.h"

#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  13

const int W = 240, H = 320;
const char *HOSTNAME = "esp32";
const char *API_HOST = "api.anthropic.com";
const char *API_URL = "https://api.anthropic.com/v1/messages";
const char *MODEL = "claude-opus-5";
// The board has no screen space for accents (the built-in font is ASCII), so the answer
// is requested without them; it is short so it fits on a 240x320 screen.
const char *SYSTEM_PROMPT =
    "Tra loi ngan gon bang tieng Viet KHONG DAU, toi da 60 tu, khong dung markdown.";
const char *IMAGE_PATH = "/ask.jpg";
const size_t MAX_IMAGE_BYTES = 700 * 1024; // keeps the base64 body well under the API's 10 MB limit

Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);
Preferences prefs;

File uploadFile;
size_t uploadBytes = 0;
bool uploadOk = false;
String question = "Anh nay noi ve cai gi?";

enum class Ask { Idle, Pending, Working, Done, Failed };
Ask askState = Ask::Idle;
String answer, askError;

void screenText(const String &title, const String &body, uint16_t color = ST77XX_WHITE) {
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, W, 24, ST77XX_BLUE);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 4);
    tft.print(title);

    // Word wrap at size 2 (12 px per character, 26 characters per line).
    tft.setTextColor(color);
    int x = 6, y = 34;
    String word;
    for (unsigned i = 0; i <= body.length(); i++) {
        char c = i < body.length() ? body[i] : ' ';
        if (c != ' ' && c != '\n') {
            word += c;
            continue;
        }
        if (x + (int)word.length() * 12 > W - 6) {
            x = 6;
            y += 20;
        }
        if (y > H - 20) break;
        tft.setCursor(x, y);
        tft.print(word);
        x += (word.length() + 1) * 12;
        if (c == '\n') {
            x = 6;
            y += 20;
        }
        word = "";
    }
}

const char *pageHtml = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Hỏi AI về ảnh</title>
  <style>
    body { font-family: sans-serif; margin: 0; background: #f5f5f5; }
    .wrap { max-width: 480px; margin: 0 auto; padding: 16px; }
    .card { background: #fff; border-radius: 10px; padding: 12px 14px; box-shadow: 0 1px 2px #0002; margin-bottom: 14px; }
    input, textarea { width: 100%; box-sizing: border-box; font-size: 16px; padding: 9px; border: 1px solid #ccc; border-radius: 8px; margin: 6px 0; }
    button { font-size: 16px; padding: 12px; border: none; border-radius: 8px; background: #1e88e5; color: #fff; width: 100%; }
    #answer { white-space: pre-wrap; }
    .meta { color: #777; font-size: 13px; }
  </style>
</head>
<body>
<div class="wrap">
  <h1>Hỏi AI về ảnh</h1>
  <div class="card">
    <h3>Khóa API</h3>
    <div id="keyState" class="meta">Đang kiểm tra...</div>
    <input id="key" type="password" placeholder="Dán khóa API vào đây">
    <button id="saveKey">Lưu khóa vào board</button>
    <p class="meta">Khóa được lưu trong bộ nhớ trong của board và không bao giờ được gửi ngược ra trang này.</p>
  </div>
  <div class="card">
    <h3>Ảnh và câu hỏi</h3>
    <input type="file" id="file" accept="image/jpeg,image/*">
    <textarea id="q" rows="2">Anh nay noi ve cai gi?</textarea>
    <button id="send">Gửi ảnh và hỏi AI</button>
    <div id="status" class="meta"></div>
  </div>
  <div class="card">
    <h3>Câu trả lời</h3>
    <div id="answer">(chưa có)</div>
  </div>
</div>
<script>
  const $ = id => document.getElementById(id);
  function keyState() {
    fetch('/ai/status').then(r => r.json()).then(d => {
      $('keyState').textContent = d.hasKey ? 'Board đã có khóa API.' : 'Board chưa có khóa API.';
    });
  }
  $('saveKey').onclick = () => {
    fetch('/ai/key', { method: 'POST', body: new URLSearchParams({ key: $('key').value }) })
      .then(r => r.text()).then(t => { $('keyState').textContent = t; $('key').value = ''; keyState(); });
  };
  // The photo is resized to 1024 px on the phone: smaller upload, fewer image tokens, same reading quality.
  async function shrink(file) {
    const bmp = await createImageBitmap(file, { imageOrientation: 'from-image' });
    const s = Math.min(1, 1024 / Math.max(bmp.width, bmp.height));
    const cv = document.createElement('canvas');
    cv.width = Math.round(bmp.width * s);
    cv.height = Math.round(bmp.height * s);
    cv.getContext('2d').drawImage(bmp, 0, 0, cv.width, cv.height);
    return new Promise(res => cv.toBlob(res, 'image/jpeg', 0.85));
  }
  $('send').onclick = async () => {
    if (!$('file').files[0]) { $('status').textContent = 'Chưa chọn ảnh'; return; }
    $('status').textContent = 'Đang thu nhỏ ảnh...';
    const blob = await shrink($('file').files[0]);
    const fd = new FormData();
    fd.append('img', blob, 'ask.jpg');
    fd.append('q', $('q').value);
    $('status').textContent = 'Đang gửi ' + Math.round(blob.size / 1024) + ' KB...';
    const t = await fetch('/ask', { method: 'POST', body: fd }).then(r => r.text());
    $('status').textContent = t;
    poll();
  };
  function poll() {
    fetch('/ask/result').then(r => r.json()).then(d => {
      if (d.state === 'working' || d.state === 'pending') {
        $('status').textContent = 'Board đang hỏi AI...';
        setTimeout(poll, 1000);
      } else if (d.state === 'done') {
        $('status').textContent = 'Xong';
        $('answer').textContent = d.answer;
      } else if (d.state === 'failed') {
        $('status').textContent = 'Lỗi: ' + d.error;
      }
    }).catch(() => setTimeout(poll, 1500));
  }
  keyState();
</script>
</body>
</html>
)rawliteral";

// Feeds HTTPClient the request body without ever holding it in RAM: a JSON prefix, then the
// image file base64-encoded on the fly, then the question.
class BodyStream : public Stream {
public:
    BodyStream(const String &prefix, File file, const String &suffix)
        : _prefix(prefix), _file(file), _suffix(suffix) {}

    static size_t base64Length(size_t rawBytes) { return 4 * ((rawBytes + 2) / 3); }
    size_t total() const { return _prefix.length() + base64Length(_file.size()) + _suffix.length(); }

    int available() override { return total() - _sent; }
    int read() override {
        uint8_t c;
        return readBytes(&c, 1) == 1 ? c : -1;
    }
    int peek() override { return -1; }
    size_t write(uint8_t) override { return 0; }

    size_t readBytes(uint8_t *out, size_t len) {
        size_t done = 0;
        while (done < len) {
            if (_sent < _prefix.length()) {
                size_t n = min(len - done, _prefix.length() - _sent);
                memcpy(out + done, _prefix.c_str() + _sent, n);
                done += n;
                _sent += n;
                continue;
            }
            if (_b64Pos < _b64Len) { // leftover of the previous encoded chunk
                size_t n = min(len - done, _b64Len - _b64Pos);
                memcpy(out + done, _b64 + _b64Pos, n);
                done += n;
                _b64Pos += n;
                _sent += n;
                continue;
            }
            if (_file.available()) {
                uint8_t raw[300]; // multiple of 3: no padding appears mid-stream
                size_t n = _file.read(raw, sizeof(raw));
                _b64Len = 0;
                _b64Pos = 0;
                mbedtls_base64_encode(_b64, sizeof(_b64), &_b64Len, raw, n);
                continue;
            }
            size_t suffixPos = _sent - _prefix.length() - base64Length(_file.size());
            if (suffixPos < _suffix.length()) {
                size_t n = min(len - done, _suffix.length() - suffixPos);
                memcpy(out + done, _suffix.c_str() + suffixPos, n);
                done += n;
                _sent += n;
                continue;
            }
            break;
        }
        return done;
    }
    size_t readBytes(char *out, size_t len) { return readBytes((uint8_t *)out, len); }

private:
    String _prefix, _suffix;
    File _file;
    size_t _sent = 0;
    uint8_t _b64[404];
    size_t _b64Len = 0, _b64Pos = 0;
};

void askClaude() {
    prefs.begin("ai", true);
    String apiKey = prefs.getString("key", "");
    prefs.end();
    if (apiKey.isEmpty()) {
        askError = "Board chưa có khóa API";
        askState = Ask::Failed;
        return;
    }
    File image = FFat.open(IMAGE_PATH, FILE_READ);
    if (!image || image.size() == 0) {
        askError = "Không đọc được ảnh";
        askState = Ask::Failed;
        return;
    }

    String prefix = String("{\"model\":\"") + MODEL + "\",\"max_tokens\":1024,\"fallbacks\":\"default\"" +
        ",\"system\":\"" + jsonEscape(SYSTEM_PROMPT) + "\"" +
        ",\"messages\":[{\"role\":\"user\",\"content\":[" +
        "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/jpeg\",\"data\":\"";
    String suffix = String("\"}},{\"type\":\"text\",\"text\":\"") + jsonEscape(question) + "\"}]}]}";
    BodyStream body(prefix, image, suffix);
    size_t length = body.total();

    WiFiClientSecure secure;
    secure.setInsecure(); // no root CA bundle on the board; the API key still travels over TLS
    HTTPClient http;
    http.setTimeout(60000);
    if (!http.begin(secure, API_URL)) {
        askError = "Không kết nối được tới API";
        askState = Ask::Failed;
        image.close();
        return;
    }
    http.addHeader("content-type", "application/json");
    http.addHeader("x-api-key", apiKey);
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("anthropic-beta", "server-side-fallback-2026-07-01");

    Serial.printf("Asking %s about a %u byte image (%u byte request)\n", MODEL, image.size(), length);
    unsigned long t0 = millis();
    int status = http.sendRequest("POST", &body, length);
    String payload = http.getString();
    http.end();
    image.close();
    Serial.printf("HTTP %d in %lu ms, %u bytes\n", status, millis() - t0, payload.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (status <= 0) {
        askError = "Loi ket noi (" + String(status) + ")";
    } else if (err) {
        askError = "Tra loi khong doc duoc";
    } else if (status != 200) {
        const char *msg = doc["error"]["message"];
        askError = "API tu choi: " + String(msg ? msg : payload.substring(0, 80).c_str());
    } else if (String((const char *)(doc["stop_reason"] | "")) == "refusal") {
        askError = "AI tu choi tra loi anh nay";
    } else {
        answer = "";
        for (JsonObject block : doc["content"].as<JsonArray>()) {
            if (block["type"] == "text") answer += (const char *)block["text"];
        }
        answer.trim();
        askState = answer.isEmpty() ? Ask::Failed : Ask::Done;
        if (answer.isEmpty()) askError = "AI tra ve rong";
        Serial.printf("Answer: %s\n", answer.c_str());
        if (askState == Ask::Done) screenText("Tra loi", answer, ST77XX_GREEN);
        return;
    }
    askState = Ask::Failed;
    Serial.printf("Ask failed: %s\n", askError.c_str());
    screenText("Loi", askError, ST77XX_RED);
}

void handleAskUpload() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        uploadBytes = 0;
        uploadFile = FFat.open(IMAGE_PATH, FILE_WRITE);
        uploadOk = (bool)uploadFile;
    } else if (up.status == UPLOAD_FILE_WRITE && uploadFile) {
        uploadBytes += up.currentSize;
        if (uploadBytes > MAX_IMAGE_BYTES || uploadFile.write(up.buf, up.currentSize) != up.currentSize) uploadOk = false;
    } else if (up.status == UPLOAD_FILE_END && uploadFile) {
        uploadFile.close();
    }
}

void setup() {
    Serial.begin(115200);
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.init(W, H);
    tft.setRotation(0);
    screenText("Hoi AI", "Dang khoi dong...");

    if (!FFat.begin(true)) Serial.println("FFat mount failed");
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) delay(250);
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    configTime(7 * 3600, 0, "pool.ntp.org"); // TLS certificates need a roughly correct clock
    Serial.printf("Ready: http://%s/\n", WiFi.localIP().toString().c_str());
    screenText("Hoi AI", "Mo tren dien thoai:\nhttp://" + WiFi.localIP().toString() + "/", ST77XX_YELLOW);

    server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", pageHtml); });
    server.on("/ai/status", HTTP_GET, []() {
        prefs.begin("ai", true);
        bool hasKey = prefs.getString("key", "").length() > 0;
        prefs.end();
        server.send(200, "application/json", String("{\"hasKey\":") + (hasKey ? "true" : "false") + "}");
    });
    server.on("/ai/key", HTTP_POST, []() {
        String key = server.arg("key");
        key.trim();
        if (key.length() < 20) return server.send(400, "text/plain; charset=utf-8", "Khóa API không hợp lệ");
        prefs.begin("ai", false);
        prefs.putString("key", key);
        prefs.end();
        server.send(200, "text/plain; charset=utf-8", "Đã lưu khóa API vào board");
    });
    server.on("/ask", HTTP_POST, []() {
        if (!uploadOk || uploadBytes == 0) return server.send(400, "text/plain; charset=utf-8", "Ảnh lỗi hoặc quá lớn");
        if (askState == Ask::Working || askState == Ask::Pending) {
            return server.send(409, "text/plain; charset=utf-8", "Đang hỏi câu trước, đợi chút");
        }
        String q = server.arg("q");
        q.trim();
        if (q.length() > 0) question = q;
        answer = "";
        askError = "";
        askState = Ask::Pending; // the call itself runs from loop(), so this reply goes out first
        server.send(200, "text/plain; charset=utf-8", "Đã nhận ảnh " + String(uploadBytes / 1024) + " KB, đang hỏi AI...");
    }, handleAskUpload);
    server.on("/ask/result", HTTP_GET, []() {
        const char *state = askState == Ask::Pending ? "pending"
                          : askState == Ask::Working ? "working"
                          : askState == Ask::Done    ? "done"
                          : askState == Ask::Failed  ? "failed"
                                                     : "idle";
        String json = String("{\"state\":\"") + state + "\",\"answer\":\"" + jsonEscape(answer) +
            "\",\"error\":\"" + jsonEscape(askError) + "\"}";
        server.send(200, "application/json; charset=utf-8", json);
    });
    server.begin();
}

void loop() {
    server.handleClient();
    if (askState == Ask::Pending) {
        askState = Ask::Working;
        screenText("Hoi AI", "Dang hoi AI...", ST77XX_YELLOW);
        askClaude();
    }
}
