#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <FFat.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "screen_page.h"
#include "wifi_config.h"

#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  13
#define BOOT_BUTTON 0 // active low: short press = next page, hold 1 s = previous page

const int W = 240, H = 320;
const uint32_t PAGE_BYTES = (uint32_t)W * H * 2; // one screen as RGB565 (photos)
// Text pages are stored 1 bit per pixel with the two colors in a small header, which is
// 16x smaller than RGB565 and is exact for text: every pixel is either ink or background.
const uint32_t MONO_BYTES = 4 + (uint32_t)W * H / 8;
const int MAX_PAGES = 800;
const char *HOSTNAME = "esp32";

Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);
Preferences prefs;

// Images arrive as a stream and are drawn (or stored) row by row, so the board never
// holds more than one row (480 bytes) of a 150 KB page.
uint16_t lineBuf[W];
uint32_t pixelsDrawn = 0;
uint32_t bytesReceived = 0;
uint8_t carryByte = 0;
bool hasCarry = false;
unsigned long uploadStartMs = 0;

File pageFile;
bool pageUploadOk = false;
bool pageUploadMono = false;
int pageCount = 0;
int currentPage = 0;

String colorPagePath(int i) { return "/p" + String(i) + ".bin"; }
String monoPagePath(int i) { return "/m" + String(i) + ".bin"; }
bool pageExists(int i) { return FFat.exists(monoPagePath(i)) || FFat.exists(colorPagePath(i)); }

void showBanner(const char *note) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 20);
    tft.println("ESP32-S3 screen");
    tft.setTextSize(1);
    tft.setCursor(10, 60);
    tft.println(note);
}

bool showStoredPage(int i) {
    if (i < 0 || i >= pageCount) return false;
    bool mono = FFat.exists(monoPagePath(i));
    File f = FFat.open(mono ? monoPagePath(i) : colorPagePath(i), FILE_READ);
    if (!f) return false;
    if (f.size() != (mono ? MONO_BYTES : PAGE_BYTES)) {
        f.close();
        return false;
    }
    if (mono) {
        uint16_t fg, bg;
        f.read((uint8_t *)&fg, 2);
        f.read((uint8_t *)&bg, 2);
        uint8_t row[W / 8];
        for (int y = 0; y < H; y++) {
            f.read(row, sizeof(row));
            for (int x = 0; x < W; x++) lineBuf[x] = (row[x >> 3] & (0x80 >> (x & 7))) ? fg : bg;
            tft.drawRGBBitmap(0, y, lineBuf, W, 1);
        }
    } else {
        for (int y = 0; y < H; y++) {
            f.read((uint8_t *)lineBuf, W * 2);
            tft.drawRGBBitmap(0, y, lineBuf, W, 1);
        }
    }
    f.close();
    currentPage = i;
    prefs.begin("doc", false);
    prefs.putInt("cur", i);
    prefs.end();
    Serial.printf("Showing stored page %d/%d\n", i + 1, pageCount);
    return true;
}

void erasePages() {
    for (int i = 0; i < MAX_PAGES; i++) {
        if (FFat.exists(colorPagePath(i))) FFat.remove(colorPagePath(i));
        if (FFat.exists(monoPagePath(i))) FFat.remove(monoPagePath(i));
    }
}

const char *pageHtml = SCREEN_PAGE_HTML;

void resetUploadState() {
    pixelsDrawn = 0;
    bytesReceived = 0;
    hasCarry = false;
    uploadStartMs = millis();
}

// Draws the incoming RGB565 stream straight to the screen, one row at a time.
void handleImageUpload() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) return resetUploadState();
    if (up.status != UPLOAD_FILE_WRITE) return;

    const uint8_t *p = up.buf;
    size_t n = up.currentSize;
    bytesReceived += n;
    while (n > 0 && pixelsDrawn < (uint32_t)W * H) {
        uint8_t lo, hi;
        if (hasCarry) {
            lo = carryByte;
            hi = *p++;
            n--;
            hasCarry = false;
        } else if (n == 1) { // a pixel split across two chunks
            carryByte = *p;
            hasCarry = true;
            break;
        } else {
            lo = p[0];
            hi = p[1];
            p += 2;
            n -= 2;
        }
        lineBuf[pixelsDrawn % W] = (uint16_t)lo | ((uint16_t)hi << 8);
        pixelsDrawn++;
        if (pixelsDrawn % W == 0) tft.drawRGBBitmap(0, pixelsDrawn / W - 1, lineBuf, W, 1);
    }
}

// Stores the incoming page in flash instead. The page number and format come from the upload
// filename ("m3.bin" = mono text page, "p3.bin" = RGB565), which the client chooses, so it is
// parsed strictly rather than used as a path.
void handlePageUpload() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        resetUploadState();
        pageUploadOk = false;
        pageUploadMono = false;
        String name = up.filename;
        bool mono = name.startsWith("m");
        if ((!mono && !name.startsWith("p")) || !name.endsWith(".bin")) return;
        String digits = name.substring(1, name.length() - 4);
        if (digits.isEmpty()) return;
        for (char c : digits) {
            if (!isdigit((unsigned char)c)) return;
        }
        int index = digits.toInt();
        if (index < 0 || index >= MAX_PAGES) return;
        pageFile = FFat.open(mono ? monoPagePath(index) : colorPagePath(index), FILE_WRITE);
        pageUploadMono = mono;
        pageUploadOk = (bool)pageFile;
        return;
    }
    if (up.status == UPLOAD_FILE_WRITE && pageFile) {
        bytesReceived += up.currentSize;
        if (pageFile.write(up.buf, up.currentSize) != up.currentSize) pageUploadOk = false;
    } else if (up.status == UPLOAD_FILE_END && pageFile) {
        pageFile.close();
    }
}

void stepPage(int delta) {
    if (pageCount <= 0) return;
    int next = (currentPage + delta % pageCount + pageCount) % pageCount;
    showStoredPage(next);
}

void setup() {
    Serial.begin(115200);
    pinMode(BOOT_BUTTON, INPUT_PULLUP);

    // The board definition's default SPI pins are different ones, so start SPI on ours.
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.init(W, H);
    tft.setRotation(0);
    showBanner("Starting...");

    if (!FFat.begin(true)) Serial.println("FFat mount failed");
    prefs.begin("doc", true);
    pageCount = prefs.getInt("count", 0);
    currentPage = prefs.getInt("cur", 0);
    prefs.end();

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    showBanner("Connecting to WiFi...");
    while (WiFi.status() != WL_CONNECTED) delay(250);
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Ready: http://%s/ or http://%s.local/ | stored pages: %d\n",
        WiFi.localIP().toString().c_str(), HOSTNAME, pageCount);

    // Resume the document that was being read, otherwise show how to reach the board.
    if (pageCount <= 0 || !showStoredPage(currentPage)) {
        String hint = "Open http://" + WiFi.localIP().toString() + "/";
        showBanner(hint.c_str());
    }

    server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", pageHtml); });

    server.on("/image", HTTP_POST, []() {
        unsigned long ms = millis() - uploadStartMs;
        Serial.printf("Image: %u bytes in %lu ms\n", bytesReceived, ms);
        if (bytesReceived != PAGE_BYTES) return server.send(400, "text/plain; charset=utf-8", "Ảnh sai kích thước (cần 240x320 RGB565)");
        server.send(200, "text/plain; charset=utf-8", "Đã hiện lên màn hình (" + String(ms) + " ms)");
    }, handleImageUpload);

    server.on("/page", HTTP_POST, []() {
        if (!pageUploadOk || bytesReceived != (pageUploadMono ? MONO_BYTES : PAGE_BYTES)) {
            return server.send(400, "text/plain; charset=utf-8", "Lưu trang thất bại (sai kích thước hoặc hết bộ nhớ)");
        }
        server.send(200, "text/plain; charset=utf-8", "ok");
    }, handlePageUpload);

    server.on("/doc", HTTP_GET, []() {
        String json = "{\"count\":" + String(pageCount) + ",\"current\":" + String(currentPage) +
                      ",\"free\":" + String(FFat.freeBytes()) + "}";
        server.send(200, "application/json", json);
    });
    server.on("/doc/begin", HTTP_POST, []() {
        int count = server.arg("count").toInt();
        uint32_t perPage = server.arg("mono") == "1" ? MONO_BYTES : PAGE_BYTES;
        if (count < 1 || count > MAX_PAGES) {
            return server.send(400, "text/plain; charset=utf-8", "Tài liệu tối đa " + String(MAX_PAGES) + " trang");
        }
        erasePages(); // free the old document first: its space counts towards the new one
        const uint32_t CLUSTER = 4096; // FAT allocates whole clusters, so a page costs a bit more
        uint32_t onDisk = (perPage + CLUSTER - 1) / CLUSTER * CLUSTER;
        uint32_t fits = FFat.freeBytes() / onDisk;
        if ((uint32_t)count > fits) {
            return server.send(400, "text/plain; charset=utf-8",
                "Không đủ bộ nhớ: chứa được khoảng " + String(fits) + " trang, tài liệu có " + String(count) +
                " trang. Hãy giảm cỡ chữ để bớt số trang.");
        }
        pageCount = 0;
        prefs.begin("doc", false);
        prefs.putInt("count", 0);
        prefs.putInt("cur", 0);
        prefs.end();
        server.send(200, "text/plain", "ok");
        Serial.printf("Receiving a %d page document\n", count);
    });
    server.on("/doc/end", HTTP_POST, []() {
        int count = 0;
        while (count < MAX_PAGES && pageExists(count)) count++;
        pageCount = count;
        prefs.begin("doc", false);
        prefs.putInt("count", count);
        prefs.end();
        showStoredPage(0);
        server.send(200, "text/plain; charset=utf-8", "Đã lưu " + String(count) + " trang");
    });
    server.on("/doc/step", HTTP_POST, []() {
        if (pageCount <= 0) return server.send(404, "text/plain; charset=utf-8", "Board chưa có tài liệu");
        stepPage(server.arg("d").toInt() < 0 ? -1 : 1);
        server.send(200, "text/plain; charset=utf-8", "Đang hiện trang " + String(currentPage + 1) + "/" + String(pageCount));
    });
    server.on("/doc/erase", HTTP_POST, []() {
        erasePages();
        pageCount = 0;
        currentPage = 0;
        prefs.begin("doc", false);
        prefs.putInt("count", 0);
        prefs.putInt("cur", 0);
        prefs.end();
        String hint = "Open http://" + WiFi.localIP().toString() + "/";
        showBanner(hint.c_str());
        server.send(200, "text/plain; charset=utf-8", "Đã xóa tài liệu trong board");
    });

    server.on("/clear", HTTP_POST, []() {
        String hint = "Open http://" + WiFi.localIP().toString() + "/";
        showBanner(hint.c_str());
        server.send(200, "text/plain; charset=utf-8", "Đã xóa màn hình");
    });
    server.begin();
}

void loop() {
    server.handleClient();

    // BOOT button: short press = next page, hold 1 s = previous page.
    static unsigned long pressedMs = 0;
    static bool longDone = false;
    // GPIO0 is also the download-boot strapping pin, which the programmer holds low across a
    // reset: wait until it has been released once, or every upload would flip a page.
    static bool armed = false;
    bool down = digitalRead(BOOT_BUTTON) == LOW;
    if (!armed) {
        if (!down) armed = true;
        return;
    }
    if (down && !pressedMs) {
        pressedMs = millis();
        longDone = false;
    } else if (down && !longDone && millis() - pressedMs > 1000) {
        stepPage(-1);
        longDone = true;
    } else if (!down && pressedMs) {
        if (!longDone && millis() - pressedMs > 40) stepPage(1); // 40 ms debounce
        pressedMs = 0;
    }
}
