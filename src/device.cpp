#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <FFat.h>
#include <Preferences.h>
#include <time.h>
#include <driver/i2s.h>
#include <vector>
#include <algorithm>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <mbedtls/base64.h>
#include "json_escape.h"
#include "rgb_led.h"
#include "screen_page.h"
#include "wifi_config.h"

#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  13
#define BUTTON    0 // the BOOT button: tap = next / act, hold = open / back
// EC11 rotary encoder (optional): turn = move, press = select, hold = back. With nothing
// wired the pins sit high through their pull-ups, so the firmware behaves as before.
#define ENC_A     4
#define ENC_B     5
#define ENC_SW    6
#define BACK_BTN  7 // optional "back" button: one leg here, the other to GND

const int W = 240, H = 320;
const uint32_t PAGE_BYTES = (uint32_t)W * H * 2;
const uint32_t MONO_BYTES = 4 + (uint32_t)W * H / 8;
const int MAX_PAGES = 800;
const unsigned long HOLD_MS = 700;
const char *HOSTNAME = "esp32";

Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8f; // UTF-8 text (Vietnamese) on the same panel
WebServer server(80);
Preferences prefs;

uint16_t lineBuf[W];
uint32_t pixelsDrawn = 0, bytesReceived = 0;
uint8_t carryByte = 0;
bool hasCarry = false;
unsigned long uploadStartMs = 0;

File pageFile;
bool pageUploadOk = false, pageUploadMono = false;
int pageCount = 0, currentPage = 0;
int recvSlot = -1; // document slot currently being uploaded

// The screen shows the home clock (appIndex == HOME, -2), the menu (-1) or one running app.
int appIndex = -1, menuSel = 0;
volatile uint32_t encEdgesA = 0, encEdgesB = 0; // per-contact encoder edge counts, to spot a dead wire
long homeTimerShownSec = -2; // second last drawn on the home clock's timer line

// Press counters, so wiring can be checked over the network instead of by watching the screen.
uint32_t bootTaps = 0, knobTaps = 0, backTaps = 0, rotations = 0;

// Each document is a folder /d<slot> holding its pages and a name.txt, so several documents
// live on the board at once and the reader picks one.
const int MAX_DOCS = 8;
int curDoc = -1;

String docDir(int slot) { return "/d" + String(slot); }
String docNamePath(int slot) { return docDir(slot) + "/name.txt"; }
String monoPagePath(int slot, int i) { return docDir(slot) + "/m" + String(i) + ".bin"; }
String colorPagePath(int slot, int i) { return docDir(slot) + "/p" + String(i) + ".bin"; }
bool pageExists(int slot, int i) { return FFat.exists(monoPagePath(slot, i)) || FFat.exists(colorPagePath(slot, i)); }
bool docExists(int slot) { return slot >= 0 && slot < MAX_DOCS && FFat.exists(docNamePath(slot)); }

int docPages(int slot) {
    int n = 0;
    while (n < MAX_PAGES && pageExists(slot, n)) n++;
    return n;
}

String docName(int slot) {
    File f = FFat.open(docNamePath(slot), FILE_READ);
    if (!f) return "";
    String name = f.readString();
    f.close();
    name.trim();
    return name.isEmpty() ? "Document " + String(slot + 1) : name;
}

void docErase(int slot) {
    // Walk the folder instead of probing 800 file names, in small batches so the paths fit
    // on the stack: collect a batch, close the directory, delete, then walk again.
    for (int round = 0; round < 64; round++) {
        File dir = FFat.open(docDir(slot));
        if (!dir || !dir.isDirectory()) break;
        String batch[32];
        int n = 0;
        for (File f = dir.openNextFile(); f && n < 32; f = dir.openNextFile()) {
            batch[n++] = f.path();
            f.close();
        }
        dir.close();
        if (n == 0) break;
        for (int i = 0; i < n; i++) FFat.remove(batch[i]);
    }
    FFat.rmdir(docDir(slot));
}

int docSavedPage(int slot) {
    prefs.begin("doc", true);
    int page = prefs.getInt(("p" + String(slot)).c_str(), 0);
    prefs.end();
    return page;
}

void docSavePage(int slot, int page) {
    prefs.begin("doc", false);
    prefs.putInt(("p" + String(slot)).c_str(), page);
    prefs.end();
}

// Battery: a 3.7 V LiPo through two equal resistors into GPIO1 (ADC1, which keeps working
// while WiFi is on). With no battery wired the pin floats, so a reading outside the range a
// LiPo can produce means "running on USB" rather than a made-up percentage.
#define BATTERY_PIN 1
const float BATTERY_DIVIDER = 2.0f;

// The ADC is noisy by tens of mV and the LiPo curve is steep around 3.7 V, so a raw reading
// would jump several percent between samples. Take the median of a burst (kills spikes from
// WiFi transmit bursts) and then smooth it over time.
float batteryVoltsRaw() {
    const int N = 15;
    uint32_t s[N];
    for (int i = 0; i < N; i++) s[i] = analogReadMilliVolts(BATTERY_PIN);
    for (int i = 1; i < N; i++) { // insertion sort, N is tiny
        uint32_t key = s[i];
        int j = i - 1;
        for (; j >= 0 && s[j] > key; j--) s[j + 1] = s[j];
        s[j + 1] = key;
    }
    return s[N / 2] * BATTERY_DIVIDER / 1000.0f;
}

float batterySmoothed = 0;
float batteryRecent[10]; // last 5 seconds of raw readings
int batteryRecentAt = 0, batteryRecentCount = 0;

void batteryUpdate() { // called about twice a second: time constant is roughly 10 seconds
    float v = batteryVoltsRaw();
    batterySmoothed = batterySmoothed == 0 ? v : batterySmoothed + (v - batterySmoothed) * 0.05f;
    batteryRecent[batteryRecentAt] = v;
    batteryRecentAt = (batteryRecentAt + 1) % 10;
    if (batteryRecentCount < 10) batteryRecentCount++;
}

float batteryVolts() {
    if (batterySmoothed == 0) batteryUpdate();
    return batterySmoothed;
}

float batterySpread() { // how much the raw readings move: a wired battery barely moves
    if (batteryRecentCount < 3) return 0;
    float lo = batteryRecent[0], hi = batteryRecent[0];
    for (int i = 1; i < batteryRecentCount; i++) {
        lo = min(lo, batteryRecent[i]);
        hi = max(hi, batteryRecent[i]);
    }
    return hi - lo;
}

// An unwired ADC pin floats and drifts across volts, which would otherwise look like a
// battery, so a reading only counts as a battery when it is also steady.
bool batteryPresent(float v) { return v > 2.8f && v < 4.6f && batterySpread() < 0.25f; }

int batteryPercent(float v) { // a LiPo's voltage is not linear in charge, so interpolate a curve
    const float curve[][2] = {{3.30f, 0}, {3.50f, 10}, {3.60f, 20}, {3.70f, 40},
                              {3.80f, 60}, {3.90f, 75}, {4.00f, 88}, {4.20f, 100}};
    if (v <= curve[0][0]) return 0;
    for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
        if (v > curve[i][0]) continue;
        float span = curve[i][0] - curve[i - 1][0];
        return curve[i - 1][1] + (v - curve[i - 1][0]) / span * (curve[i][1] - curve[i - 1][1]);
    }
    return 100;
}

// Shared look: black screens, a light-blue accent (also the clock's weekday) and a quiet divider.
const uint16_t ACCENT = ((90 & 0xF8) << 8) | ((200 & 0xFC) << 3) | (255 >> 3);
const uint16_t DIVIDER = ((45 & 0xF8) << 8) | ((45 & 0xFC) << 3) | (52 >> 3);
const uint16_t MUTED = ((160 & 0xF8) << 8) | ((160 & 0xFC) << 3) | (160 >> 3);

// Battery indicator in the title bar, so every screen that has one shows it.
void drawBatteryIcon() {
    const int x = 190, y = 5, w = 36, h = 14;
    float v = batteryVolts();
    tft.fillRect(x - 2, y - 2, W - x + 2, h + 4, ST77XX_BLACK); // clear the old indicator
    if (!batteryPresent(v)) {
        tft.setTextColor(MUTED, ST77XX_BLACK);
        tft.setTextSize(1);
        tft.setCursor(x + 6, y + 4);
        tft.print("USB");
        return;
    }
    int pct = batteryPercent(v);
    uint16_t color = pct > 50 ? ST77XX_GREEN : pct > 20 ? ST77XX_YELLOW : ST77XX_RED;
    tft.drawRect(x, y, w, h, MUTED);
    tft.fillRect(x + w, y + 4, 3, h - 8, MUTED); // the nub
    tft.fillRect(x + 2, y + 2, (w - 4) * pct / 100, h - 4, color);
}

void screenInit() {
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); // the board definition's default SPI pins are other ones
    tft.init(W, H);
    tft.setRotation(0);
    tft.setTextWrap(false); // a long line must be clipped, never wrapped onto the layout below
    u8f.begin(tft);
    u8f.setFont(u8g2_font_unifont_t_vietnamese2);
    u8f.setFontMode(1); // transparent: rows are cleared before drawing
}

// The built-in font is ASCII only, so Vietnamese text from the phone (place names, file names)
// is shown without its accents instead of as garbage glyphs: "Hồ Gươm" -> "Ho Guom".
String asciiFold(const String &in) {
    static const char *GROUPS[][2] = {
        {"a", "àáảãạăằắẳẵặâầấẩẫậ"}, {"A", "ÀÁẢÃẠĂẰẮẲẴẶÂẦẤẨẪẬ"}, {"e", "èéẻẽẹêềếểễệ"}, {"E", "ÈÉẺẼẸÊỀẾỂỄỆ"},
        {"i", "ìíỉĩị"}, {"I", "ÌÍỈĨỊ"}, {"o", "òóỏõọôồốổỗộơờớởỡợ"}, {"O", "ÒÓỎÕỌÔỒỐỔỖỘƠỜỚỞỠỢ"},
        {"u", "ùúủũụưừứửữự"}, {"U", "ÙÚỦŨỤƯỪỨỬỮỰ"}, {"y", "ỳýỷỹỵ"}, {"Y", "ỲÝỶỸỴ"}, {"d", "đ"}, {"D", "Đ"},
    };
    String out;
    const char *p = in.c_str();
    while (*p) {
        uint8_t c = *p;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (len == 1) {
            out += (char)c;
            p++;
            continue;
        }
        String ch = String(p).substring(0, len);
        const char *base = "?";
        for (auto &g : GROUPS) {
            if (strstr(g[1], ch.c_str())) {
                base = g[0];
                break;
            }
        }
        out += base;
        p += len;
    }
    return out;
}

// The built-in font has no Vietnamese letters, so on-screen labels are unaccented.
// Anything the phone sends is drawn by the phone, so it keeps its accents.
// Title bar: the name on black, underlined by a short accent stroke, above a thin divider.
// It spans y 0-26; screens draw their content from y 28.
void header(const char *title) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 5);
    tft.print(title);
    tft.fillRect(10, 23, strlen(title) * 12 - 2, 2, ACCENT);
    tft.drawFastHLine(0, 26, W, DIVIDER);
    tft.setTextSize(1);
    drawBatteryIcon();
}

void line(int y, const String &text, uint16_t color = ST77XX_WHITE, uint8_t size = 2) {
    tft.setTextColor(color, ST77XX_BLACK);
    tft.setTextSize(size);
    tft.setCursor(8, y);
    tft.print(text);
}

bool showStoredPage(int i) {
    if (curDoc < 0 || i < 0 || i >= pageCount) return false;
    bool mono = FFat.exists(monoPagePath(curDoc, i));
    File f = FFat.open(mono ? monoPagePath(curDoc, i) : colorPagePath(curDoc, i), FILE_READ);
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
    docSavePage(curDoc, i);
    return true;
}

// Switches the reader to another stored document, resuming where it was left off.
bool openDoc(int slot) {
    if (!docExists(slot)) return false;
    curDoc = slot;
    pageCount = docPages(slot);
    prefs.begin("doc", false);
    prefs.putInt("curdoc", slot);
    prefs.end();
    currentPage = min(docSavedPage(slot), pageCount - 1);
    return showStoredPage(currentPage);
}

void stepPage(int delta) {
    if (pageCount <= 0) return;
    showStoredPage((currentPage + delta + pageCount) % pageCount);
}

// ---------- apps ----------

// The reader has two screens: the list of stored documents, and the document itself.
// Names and page counts are read from flash once and kept in RAM: reading them again on
// every turn of the knob made scrolling the list crawl.
struct DocInfo {
    int slot, pages;
    String name;
};
DocInfo docCache[MAX_DOCS];
int docCacheN = 0;
bool readerInList = true;
int readerAt = 0; // index into docCache

void refreshDocCache() {
    docCacheN = 0;
    for (int s = 0; s < MAX_DOCS; s++) {
        if (docExists(s)) docCache[docCacheN++] = {s, docPages(s), docName(s)};
    }
    if (readerAt >= docCacheN) readerAt = 0;
}

void readerSelectSlot(int slot) {
    for (int i = 0; i < docCacheN; i++) {
        if (docCache[i].slot == slot) readerAt = i;
    }
}

void readerDrawRow(int i) { // one row: name plus page info, painted over the old text
    if (i < 0 || i >= docCacheN) return;
    const DocInfo &d = docCache[i];
    bool sel = i == readerAt;
    String row = String(sel ? ">" : " ") + asciiFold(d.name);
    if (row.length() > 19) row = row.substring(0, 19);
    while (row.length() < 19) row += ' ';
    line(40 + i * 28, row, sel ? ST77XX_YELLOW : ST77XX_WHITE, 2);
    String sub = String(d.pages) + " pages" +
        (d.slot == curDoc ? ", page " + String(currentPage + 1) : "");
    while (sub.length() < 34) sub += ' ';
    line(40 + i * 28 + 18, sub, ST77XX_CYAN, 1);
}

void readerDrawList() {
    header("Documents");
    refreshDocCache();
    if (docCacheN == 0) {
        line(50, "No documents");
        line(80, "http://" + WiFi.localIP().toString() + "/", ST77XX_YELLOW, 1);
        return;
    }
    for (int i = 0; i < docCacheN; i++) readerDrawRow(i);
}

void readerEnter() {
    readerInList = true;
    readerDrawList();
}

void readerPress() {
    if (!readerInList) {
        stepPage(1);
    } else if (docCacheN > 0 && openDoc(docCache[readerAt].slot)) {
        readerInList = false;
    }
}

void receiveEnter() {
    header("From phone");
    line(60, "http://" + WiFi.localIP().toString() + "/", ST77XX_YELLOW);
    line(92, "http://esp32.local/", ST77XX_YELLOW, 1);
}

int lastClockMinute = -1;

// Optional photo behind the clock, sent from the phone as 240x320 RGB565 and kept in PSRAM
// (and in /clockbg.bin across reboots). Text on it can't use a solid background box, so each
// refresh first repaints that strip of the photo, then draws the text with a dark shadow.
const char *CLOCK_BG_PATH = "/clockbg.bin";
uint16_t *clockBg = nullptr;

void clockBgLoad() {
    File f = FFat.open(CLOCK_BG_PATH, FILE_READ);
    if (!f) return;
    if (f.size() == PAGE_BYTES) {
        if (!clockBg) clockBg = (uint16_t *)ps_malloc(PAGE_BYTES);
        if (clockBg && f.read((uint8_t *)clockBg, PAGE_BYTES) != PAGE_BYTES) {
            free(clockBg);
            clockBg = nullptr;
        }
    }
    f.close();
}

void clockBgRestore(int y, int h) {
    y = max(y, 0);
    if (y >= H) return;
    h = min(h, H - y);
    if (!clockBg) {
        tft.fillRect(0, y, W, h, ST77XX_BLACK);
        return;
    }
    tft.drawRGBBitmap(0, y, clockBg + y * W, W, h);
}

// A centred line of the clock screen. On black it is padded to `cells` characters and drawn with
// a solid background, so it overwrites the previous text without a clear (no flicker). On a
// photo it repaints its strip of the photo first, then draws the text over a dark shadow.
void clockLine(int y, const String &text, uint16_t color, uint8_t size, int cells) {
    tft.setTextSize(size);
    String t = text;
    if (!clockBg) {
        int pad = max(0, cells - (int)t.length());
        String left;
        for (int i = 0; i < pad / 2; i++) left += ' ';
        t = left + t;
        while ((int)t.length() < cells) t += ' ';
        tft.setTextColor(color, ST77XX_BLACK);
        tft.setCursor((W - (int)t.length() * 6 * size + size) / 2, y);
        tft.print(t);
        return;
    }
    clockBgRestore(y, 8 * size);
    int x = (W - (int)t.length() * 6 * size + size) / 2;
    int o = size >= 4 ? 2 : 1;
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(x + o, y + o);
    tft.print(t);
    tft.setTextColor(color);
    tft.setCursor(x, y);
    tft.print(t);
}

// Home screen: no title bar and no status lines, just the time, with the weekday above and the
// date below, centred on black or on the photo from the phone.
const int CLOCK_DAY_Y = 72, CLOCK_TIME_Y = 108, CLOCK_DATE_Y = 188, CLOCK_TIMER_Y = 250;

// HH:MM at text size 8. The font's colon sits in a full-width cell and leaves wide gaps, so the
// two digit pairs are printed separately around a colon drawn as two squares.
void clockTime(const char *hhmm) {
    const int size = 8, pairW = 2 * 6 * size - size, gap = 10, dot = size;
    const int x0 = (W - (2 * pairW + 2 * gap + dot)) / 2, x1 = x0 + pairW + gap + dot + gap, cx = x0 + pairW + gap;
    const int y = CLOCK_TIME_Y;
    String hh = String(hhmm).substring(0, 2), mm = String(hhmm).substring(3, 5);
    tft.setTextSize(size);
    auto draw = [&](int o, uint16_t color, bool solid) {
        if (solid) tft.setTextColor(color, ST77XX_BLACK);
        else tft.setTextColor(color);
        tft.setCursor(x0 + o, y + o);
        tft.print(hh);
        tft.setCursor(x1 + o, y + o);
        tft.print(mm);
        tft.fillRect(cx + o, y + 2 * size + o, dot, dot, color);
        tft.fillRect(cx + o, y + 5 * size + o, dot, dot, color);
    };
    if (!clockBg) {
        draw(0, ST77XX_WHITE, true);
        return;
    }
    clockBgRestore(y, 8 * size);
    draw(2, ST77XX_BLACK, false); // shadow
    draw(0, ST77XX_WHITE, false);
}

void clockEnter() {
    lastClockMinute = -1;
    clockBgRestore(0, H);
    clockLine(150, "Waiting for time", tft.color565(150, 150, 150), 1, 16);
}

void clockTick() {
    time_t now = time(nullptr);
    if (now < 8 * 3600) return; // NTP has not answered yet
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_min == lastClockMinute) return;
    if (lastClockMinute < 0) clockBgRestore(0, H); // clears the "waiting for time" note
    lastClockMinute = t.tm_min;

    static const char *DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    char buf[32];
    clockLine(CLOCK_DAY_Y, DAYS[t.tm_wday], ACCENT, 2, 9);
    strftime(buf, sizeof(buf), "%H:%M", &t);
    clockTime(buf);
    strftime(buf, sizeof(buf), "%d/%m/%Y", &t);
    clockLine(CLOCK_DATE_Y, buf, tft.color565(200, 200, 200), 2, 10);
}

const char *AP_SSID = "ESP32-Device";
const IPAddress AP_IP(192, 168, 4, 1);
DNSServer dns;
bool apOn = false;

void apSave(bool on) {
    prefs.begin("ap", false);
    prefs.putBool("on", on);
    prefs.end();
}

void apStart() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    // Same channel as the home WiFi: the radio has one channel, so a mismatch would make
    // the access point hop and briefly drop the phones connected to it.
    WiFi.softAP(AP_SSID, nullptr, WiFi.status() == WL_CONNECTED ? WiFi.channel() : 1, 0, 8);
    dns.start(53, "*", AP_IP); // every name resolves here, so the page pops up on joining
    apOn = true;
    Serial.printf("Access point \"%s\" on at http://%s/\n", AP_SSID, AP_IP.toString().c_str());
}

void apStop() {
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apOn = false;
    Serial.println("Access point off");
}

int lastApClients = -1;
void hotspotDraw() {
    header("Hotspot");
    lastApClients = -1;
    if (apOn) {
        line(50, AP_SSID, ST77XX_WHITE, 2);
        line(82, "no password", ST77XX_WHITE, 1);
        line(110, "http://192.168.4.1/", ST77XX_YELLOW, 2);
    } else {
        line(50, "Off", ST77XX_WHITE, 2);
    }
}
void hotspotEnter() { hotspotDraw(); }
void hotspotTick() {
    if (!apOn) return;
    int clients = WiFi.softAPgetStationNum();
    if (clients == lastApClients) return;
    lastApClients = clients;
    line(160, "Clients: " + String(clients) + "  ", ST77XX_WHITE, 2);
}
void hotspotPress() {
    apOn ? apStop() : apStart();
    apSave(apOn);
    hotspotDraw();
}

int radarState = 0; // 0 = scanning, 1 = done
void radarEnter() {
    header("WiFi scan");
    line(50, "Scanning...");
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    radarState = 0;
}
void radarTick() {
    if (radarState) return;
    int n = WiFi.scanComplete();
    if (n < 0) return;
    header("WiFi scan");
    int y = 34;
    for (int i = 0; i < n && i < 9; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() > 17) ssid = ssid.substring(0, 17);
        int rssi = WiFi.RSSI(i);
        uint16_t c = rssi >= -60 ? ST77XX_GREEN : rssi >= -75 ? ST77XX_YELLOW : ST77XX_RED;
        line(y, ssid, c, 1);
        tft.setCursor(190, y);
        tft.print(rssi);
        y += 14;
    }
    line(y + 10, String(n) + " networks", ST77XX_WHITE, 1);
    WiFi.scanDelete();
    radarState = 1;
}
void radarPress() { radarEnter(); }

struct Preset {
    const char *name;
    uint8_t r, g, b;
};
const Preset PRESETS[] = {{"Off", 0, 0, 0}, {"Red", 255, 0, 0}, {"Orange", 255, 64, 0}, {"Yellow", 255, 176, 0},
    {"Green", 0, 255, 0}, {"Cyan", 0, 255, 255}, {"Blue", 0, 0, 255}, {"Purple", 128, 0, 255},
    {"Pink", 255, 0, 96}, {"White", 255, 255, 255}};
int presetIndex = 0;
void ledShow() {
    const Preset &p = PRESETS[presetIndex];
    setLed(p.r, p.g, p.b);
    header("RGB light");
    line(60, String(p.name) + "        ", ST77XX_WHITE, 3);
    tft.fillRect(40, 120, 160, 80, tft.color565(p.r, p.g, p.b));
}
void ledEnter() { ledShow(); }
void ledPress() {
    presetIndex = (presetIndex + 1) % (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
    ledShow();
}

void infoTick() {
    static unsigned long last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    int y = 40;
    line(y, "WiFi: " + String(WiFi.status() == WL_CONNECTED ? WIFI_SSID : "not connected") + "    ", ST77XX_WHITE, 1);
    line(y += 16, "IP:   " + WiFi.localIP().toString() + "     ", ST77XX_WHITE, 1);
    line(y += 16, "Signal: " + String((int)WiFi.RSSI()) + " dBm   ", ST77XX_WHITE, 1);
    line(y += 16, "Uptime: " + String(millis() / 60000) + " min   ", ST77XX_WHITE, 1);
    line(y += 16, "Free RAM: " + String(ESP.getFreeHeap() / 1024) + " KB   ", ST77XX_WHITE, 1);
    line(y += 16, "PSRAM:     " + String(ESP.getFreePsram() / 1024) + " KB   ", ST77XX_WHITE, 1);
    line(y += 16, "Storage free: " + String(FFat.freeBytes() / 1048576) + " MB   ", ST77XX_WHITE, 1);
    line(y += 16, "Pages: " + String(pageCount) + "    ", ST77XX_WHITE, 1);
    line(y += 16, "Chip temp: " + String(temperatureRead(), 1) + " C  ", ST77XX_WHITE, 1);
    float v = batteryVolts();
    line(y += 16, batteryPresent(v) ? "Battery: " + String(batteryPercent(v)) + "% (" + String(v, 2) + "V)  "
                                    : String("Battery: USB power"), ST77XX_WHITE, 1);
}
void infoEnter() { header("Info"); }

// ---------- microphone ----------
// INMP441 I2S microphone: SCK=GPIO15, WS=GPIO16, SD=GPIO17, L/R to GND (left slot).
// A background task reads it continuously and publishes the level, so the screen and /mic
// never wait on audio. Both slots are measured so a wrong L/R strap shows up in /mic.
#define MIC_SCK 15
#define MIC_WS  16
#define MIC_SD  17
const int MIC_RATE = 16000;

struct MicSlot {
    float rms;          // after removing the DC offset, in 24-bit counts
    int32_t min, max;   // raw 24-bit range over the window
};
volatile float micDb = 0; // approximate sound level, dB SPL
MicSlot micSlot[2];       // [0] = left, [1] = right
volatile uint32_t micWindows = 0;
// /mic/raw asks the task for one buffer of untouched I2S words, to check bit alignment.
volatile bool micCaptureReq = false, micCaptureDone = false;
int32_t micCapture[512];
bool micReady = false;
void recPush(float x);
extern volatile bool recording;

// INMP441 sensitivity is -26 dBFS at 94 dB SPL, so dB SPL = dBFS + 120.
float micToDb(float rms) {
    float db = 20.0f * log10f(rms / 8388608.0f + 1e-9f) + 120.0f;
    return db < 0 ? 0 : db;
}

// 2nd-order Butterworth high-pass at 100 Hz: removes the DC offset and the rumble from air
// movement and handling, which otherwise dominate the level of a MEMS mic.
struct HighPass {
    float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    HighPass() {
        float w = 2 * PI * 100 / MIC_RATE, c = cosf(w), alpha = sinf(w) / (2 * 0.7071f), a0 = 1 + alpha;
        b0 = (1 + c) / 2 / a0;
        b1 = -(1 + c) / a0;
        b2 = b0;
        a1 = -2 * c / a0;
        a2 = (1 - alpha) / a0;
    }
    float step(float x) { // transposed direct form II
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

void micTask(void *) {
    static int32_t buf[512]; // 256 stereo frames = 16 ms
    HighPass hp[2];
    float sum[2] = {0, 0};
    int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
    int frames = 0;
    for (;;) {
        size_t got = 0;
        i2s_read(I2S_NUM_0, buf, sizeof(buf), &got, portMAX_DELAY);
        if (micCaptureReq && got == sizeof(buf)) {
            memcpy(micCapture, buf, sizeof(buf));
            micCaptureReq = false;
            micCaptureDone = true;
        }
        for (size_t i = 0; i + 1 < got / 4; i += 2) {
            for (int c = 0; c < 2; c++) { // left slot first in each frame (checked with /mic/raw)
                int32_t v = buf[i + c] >> 8; // 24-bit sample, MSB-aligned in 32
                lo[c] = min(lo[c], v);
                hi[c] = max(hi[c], v);
                float x = hp[c].step(v);
                if (c == 0 && recording) recPush(x);
                sum[c] += x * x;
            }
            frames++;
        }
        if (frames >= MIC_RATE / 10) { // publish every 100 ms
            for (int c = 0; c < 2; c++) {
                micSlot[c] = {sqrtf(sum[c] / frames), lo[c], hi[c]};
                sum[c] = 0;
                lo[c] = INT32_MAX;
                hi[c] = INT32_MIN;
            }
            micDb = micToDb(micSlot[0].rms);
            micWindows++;
            frames = 0;
        }
    }
}

void micBegin() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = MIC_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 256;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = MIC_SCK;
    pins.ws_io_num = MIC_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = MIC_SD;
    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK || i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
        Serial.println("Mic: I2S init failed");
        return;
    }
    micReady = true;
    xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 1, nullptr, 0);
}

// Sound meter screen: big level, a bar, the loudest level since opening, and a sweeping graph.
const int MIC_DB_LO = 30, MIC_DB_HI = 100;
const int GRAPH_X = 8, GRAPH_W = 224, GRAPH_TOP = 176, GRAPH_H = 136;
int micGraphX = 0;
float micMax = 0;

uint16_t micColor(float db) {
    return db < 60 ? ST77XX_GREEN : db < 80 ? ST77XX_YELLOW : ST77XX_RED;
}
int micScale(float db, int span) {
    return constrain((int)((db - MIC_DB_LO) * span / (MIC_DB_HI - MIC_DB_LO)), 0, span);
}
void micEnter() {
    header("Sound meter");
    micGraphX = 0;
    micMax = 0;
    tft.drawFastHLine(GRAPH_X, GRAPH_TOP - 4, GRAPH_W, tft.color565(60, 60, 60));
}
void micTick() {
    static unsigned long last = 0;
    if (millis() - last < 100) return;
    last = millis();
    if (!micReady) {
        line(60, "No microphone", ST77XX_RED, 2);
        return;
    }
    float db = micDb;
    micMax = max(micMax, db);
    uint16_t c = micColor(db);

    char num[8];
    snprintf(num, sizeof(num), "%3d", (int)lroundf(db));
    tft.setTextColor(c, ST77XX_BLACK);
    tft.setTextSize(6);
    tft.setCursor(24, 40);
    tft.print(num);
    tft.setTextSize(3);
    tft.setCursor(140, 61);
    tft.print("dB");

    int w = micScale(db, 224);
    tft.fillRect(8, 100, w, 20, c);
    tft.fillRect(8 + w, 100, 224 - w, 20, tft.color565(40, 40, 40));

    line(134, "Max " + String((int)lroundf(micMax)) + " dB   ", micColor(micMax), 2);

    // Sweep the graph like an oscilloscope: one column per update, a blank column ahead of it
    // marks where the trace is, and nothing is ever cleared wholesale.
    int x = GRAPH_X + micGraphX;
    int h = micScale(db, GRAPH_H);
    tft.drawFastVLine(x, GRAPH_TOP, GRAPH_H - h, ST77XX_BLACK);
    tft.drawFastVLine(x, GRAPH_TOP + GRAPH_H - h, h, c);
    uint16_t grid = tft.color565(70, 70, 70);
    for (int g = 60; g <= 80; g += 20) {
        int gy = GRAPH_TOP + GRAPH_H - micScale(g, GRAPH_H);
        if (gy < GRAPH_TOP + GRAPH_H - h) tft.drawPixel(x, gy, grid);
    }
    micGraphX = (micGraphX + 1) % GRAPH_W;
    tft.drawFastVLine(GRAPH_X + micGraphX, GRAPH_TOP, GRAPH_H, ST77XX_BLACK);
}
void micPress() { micEnter(); } // knob press: reset the max and the graph

// ---------- recorder ----------
// Knob press starts and stops a recording. The mic task drops samples into a PSRAM ring and a
// writer task moves them into a WAV file, so a slow flash write never loses audio. Leaving the
// app (back button, BOOT hold) stops and saves too.
const char *REC_DIR = "/rec";
const float REC_GAIN = 8; // +18 dB: speech is quiet at the mic's 24-bit full scale
const uint32_t REC_RING = MIC_RATE * 8; // 8 s of slack for the writer
const uint32_t REC_MAX_SAMPLES = MIC_RATE * 60 * 30; // 30 min per file
int16_t *recRing = nullptr;
volatile uint32_t recHead = 0, recTail = 0; // head moves in the mic task, tail in the writer
volatile bool recording = false;
volatile uint32_t recDropped = 0;
File recFile;
String recName, recLastSaved;
uint32_t recSamples = 0;
unsigned long recStartMs = 0;
SemaphoreHandle_t recLock;

void recPush(float x) { // called from the mic task for every filtered sample
    uint32_t h = recHead;
    if (h - recTail >= REC_RING) {
        recDropped++;
        return;
    }
    recRing[h % REC_RING] = (int16_t)constrain(lroundf(x * REC_GAIN / 256), -32767, 32767);
    recHead = h + 1;
}

// Moves up to `limit` buffered samples into the file. Caller holds recLock.
uint32_t recDrain(uint32_t limit) {
    static int16_t chunk[2048];
    uint32_t n = min(recHead - recTail, min(limit, (uint32_t)2048));
    for (uint32_t i = 0; i < n; i++) chunk[i] = recRing[(recTail + i) % REC_RING];
    if (n) {
        recFile.write((uint8_t *)chunk, n * 2);
        recTail += n;
        recSamples += n;
    }
    return n;
}

void recWriterTask(void *) {
    for (;;) {
        uint32_t n = 0;
        xSemaphoreTake(recLock, portMAX_DELAY);
        if (recFile) n = recDrain(2048);
        xSemaphoreGive(recLock);
        if (n < 2048) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void writeWavHeader(File &f, uint32_t dataBytes) {
    uint32_t rate = MIC_RATE, byteRate = MIC_RATE * 2, riff = 36 + dataBytes, fmtLen = 16;
    uint16_t pcm = 1, mono = 1, align = 2, bits = 16;
    f.write((const uint8_t *)"RIFF", 4);
    f.write((uint8_t *)&riff, 4);
    f.write((const uint8_t *)"WAVEfmt ", 8);
    f.write((uint8_t *)&fmtLen, 4);
    f.write((uint8_t *)&pcm, 2);
    f.write((uint8_t *)&mono, 2);
    f.write((uint8_t *)&rate, 4);
    f.write((uint8_t *)&byteRate, 4);
    f.write((uint8_t *)&align, 2);
    f.write((uint8_t *)&bits, 2);
    f.write((const uint8_t *)"data", 4);
    f.write((uint8_t *)&dataBytes, 4);
}

// Only names the recorder makes are served or deleted, so a request can't reach other files.
bool validRecName(const String &n) {
    if (n.length() < 5 || n.length() > 40 || !n.endsWith(".wav") || n[0] == '.') return false;
    for (char c : n) {
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

int recOwner = -1;    // the app that started the recording; leaving it stops the recording
bool recToLibrary;    // false for a scratch file (a spoken question), which the list never shows

// With no path the recording goes into the library under a date-time name.
bool recStart(const char *path = nullptr) {
    if (recording || !micReady || !recRing) return false;
    if (FFat.freeBytes() < 512 * 1024) return false;
    char name[32];
    String full;
    if (path) {
        strlcpy(name, path + 1, sizeof(name));
        full = path;
    } else {
        FFat.mkdir(REC_DIR);
        time_t now = time(nullptr);
        if (now > 1700000000) strftime(name, sizeof(name), "%Y%m%d-%H%M%S.wav", localtime(&now));
        else snprintf(name, sizeof(name), "rec%lu.wav", millis()); // no clock yet
        full = String(REC_DIR) + "/" + name;
    }
    xSemaphoreTake(recLock, portMAX_DELAY);
    recFile = FFat.open(full, FILE_WRITE);
    bool ok = (bool)recFile;
    if (ok) {
        writeWavHeader(recFile, 0);
        recName = name;
        recSamples = 0;
        recDropped = 0;
        recTail = recHead; // start from now, not from whatever the ring held
        recStartMs = millis();
        recOwner = appIndex;
        recToLibrary = !path;
        recording = true;
    }
    xSemaphoreGive(recLock);
    Serial.printf("Rec start %s: %s\n", name, ok ? "ok" : "open failed");
    return ok;
}

void recStop() {
    if (!recording) return;
    recording = false;
    xSemaphoreTake(recLock, portMAX_DELAY);
    while (recDrain(2048)) {}
    recFile.seek(0);
    writeWavHeader(recFile, recSamples * 2);
    recFile.close();
    xSemaphoreGive(recLock);
    if (recToLibrary) recLastSaved = recName;
    Serial.printf("Rec saved %s: %.1f s, dropped %u\n", recName.c_str(), recSamples / (float)MIC_RATE, recDropped);
}

void recBegin() {
    recRing = (int16_t *)ps_malloc(REC_RING * sizeof(int16_t));
    recLock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(recWriterTask, "recwrite", 4096, nullptr, 1, nullptr, 1);
}

int recFileCount() {
    int n = 0;
    File dir = FFat.open(REC_DIR);
    if (!dir) return 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) n++;
    return n;
}

String mmss(uint32_t sec) {
    char t[12];
    snprintf(t, sizeof(t), "%02u:%02u", sec / 60, sec % 60);
    return t;
}

void recDraw() {
    tft.fillRect(0, 28, W, H - 28, ST77XX_BLACK);
    tft.fillCircle(120, 90, 38, recording ? ST77XX_RED : tft.color565(150, 0, 0));
    tft.drawCircle(120, 90, 45, ST77XX_WHITE);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setTextSize(5);
    tft.setCursor(45, 160);
    tft.print(mmss(recording ? (millis() - recStartMs) / 1000 : 0));
    if (recording) {
        line(260, "Recording", ST77XX_RED, 2);
    } else if (recLastSaved.length()) {
        line(250, "Saved", ST77XX_GREEN, 2);
        line(272, recLastSaved, ST77XX_WHITE, 1);
    } else {
        line(260, "Ready", ST77XX_WHITE, 2);
    }
    line(300, String(recFileCount()) + " files  " + mmss(FFat.freeBytes() / (MIC_RATE * 2)) + " free", tft.color565(150, 150, 150), 1);
}
void recEnter() {
    header("Recorder");
    recDraw();
}
void recTick() {
    static unsigned long last = 0;
    if (!recording || millis() - last < 200) return;
    last = millis();
    bool on = (millis() - recStartMs) % 1000 < 600; // blinking dot while recording
    tft.fillCircle(120, 90, 38, on ? ST77XX_RED : tft.color565(60, 0, 0));
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setTextSize(5);
    tft.setCursor(45, 160);
    tft.print(mmss((millis() - recStartMs) / 1000));
    float db = micDb;
    int w = micScale(db, 224);
    tft.fillRect(8, 222, w, 14, micColor(db));
    tft.fillRect(8 + w, 222, 224 - w, 14, tft.color565(40, 40, 40));
}
void recPress() {
    if (recording) recStop();
    else recStart();
    recDraw();
}

// ---------- ask AI (voice) ----------
// Knob press records a spoken question, the next press sends the WAV to Gemini, which hears
// it directly and replies with what it heard and an answer. The answer is drawn in a Unifont
// with Vietnamese glyphs (the built-in font has none) and scrolls with the knob. The request
// runs in its own task so the screen and buttons stay live while waiting.
const char *ASK_PATH = "/ask.wav";
const uint32_t ASK_MAX_S = 60;
const char *ASK_DEFAULT_MODEL = "gemini-3.8-flash";
const char *ASK_FALLBACK_MODEL = "gemini-flash-lite-latest"; // tried when the main one is overloaded
const char *ASK_SYSTEM =
    "Bạn là trợ lý giọng nói trên một thiết bị nhỏ có màn hình 240x320. Người dùng hỏi bằng giọng nói "
    "trong file ghi âm. Ghi lại nguyên văn câu người dùng nói vào trường question, rồi trả lời vào trường "
    "answer bằng tiếng Việt có dấu, ngắn gọn (không quá khoảng 120 từ), văn bản thuần, không dùng markdown. "
    "Nếu người dùng hỏi bằng ngôn ngữ khác thì trả lời bằng ngôn ngữ đó. Nếu không nghe rõ thì nói vậy trong answer.";

enum class AskState { Idle, Recording, Thinking, Answer, Failed };
volatile AskState askState = AskState::Idle;
String askQuestion, askAnswer, askError;
unsigned long askSentMs = 0, askTookMs = 0;

// Feeds HTTPClient the request body without holding it in RAM: a JSON prefix, the WAV file
// base64-encoded on the fly, then a JSON suffix.
class Base64FileBody : public Stream {
public:
    Base64FileBody(const String &prefix, File file, const String &suffix) : _prefix(prefix), _file(file), _suffix(suffix) {}
    static size_t base64Length(size_t raw) { return 4 * ((raw + 2) / 3); }
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
            } else if (_b64Pos < _b64Len) {
                size_t n = min(len - done, _b64Len - _b64Pos);
                memcpy(out + done, _b64 + _b64Pos, n);
                done += n;
                _b64Pos += n;
                _sent += n;
            } else if (_file.available()) {
                uint8_t raw[300]; // a multiple of 3, so no padding appears mid-stream
                size_t n = _file.read(raw, sizeof(raw));
                _b64Pos = 0;
                mbedtls_base64_encode(_b64, sizeof(_b64), &_b64Len, raw, n);
            } else {
                size_t at = _sent - _prefix.length() - base64Length(_file.size());
                if (at >= _suffix.length()) break;
                size_t n = min(len - done, _suffix.length() - at);
                memcpy(out + done, _suffix.c_str() + at, n);
                done += n;
                _sent += n;
            }
        }
        return done;
    }
    size_t readBytes(char *out, size_t len) { return readBytes((uint8_t *)out, len); }

private:
    String _prefix, _suffix;
    File _file;
    size_t _sent = 0, _b64Len = 0, _b64Pos = 0;
    uint8_t _b64[404];
};

String askKey() {
    prefs.begin("gemini", true);
    String k = prefs.getString("key", "");
    prefs.end();
    return k;
}
String askModel() {
    prefs.begin("gemini", true);
    String m = prefs.getString("model", ASK_DEFAULT_MODEL);
    prefs.end();
    return m.length() ? m : String(ASK_DEFAULT_MODEL);
}

void askFail(const String &why) {
    askError = why;
    askState = AskState::Failed;
    Serial.println("Ask failed: " + why);
}

// One generateContent call with the recorded question. Returns the HTTP status (<= 0 when the
// connection failed) and the reply body.
int askRequest(const String &key, const String &model, String &payload) {
    File wav = FFat.open(ASK_PATH, FILE_READ);
    if (!wav) return -100;
    String prefix = String("{\"system_instruction\":{\"parts\":[{\"text\":\"") + jsonEscape(ASK_SYSTEM) + "\"}]}," +
        "\"contents\":[{\"role\":\"user\",\"parts\":[{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
    String suffix = "\"}}]}],\"generationConfig\":{\"responseMimeType\":\"application/json\",\"responseSchema\":"
        "{\"type\":\"OBJECT\",\"properties\":{\"question\":{\"type\":\"STRING\"},\"answer\":{\"type\":\"STRING\"}},"
        "\"required\":[\"question\",\"answer\"]}}}";
    Base64FileBody body(prefix, wav, suffix);

    WiFiClientSecure secure;
    secure.setInsecure(); // no root CA bundle on the board; the key still travels over TLS
    HTTPClient http;
    http.setTimeout(60000);
    String url = "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent";
    if (!http.begin(secure, url)) {
        wav.close();
        return -101;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-goog-api-key", key);
    Serial.printf("Ask %s: %u byte WAV, %u byte request\n", model.c_str(), wav.size(), body.total());
    int status = http.sendRequest("POST", &body, body.total());
    payload = http.getString();
    http.end();
    wav.close();
    Serial.printf("Ask HTTP %d after %lu ms, %u bytes\n", status, millis() - askSentMs, payload.length());
    return status;
}

void askTask(void *) {
    String key = askKey(), model = askModel(), payload;
    int status = askRequest(key, model, payload);
    if ((status == 503 || status == 429) && model != ASK_FALLBACK_MODEL) {
        status = askRequest(key, ASK_FALLBACK_MODEL, payload); // demand spikes are per model
    }
    askTookMs = millis() - askSentMs;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (status == -100) {
        askFail("Recording missing");
    } else if (status <= 0) {
        askFail("Connection error " + String(status));
    } else if (err) {
        askFail("HTTP " + String(status) + ": unreadable reply");
    } else if (status != 200) {
        const char *msg = doc["error"]["message"];
        askFail("HTTP " + String(status) + ": " + String(msg ? msg : ""));
    } else {
        String text;
        for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
            if (!(part["thought"] | false)) text += (const char *)(part["text"] | "");
        }
        JsonDocument reply;
        if (text.isEmpty()) {
            const char *why = doc["promptFeedback"]["blockReason"] | doc["candidates"][0]["finishReason"] | "empty";
            askFail(String("No answer (") + why + ")");
        } else if (deserializeJson(reply, text) || !reply["answer"].is<const char *>()) {
            askQuestion = "";
            askAnswer = text; // not the JSON asked for, but still worth showing
            askState = AskState::Answer;
        } else {
            askQuestion = (const char *)(reply["question"] | "");
            askAnswer = (const char *)reply["answer"];
            askState = AskState::Answer;
        }
    }
    vTaskDelete(nullptr);
}

// Wrapped lines of the answer screen, each with its colour.
struct AskLine {
    String text;
    uint16_t color;
};
std::vector<AskLine> askLines;
int askScroll = 0;
const int ASK_TOP = 28, ASK_LINE_H = 18, ASK_ROWS = (320 - ASK_TOP) / ASK_LINE_H, ASK_TEXT_W = 226;

void askWrap(const String &text, uint16_t color) {
    int start = 0;
    while (start <= (int)text.length()) {
        int nl = text.indexOf('\n', start);
        String para = text.substring(start, nl < 0 ? text.length() : nl);
        String cur;
        int i = 0;
        while (i < (int)para.length()) {
            int sp = para.indexOf(' ', i);
            String word = para.substring(i, sp < 0 ? para.length() : sp);
            String next = cur.length() ? cur + " " + word : word;
            if (cur.length() && u8f.getUTF8Width(next.c_str()) > ASK_TEXT_W) {
                askLines.push_back({cur, color});
                cur = word;
            } else {
                cur = next;
            }
            i = sp < 0 ? para.length() : sp + 1;
        }
        askLines.push_back({cur, color});
        if (nl < 0) break;
        start = nl + 1;
    }
}

void askDrawText() {
    for (int r = 0; r < ASK_ROWS; r++) {
        int y = ASK_TOP + r * ASK_LINE_H;
        tft.fillRect(0, y, 234, ASK_LINE_H, ST77XX_BLACK);
        int i = askScroll + r;
        if (i >= (int)askLines.size()) continue;
        u8f.setForegroundColor(askLines[i].color);
        u8f.setCursor(6, y + 14);
        u8f.print(askLines[i].text);
    }
    int total = askLines.size();
    tft.fillRect(236, ASK_TOP, 4, ASK_ROWS * ASK_LINE_H, ST77XX_BLACK);
    if (total > ASK_ROWS) { // scrollbar
        int h = ASK_ROWS * ASK_LINE_H, thumb = max(12, h * ASK_ROWS / total);
        int y = ASK_TOP + (h - thumb) * askScroll / (total - ASK_ROWS);
        tft.fillRect(236, y, 3, thumb, tft.color565(120, 120, 120));
    }
}

void askDraw() {
    header("Ask AI");
    switch (askState) {
        case AskState::Idle:
            tft.fillRoundRect(100, 60, 40, 64, 20, tft.color565(255, 90, 160));
            tft.drawCircleHelper(120, 100, 32, 0x4 | 0x8, tft.color565(255, 90, 160));
            tft.drawCircleHelper(120, 100, 33, 0x4 | 0x8, tft.color565(255, 90, 160));
            tft.fillRect(118, 133, 5, 20, tft.color565(255, 90, 160));
            tft.fillRect(100, 152, 41, 4, tft.color565(255, 90, 160));
            if (askKey().isEmpty()) line(200, "No API key", ST77XX_RED, 2);
            else line(200, "Ready", ST77XX_WHITE, 2);
            break;
        case AskState::Recording:
            tft.drawCircle(120, 90, 45, ST77XX_WHITE);
            line(260, "Listening", ST77XX_RED, 2);
            break;
        case AskState::Thinking:
            line(140, "Thinking", ST77XX_YELLOW, 3);
            break;
        case AskState::Answer:
            askLines.clear();
            askScroll = 0;
            if (askQuestion.length()) {
                askWrap(askQuestion, tft.color565(120, 200, 255));
                askLines.push_back({"", ST77XX_WHITE});
            }
            askWrap(askAnswer, ST77XX_WHITE);
            askDrawText();
            break;
        case AskState::Failed:
            line(40, "Error", ST77XX_RED, 2);
            askLines.clear();
            askWrap(askError, ST77XX_WHITE);
            for (int i = 0; i < (int)askLines.size() && i < 13; i++) {
                u8f.setForegroundColor(ST77XX_WHITE);
                u8f.setCursor(6, 80 + i * ASK_LINE_H);
                u8f.print(askLines[i].text);
            }
            break;
    }
}

AskState askShown = AskState::Idle; // what askDraw last drew, so the tick sees task updates
void askEnter() {
    if (askState == AskState::Recording && !recording) askState = AskState::Idle; // left mid-question
    askShown = askState;
    askDraw();
}

void askSend() {
    recStop();
    if (recSamples < MIC_RATE / 2) {
        askFail("Too short");
    } else {
        askError = "";
        askState = AskState::Thinking;
        askSentMs = millis();
        xTaskCreatePinnedToCore(askTask, "ask", 12288, nullptr, 1, nullptr, 1);
    }
}

// A press on a long answer pages down first (one line of overlap, so reading stays continuous);
// only on the last page does it start the next question. This is how a board with just BOOT and
// the back button reads a long answer.
void askPress() {
    if (askState == AskState::Thinking) return;
    int maxScroll = max(0, (int)askLines.size() - ASK_ROWS);
    if (askState == AskState::Answer && askScroll < maxScroll) {
        askScroll = min(askScroll + ASK_ROWS - 1, maxScroll);
        askDrawText();
        return;
    }
    if (askState == AskState::Recording) {
        askSend();
    } else if (askKey().isEmpty()) {
        askFail("No API key. Add it on the web page.");
    } else if (recStart(ASK_PATH)) {
        askState = AskState::Recording;
    } else {
        askFail("Cannot record");
    }
    askShown = askState;
    askDraw();
}

void askTick() {
    if (askState != askShown) { // the request task finished
        askShown = askState;
        askDraw();
        return;
    }
    static unsigned long last = 0;
    if (millis() - last < 200) return;
    last = millis();
    if (askState == AskState::Recording) {
        uint32_t sec = (millis() - recStartMs) / 1000;
        if (sec >= ASK_MAX_S) {
            askSend();
            askShown = askState;
            askDraw();
            return;
        }
        bool on = (millis() - recStartMs) % 1000 < 600;
        tft.fillCircle(120, 90, 38, on ? ST77XX_RED : tft.color565(60, 0, 0));
        tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        tft.setTextSize(5);
        tft.setCursor(45, 160);
        tft.print(mmss(sec));
        float db = micDb;
        int w = micScale(db, 224);
        tft.fillRect(8, 222, w, 14, micColor(db));
        tft.fillRect(8 + w, 222, 224 - w, 14, tft.color565(40, 40, 40));
    } else if (askState == AskState::Thinking) {
        int dots = (millis() - askSentMs) / 500 % 4;
        line(140, String("Thinking") + String("...").substring(0, dots) + "   ", ST77XX_YELLOW, 3);
        line(180, String((millis() - askSentMs) / 1000) + " s   ", tft.color565(150, 150, 150), 2);
    }
}

void askRotate(int dir) {
    if (askState != AskState::Answer) return;
    int maxScroll = max(0, (int)askLines.size() - ASK_ROWS);
    int next = constrain(askScroll + dir, 0, maxScroll);
    if (next == askScroll) return;
    askScroll = next;
    askDrawText();
}

// ---------- timer ----------
// The countdown lives outside the app screen: it keeps running from the home clock or any
// other app, and when it ends the timer screen comes back with the LED flashing.
enum class TimerState { Set, Running, Paused, Done };
TimerState timerState = TimerState::Set;
int timerMinutes = 25, timerSeconds = 0;
int timerField = 0; // while setting: 0 = minutes, 1 = seconds
unsigned long timerEndMs = 0, timerLeftMs = 0, timerDoneMs = 0;

long timerSetMs() { return (timerMinutes * 60L + timerSeconds) * 1000L; }

long timerRemainingMs() {
    switch (timerState) {
        case TimerState::Running: return (long)(timerEndMs - millis()) > 0 ? timerEndMs - millis() : 0;
        case TimerState::Paused: return timerLeftMs;
        case TimerState::Set: return timerSetMs();
        default: return 0;
    }
}

String timerText() {
    long s = (timerRemainingMs() + 999) / 1000;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02ld:%02ld", s / 60, s % 60);
    return buf;
}

// Digits at text size 6 are 36 px wide: minutes at x=30, the colon at 102, seconds at 138.
void timerDraw() {
    bool setting = timerState == TimerState::Set;
    const char *label = setting ? (timerField == 0 ? "Set minutes" : "Set seconds")
                      : timerState == TimerState::Running ? "Running"
                      : timerState == TimerState::Paused ? "Paused" : "Time's up!";
    uint16_t color = timerState == TimerState::Done ? ST77XX_RED
                   : timerState == TimerState::Running ? ST77XX_GREEN : ST77XX_WHITE;
    String t = timerText();
    // While setting, the field being edited is yellow and underlined.
    uint16_t minColor = setting && timerField == 0 ? ST77XX_YELLOW : color;
    uint16_t secColor = setting && timerField == 1 ? ST77XX_YELLOW : color;
    tft.setTextSize(6);
    tft.setTextColor(minColor, ST77XX_BLACK);
    tft.setCursor(30, 110);
    tft.print(t.substring(0, 2));
    tft.setTextColor(color, ST77XX_BLACK);
    tft.print(":");
    tft.setTextColor(secColor, ST77XX_BLACK);
    tft.print(t.substring(3, 5));
    tft.fillRect(30, 162, 70, 4, setting && timerField == 0 ? ST77XX_YELLOW : ST77XX_BLACK);
    tft.fillRect(138, 162, 70, 4, setting && timerField == 1 ? ST77XX_YELLOW : ST77XX_BLACK);
    String l = label;
    while (l.length() < 12) l += ' ';
    line(190, l, color, 2);
}

void timerEnter() {
    header("Timer");
    timerDraw();
}

void timerTick() { // redraw once per second, only the digits and the label
    static long lastSecond = -1;
    static TimerState lastState = TimerState::Set;
    long s = (timerRemainingMs() + 999) / 1000;
    if (s == lastSecond && timerState == lastState) return;
    lastSecond = s;
    lastState = timerState;
    timerDraw();
}

void timerPress() {
    switch (timerState) {
        case TimerState::Set:
            if (timerField == 0) { // minutes done, now the seconds
                timerField = 1;
                break;
            }
            if (timerSetMs() == 0) break; // nothing to count down
            timerField = 0;
            timerEndMs = millis() + timerSetMs();
            timerState = TimerState::Running;
            break;
        case TimerState::Running:
            timerLeftMs = timerRemainingMs();
            timerState = TimerState::Paused;
            break;
        case TimerState::Paused:
            timerEndMs = millis() + timerLeftMs;
            timerState = TimerState::Running;
            break;
        case TimerState::Done:
            timerState = TimerState::Set;
            timerField = 0;
            setLed(0, 0, 0);
            break;
    }
    timerDraw();
}

void timerReset() { // stop whatever is running and go back to setting the time
    timerState = TimerState::Set;
    timerField = 0;
    setLed(0, 0, 0);
    timerDraw();
}

void timerRotate(int dir) {
    if (timerState == TimerState::Paused) { // turning a paused timer re-sets it
        timerState = TimerState::Set;
        timerField = 0;
    }
    if (timerState != TimerState::Set) return;
    if (timerField == 0) timerMinutes = constrain(timerMinutes + dir, 0, 99);
    else timerSeconds = (timerSeconds + dir + 60) % 60; // seconds wrap around
    timerDraw();
}

// ---------- weather ----------
// Open-Meteo needs no account or key. Location: Hanoi.
const char *WEATHER_URL =
    "https://api.open-meteo.com/v1/forecast?latitude=21.03&longitude=105.85"
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
    "&hourly=temperature_2m,weather_code&forecast_hours=5&timezone=Asia%2FBangkok";
const char *WEATHER_CITY = "Ha Noi";

struct Weather {
    bool ok = false;
    float temp = 0, wind = 0;
    int humidity = 0, code = 0;
    int hourH[4], hourTemp[4], hourCode[4];
    unsigned long fetchedMs = 0;
    String error;
} weather;

enum WeatherKind { WX_SUN, WX_PARTLY, WX_CLOUD, WX_RAIN, WX_STORM };

WeatherKind weatherKind(int code) { // WMO weather codes
    if (code == 0) return WX_SUN;
    if (code <= 2) return WX_PARTLY;
    if (code <= 48) return WX_CLOUD;
    if (code >= 95) return WX_STORM;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return WX_RAIN;
    return WX_CLOUD;
}

const char *weatherText(int code) {
    switch (weatherKind(code)) {
        case WX_SUN: return "Clear";
        case WX_PARTLY: return "Partly cloudy";
        case WX_RAIN: return code >= 80 ? "Showers" : code <= 57 ? "Drizzle" : "Rain";
        case WX_STORM: return "Thunderstorm";
        default: return code >= 45 && code <= 48 ? "Fog" : "Cloudy";
    }
}

void drawCloud(int cx, int cy, int s, uint16_t c) { // s = scale (1 small, 2 large)
    tft.fillCircle(cx - 8 * s, cy + 2 * s, 6 * s, c);
    tft.fillCircle(cx, cy - 3 * s, 8 * s, c);
    tft.fillCircle(cx + 8 * s, cy + 2 * s, 6 * s, c);
    tft.fillRoundRect(cx - 8 * s, cy + 2 * s, 16 * s, 6 * s, 2, c);
}

void drawSun(int cx, int cy, int s) {
    for (int a = 0; a < 360; a += 45) {
        float r = a * DEG_TO_RAD;
        tft.drawLine(cx + cos(r) * 9 * s, cy + sin(r) * 9 * s, cx + cos(r) * 13 * s, cy + sin(r) * 13 * s, ST77XX_YELLOW);
    }
    tft.fillCircle(cx, cy, 7 * s, ST77XX_YELLOW);
}

void drawWeatherIcon(WeatherKind k, int cx, int cy, int s) {
    uint16_t grey = tft.color565(180, 180, 190);
    switch (k) {
        case WX_SUN: drawSun(cx, cy, s); break;
        case WX_PARTLY:
            drawSun(cx - 5 * s, cy - 5 * s, s);
            drawCloud(cx + 3 * s, cy + 3 * s, s, grey);
            break;
        case WX_CLOUD: drawCloud(cx, cy, s, grey); break;
        case WX_RAIN:
            drawCloud(cx, cy - 3 * s, s, grey);
            for (int i = -1; i <= 1; i++) tft.drawLine(cx + i * 7 * s, cy + 7 * s, cx + i * 7 * s - 3 * s, cy + 13 * s, ST77XX_CYAN);
            break;
        case WX_STORM:
            drawCloud(cx, cy - 3 * s, s, tft.color565(120, 120, 130));
            tft.fillTriangle(cx + 1 * s, cy + 3 * s, cx - 4 * s, cy + 10 * s, cx + 1 * s, cy + 9 * s, ST77XX_YELLOW);
            tft.fillTriangle(cx - 1 * s, cy + 9 * s, cx + 4 * s, cy + 9 * s, cx - 2 * s, cy + 16 * s, ST77XX_YELLOW);
            break;
    }
}

bool weatherFetch() {
    WiFiClientSecure secure;
    secure.setInsecure(); // public weather data; no root CA bundle on the board
    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(secure, WEATHER_URL)) {
        weather.error = "No connection";
        return false;
    }
    int status = http.GET();
    if (status != 200) {
        weather.error = "HTTP " + String(status);
        http.end();
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();
    if (err) {
        weather.error = "Bad data";
        return false;
    }
    weather.temp = doc["current"]["temperature_2m"] | 0.0f;
    weather.humidity = doc["current"]["relative_humidity_2m"] | 0;
    weather.code = doc["current"]["weather_code"] | 0;
    weather.wind = doc["current"]["wind_speed_10m"] | 0.0f;
    for (int i = 0; i < 4; i++) { // entry 0 is the current hour; show the next four
        const char *t = doc["hourly"]["time"][i + 1] | "0000-00-00T00:00";
        weather.hourH[i] = atoi(t + 11);
        weather.hourTemp[i] = lround(doc["hourly"]["temperature_2m"][i + 1] | 0.0f);
        weather.hourCode[i] = doc["hourly"]["weather_code"][i + 1] | 0;
    }
    weather.ok = true;
    weather.fetchedMs = millis();
    weather.error = "";
    return true;
}

void weatherDraw() {
    header("Weather");
    if (!weather.ok) {
        line(60, "Unavailable", ST77XX_RED, 2);
        line(90, weather.error, ST77XX_WHITE, 1);
        return;
    }
    line(34, WEATHER_CITY, ST77XX_CYAN, 2);
    drawWeatherIcon(weatherKind(weather.code), 62, 100, 2);
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.setTextSize(5);
    tft.setCursor(124, 80);
    tft.print(lround(weather.temp));
    int degX = tft.getCursorX() + 6;
    tft.drawCircle(degX, 84, 5, ST77XX_WHITE); // the built-in font has no degree sign
    tft.drawCircle(degX, 84, 4, ST77XX_WHITE);
    line(150, weatherText(weather.code), ST77XX_WHITE, 2);
    line(178, "Humidity " + String(weather.humidity) + "%", ST77XX_WHITE, 1);
    line(192, "Wind " + String(lround(weather.wind)) + " km/h", ST77XX_WHITE, 1);
    tft.drawFastHLine(8, 214, W - 16, tft.color565(60, 60, 70));
    for (int i = 0; i < 4; i++) {
        int cx = 30 + i * 60;
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        tft.setCursor(cx - 9, 224);
        tft.printf("%02dh", weather.hourH[i]);
        drawWeatherIcon(weatherKind(weather.hourCode[i]), cx, 256, 1);
        tft.setTextSize(2);
        tft.setCursor(cx - 12, 284);
        tft.print(weather.hourTemp[i]);
        tft.drawCircle(tft.getCursorX() + 3, 286, 2, ST77XX_WHITE);
    }
}

void weatherEnter() {
    if (!weather.ok || millis() - weather.fetchedMs > 10 * 60000UL) { // refresh at most every 10 minutes
        header("Weather");
        line(60, "Loading...", ST77XX_WHITE, 2);
        weatherFetch();
    }
    weatherDraw();
}

void weatherPress() { // a press forces a refresh
    header("Weather");
    line(60, "Loading...", ST77XX_WHITE, 2);
    weatherFetch();
    weatherDraw();
}

// ---------- navigate ----------
// The destination comes from the phone (an iOS Shortcut shared from Apple/Google Maps) and
// the position from the phone's GPS (OwnTracks, or a Shortcut posting its location). The
// board shows an arrow to the destination, the distance, speed and ETA.
struct Nav {
    bool hasDest = false, hasFix = false;
    double destLat = 0, destLon = 0, lat = 0, lon = 0;
    String destName;
    float speedKmh = -1, course = -1; // course: direction of travel, -1 when unknown
    unsigned long fixMs = 0;
} nav;

// What reached /nav/pos, so a phone that isn't delivering can be diagnosed over the network.
uint32_t navPosHits = 0;
unsigned long navPosLastMs = 0;
String navPosLastFrom, navPosLastBody;

double distanceM(double lat1, double lon1, double lat2, double lon2) { // haversine
    const double R = 6371000.0;
    double dLat = (lat2 - lat1) * DEG_TO_RAD, dLon = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) * sin(dLon / 2) * sin(dLon / 2);
    return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) { // 0 = north, clockwise
    double p1 = lat1 * DEG_TO_RAD, p2 = lat2 * DEG_TO_RAD, dL = (lon2 - lon1) * DEG_TO_RAD;
    double b = atan2(sin(dL) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dL)) * RAD_TO_DEG;
    return fmod(b + 360, 360);
}

void navLoadDest() {
    prefs.begin("nav", true);
    nav.hasDest = prefs.getBool("has", false);
    nav.destLat = prefs.getDouble("lat", 0);
    nav.destLon = prefs.getDouble("lon", 0);
    nav.destName = prefs.getString("name", "");
    prefs.end();
}

void navSaveDest() {
    prefs.begin("nav", false);
    prefs.putBool("has", nav.hasDest);
    prefs.putDouble("lat", nav.destLat);
    prefs.putDouble("lon", nav.destLon);
    prefs.putString("name", nav.destName);
    prefs.end();
}

// New GPS fix. The phone may send speed and course (OwnTracks does); otherwise both are worked
// out from the previous fix, once the phone has moved far enough for GPS noise not to dominate.
void navUpdateFix(double lat, double lon, float speedKmh, float course) {
    static double anchorLat = 0, anchorLon = 0;
    static unsigned long anchorMs = 0;
    unsigned long now = millis();
    if (anchorMs == 0) {
        anchorLat = lat;
        anchorLon = lon;
        anchorMs = now;
    }
    double moved = distanceM(anchorLat, anchorLon, lat, lon);
    if (moved >= 8) {
        if (course < 0) course = bearingDeg(anchorLat, anchorLon, lat, lon);
        // A GPS jump or two fixes a moment apart gives absurd speeds; keep only plausible ones.
        float computed = now - anchorMs >= 1000 ? moved / ((now - anchorMs) / 1000.0) * 3.6 : -1;
        if (speedKmh < 0 && computed >= 0 && computed <= 200) speedKmh = computed;
        anchorLat = lat;
        anchorLon = lon;
        anchorMs = now;
    }
    nav.lat = lat;
    nav.lon = lon;
    if (speedKmh >= 0) nav.speedKmh = speedKmh;
    if (course >= 0) nav.course = course;
    nav.fixMs = now;
    nav.hasFix = true;
}

// Reads "lat, lon" out of free text or a Google Maps URL. Place links carry the pin as
// "!3d<lat>!4d<lon>" (more exact than the "@lat,lon" map centre), so that is tried first.
bool parseCoords(const String &text, double &lat, double &lon) {
    auto grab = [&](int at, double &out) { // number starting at text[at]
        int end = at;
        while (end < (int)text.length() && (isdigit(text[end]) || text[end] == '.' || text[end] == '-')) end++;
        if (end == at) return false;
        out = text.substring(at, end).toDouble();
        return true;
    };
    int d3 = text.indexOf("!3d"), d4 = text.indexOf("!4d");
    if (d3 >= 0 && d4 > d3 && grab(d3 + 3, lat) && grab(d4 + 3, lon)) return true;
    int atSign = text.indexOf('@');
    if (atSign >= 0) {
        int comma = text.indexOf(',', atSign);
        if (comma > 0 && grab(atSign + 1, lat) && grab(comma + 1, lon)) return true;
    }
    // Bare coordinates, e.g. copied from a dropped pin: "21.0285, 105.8522". Look for two
    // decimal numbers separated only by commas, spaces or '+'.
    int n = text.length();
    auto numberEnd = [&](int from) {
        int e = from + 1;
        while (e < n && (isdigit(text[e]) || text[e] == '.')) e++;
        return e;
    };
    for (int i = 0; i < n;) {
        bool starts = isdigit(text[i]) || (text[i] == '-' && i + 1 < n && isdigit(text[i + 1]));
        if (!starts) {
            i++;
            continue;
        }
        int e1 = numberEnd(i), j = e1;
        while (j < n && (text[j] == ',' || text[j] == ' ' || text[j] == '+')) j++;
        if (j > e1 && j < n && (isdigit(text[j]) || text[j] == '-')) {
            String a = text.substring(i, e1), b = text.substring(j, numberEnd(j));
            if (a.indexOf('.') > 0 && b.indexOf('.') > 0 && fabs(a.toDouble()) <= 90 && fabs(b.toDouble()) <= 180) {
                lat = a.toDouble();
                lon = b.toDouble();
                return true;
            }
        }
        i = e1;
    }
    return false;
}

String urlDecode(const String &s) {
    String out;
    for (unsigned i = 0; i < s.length(); i++) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.length()) {
            out += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16);
            i += 2;
        } else out += s[i];
    }
    return out;
}

String nameFromMapsUrl(const String &url) { // ".../maps/place/Ho+Guom/@..." -> "Ho Guom"
    int at = url.indexOf("/place/");
    if (at < 0) return "";
    int start = at + 7, end = url.indexOf('/', start);
    return urlDecode(url.substring(start, end < 0 ? url.length() : end));
}

// "Copy link" in Google Maps gives a short maps.app.goo.gl link; the coordinates are in the
// full URL it redirects to, so follow the redirects by hand and return the final URL.
String resolveMapsLink(String url) {
    for (int hop = 0; hop < 4 && url.startsWith("http") && !url.startsWith("https://www.google.com/maps"); hop++) {
        WiFiClientSecure secure;
        secure.setInsecure();
        HTTPClient http;
        http.setTimeout(8000);
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        if (!http.begin(secure, url)) break;
        http.setUserAgent("Mozilla/5.0");
        const char *keep[] = {"Location"};
        http.collectHeaders(keep, 1);
        int status = http.GET();
        String next = http.header("Location");
        http.end();
        Serial.printf("Maps link hop %d: HTTP %d -> %s\n", hop, status, next.substring(0, 120).c_str());
        if (next.isEmpty()) break;
        url = next;
    }
    return url;
}

// Arrow centred on (cx, cy), pointing at angleDeg (0 = up, clockwise).
void drawArrow(int cx, int cy, int r, float angleDeg, uint16_t c) {
    float a = angleDeg * DEG_TO_RAD;
    auto pt = [&](float along, float across, int &x, int &y) {
        x = cx + along * sin(a) + across * cos(a);
        y = cy - along * cos(a) + across * sin(a);
    };
    int x1, y1, x2, y2, x3, y3, x4, y4;
    pt(r, 0, x1, y1);                // tip
    pt(-r * 0.6f, -r * 0.55f, x2, y2); // left tail
    pt(-r * 0.25f, 0, x3, y3);        // notch
    pt(-r * 0.6f, r * 0.55f, x4, y4);  // right tail
    tft.fillTriangle(x1, y1, x2, y2, x3, y3, c);
    tft.fillTriangle(x1, y1, x3, y3, x4, y4, c);
}

String formatDistance(double m) {
    if (m < 1000) return String((int)lround(m)) + " m";
    return String(m / 1000.0, m < 10000 ? 1 : 0) + " km";
}

const int NAV_CX = 120, NAV_CY = 130, NAV_R = 58;

void navDraw() {
    if (!nav.hasDest) {
        line(60, "No destination", ST77XX_WHITE, 2);
        return;
    }
    String name = nav.destName.length() ? asciiFold(nav.destName) : String("Destination");
    if (name.length() > 19) name = name.substring(0, 19);
    while (name.length() < 19) name += ' ';
    line(32, name, ST77XX_CYAN, 2);

    unsigned long ageS = nav.hasFix ? (millis() - nav.fixMs) / 1000 : 0;
    tft.fillCircle(NAV_CX, NAV_CY, NAV_R + 4, ST77XX_BLACK);
    tft.drawCircle(NAV_CX, NAV_CY, NAV_R + 4, tft.color565(60, 60, 70));
    if (!nav.hasFix) {
        line(200, "Waiting for GPS     ", ST77XX_YELLOW, 2);
        return;
    }
    double dist = distanceM(nav.lat, nav.lon, nav.destLat, nav.destLon);
    double toDest = bearingDeg(nav.lat, nav.lon, nav.destLat, nav.destLon);
    bool arrived = dist < 30;
    // With a known direction of travel the arrow is relative to where you are heading;
    // standing still there is none, so the arrow is drawn north-up and says so.
    bool headingUp = nav.course >= 0 && nav.speedKmh >= 2;
    float angle = headingUp ? toDest - nav.course : toDest;
    drawArrow(NAV_CX, NAV_CY, NAV_R - 6, angle, arrived ? ST77XX_GREEN : ST77XX_WHITE);
    if (!headingUp) {
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
        tft.setCursor(NAV_CX - 3, NAV_CY - NAV_R - 2);
        tft.print("N");
    }

    String d = arrived ? String("Arrived") : formatDistance(dist);
    while (d.length() < 9) d += ' ';
    tft.setTextSize(3);
    tft.setTextColor(arrived ? ST77XX_GREEN : ST77XX_WHITE, ST77XX_BLACK);
    tft.setCursor(8, 200);
    tft.print(d);

    String speed = nav.speedKmh >= 0 ? String((int)lround(nav.speedKmh)) + " km/h" : String("-- km/h");
    String eta = !arrived && nav.speedKmh >= 2 ? "  ~" + String((int)ceil(dist / 1000.0 / nav.speedKmh * 60)) + " min"
                                              : String("");
    String info = speed + eta;
    while (info.length() < 19) info += ' ';
    line(240, info, ST77XX_WHITE, 2);

    String gps = ageS <= 15 ? "GPS " + String(ageS) + "s ago" : "GPS lost " + String(ageS) + "s";
    while (gps.length() < 19) gps += ' ';
    line(270, gps, ageS <= 15 ? tft.color565(120, 200, 120) : ST77XX_RED, 2);
}

void navEnter() {
    header("Navigate");
    navDraw();
}

void navTick() { // redraw once a second, and at once when a new fix or destination arrives
    static unsigned long lastDrawMs = 0, lastFixMs = 0;
    if (millis() - lastDrawMs < 1000 && nav.fixMs == lastFixMs) return;
    lastDrawMs = millis();
    lastFixMs = nav.fixMs;
    navDraw();
}

enum Icon { ICON_TIMER, ICON_WEATHER, ICON_NAV, ICON_DOC, ICON_PHONE, ICON_WIFI, ICON_RGB, ICON_HOTSPOT, ICON_MIC, ICON_REC, ICON_ASK, ICON_INFO };

struct App {
    const char *name;
    Icon icon;
    void (*enter)();
    void (*tick)();
    void (*press)();
};
// The clock is not listed: it is the home screen, shown at power-on and below the menu.
const App APPS[] = {
    {"Timer", ICON_TIMER, timerEnter, timerTick, timerPress},
    {"Weather", ICON_WEATHER, weatherEnter, nullptr, weatherPress},
    {"Navigate", ICON_NAV, navEnter, navTick, nullptr},
    {"Documents", ICON_DOC, readerEnter, nullptr, readerPress},
    {"From phone", ICON_PHONE, receiveEnter, nullptr, nullptr},
    {"WiFi scan", ICON_WIFI, radarEnter, radarTick, radarPress},
    {"RGB light", ICON_RGB, ledEnter, nullptr, ledPress},
    {"Hotspot", ICON_HOTSPOT, hotspotEnter, hotspotTick, hotspotPress},
    {"Sound meter", ICON_MIC, micEnter, micTick, micPress},
    {"Recorder", ICON_REC, recEnter, recTick, recPress},
    {"Ask AI", ICON_ASK, askEnter, askTick, askPress},
    {"Info", ICON_INFO, infoEnter, infoTick, nullptr},
};
const int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);
const int APP_TIMER = 0;
const int APP_NAV = 2;
const int APP_READER = 3;
const int APP_RECEIVE = 4;
const int APP_LED = 6;
const int APP_HOTSPOT = 7;
const int APP_REC = 9;
const int APP_ASK = 10;
const int HOME = -2; // appIndex value for the home clock (-1 is the menu)

// Drawing text with a background color overwrites the old glyphs in place, so a row can be
// refreshed without clearing it first - clearing is what makes the menu flicker.
// Menu as a vertical list (it is scrolled with a knob, so one axis): each row is a card with an
// icon drawn from shapes (no image assets) and the name. 7 rows fit; the list scrolls past that.
const int ROW_X = 6, ROW_W = 224, ROW_H = 36, ROW_GAP = 5, ROW_TOP = 30, ROWS_VISIBLE = 7;
int menuTop = 0; // first visible row

void drawIcon(Icon icon, int cx, int cy) {
    switch (icon) {
        case ICON_TIMER: { // stopwatch: dial, crown button and a hand
            uint16_t c = tft.color565(255, 120, 80);
            tft.drawCircle(cx, cy + 2, 11, c);
            tft.drawCircle(cx, cy + 2, 10, c);
            tft.fillRect(cx - 3, cy - 13, 7, 3, c);
            tft.drawLine(cx, cy + 2, cx + 5, cy - 4, c);
            tft.fillCircle(cx, cy + 2, 2, c);
            break;
        }
        case ICON_WEATHER: {
            drawSun(cx - 5, cy - 5, 1);
            drawCloud(cx + 4, cy + 4, 1, tft.color565(200, 200, 210));
            break;
        }
        case ICON_NAV: // navigate: a navigation arrow tilted like a map pointer
            drawArrow(cx, cy, 13, 45, tft.color565(80, 200, 255));
            break;
        case ICON_DOC: { // documents: a page with text lines
            uint16_t c = ST77XX_WHITE;
            tft.drawRect(cx - 10, cy - 13, 20, 26, c);
            for (int i = 0; i < 4; i++) tft.drawFastHLine(cx - 6, cy - 7 + i * 5, 12, c);
            break;
        }
        case ICON_PHONE: { // from phone: a phone with an arrow coming in
            uint16_t c = ST77XX_CYAN;
            tft.drawRoundRect(cx - 9, cy - 14, 18, 28, 3, c);
            tft.drawFastHLine(cx - 3, cy + 10, 6, c);
            tft.drawFastVLine(cx, cy - 9, 9, c);
            tft.fillTriangle(cx, cy + 5, cx - 5, cy - 1, cx + 5, cy - 1, c);
            break;
        }
        case ICON_WIFI: { // wifi scan: signal arcs
            uint16_t c = ST77XX_GREEN;
            for (int r = 5; r <= 15; r += 5) {
                tft.drawCircleHelper(cx, cy + 9, r, 0x1 | 0x2, c); // top half of a circle
                tft.drawCircleHelper(cx, cy + 9, r + 1, 0x1 | 0x2, c);
            }
            tft.fillCircle(cx, cy + 9, 2, c);
            break;
        }
        case ICON_RGB: // rgb light: three overlapping colour dots
            tft.fillCircle(cx - 6, cy - 4, 7, ST77XX_RED);
            tft.fillCircle(cx + 6, cy - 4, 7, ST77XX_GREEN);
            tft.fillCircle(cx, cy + 6, 7, ST77XX_BLUE);
            break;
        case ICON_HOTSPOT: { // hotspot: a mast broadcasting to both sides
            uint16_t c = tft.color565(255, 140, 0);
            tft.fillCircle(cx, cy - 5, 3, c);
            tft.drawLine(cx, cy - 3, cx - 6, cy + 13, c);
            tft.drawLine(cx, cy - 3, cx + 6, cy + 13, c);
            tft.drawCircleHelper(cx, cy - 5, 8, 0x1 | 0x8, c);  // left half
            tft.drawCircleHelper(cx, cy - 5, 8, 0x2 | 0x4, c);  // right half
            tft.drawCircleHelper(cx, cy - 5, 12, 0x1 | 0x8, c);
            tft.drawCircleHelper(cx, cy - 5, 12, 0x2 | 0x4, c);
            break;
        }
        case ICON_MIC: { // sound meter: a microphone on a stand
            uint16_t c = tft.color565(255, 90, 160);
            tft.fillRoundRect(cx - 5, cy - 14, 10, 17, 5, c);
            tft.drawCircleHelper(cx, cy - 3, 9, 0x4 | 0x8, c); // bottom half: the holder
            tft.drawCircleHelper(cx, cy - 3, 10, 0x4 | 0x8, c);
            tft.fillRect(cx - 1, cy + 7, 3, 6, c);
            tft.fillRect(cx - 6, cy + 12, 13, 2, c);
            break;
        }
        case ICON_REC: // recorder: a record button
            tft.drawCircle(cx, cy, 13, ST77XX_WHITE);
            tft.fillCircle(cx, cy, 9, ST77XX_RED);
            break;
        case ICON_ASK: { // ask AI: a speech bubble with sparkle dots
            uint16_t c = tft.color565(170, 120, 255);
            tft.fillRoundRect(cx - 14, cy - 11, 28, 19, 6, c);
            tft.fillTriangle(cx - 8, cy + 7, cx - 2, cy + 7, cx - 10, cy + 13, c);
            for (int d = -7; d <= 7; d += 7) tft.fillCircle(cx + d, cy - 2, 2, ST77XX_WHITE);
            break;
        }
        case ICON_INFO: { // info: an "i" in a circle
            uint16_t c = tft.color565(120, 180, 255);
            tft.drawCircle(cx, cy, 13, c);
            tft.drawCircle(cx, cy, 12, c);
            tft.fillRect(cx - 1, cy - 8, 3, 3, c);
            tft.fillRect(cx - 1, cy - 3, 3, 11, c);
            break;
        }
    }
}

// One row, painted over its own area, so switching the selection repaints two rows only.
void drawMenuItem(int i) {
    if (i < menuTop || i >= menuTop + ROWS_VISIBLE) return; // scrolled out of view
    int x = ROW_X, y = ROW_TOP + (i - menuTop) * (ROW_H + ROW_GAP);
    bool sel = i == menuSel;
    uint16_t bg = sel ? tft.color565(22, 52, 72) : tft.color565(32, 32, 38);
    tft.fillRoundRect(x, y, ROW_W, ROW_H, 7, bg);
    if (sel) {
        tft.drawRoundRect(x, y, ROW_W, ROW_H, 7, ACCENT);
        tft.drawRoundRect(x + 1, y + 1, ROW_W - 2, ROW_H - 2, 6, ACCENT);
    }
    drawIcon(APPS[i].icon, x + 24, y + ROW_H / 2);
    tft.setTextSize(2);
    tft.setTextColor(sel ? ACCENT : ST77XX_WHITE, bg);
    tft.setCursor(x + 50, y + (ROW_H - 16) / 2);
    tft.print(APPS[i].name);
}

void drawScrollbar() { // thin bar on the right showing where the visible rows sit in the list
    if (APP_COUNT <= ROWS_VISIBLE) return;
    const int top = ROW_TOP, h = ROWS_VISIBLE * (ROW_H + ROW_GAP) - ROW_GAP;
    int thumbH = h * ROWS_VISIBLE / APP_COUNT;
    int thumbY = top + (h - thumbH) * menuTop / (APP_COUNT - ROWS_VISIBLE);
    tft.fillRect(W - 5, top, 3, h, tft.color565(40, 40, 48));
    tft.fillRect(W - 5, thumbY, 3, thumbH, tft.color565(150, 150, 160));
}

void drawMenu() {
    header("Menu");
    for (int i = menuTop; i < menuTop + ROWS_VISIBLE && i < APP_COUNT; i++) drawMenuItem(i);
    drawScrollbar();
}

// Moves the selection; repaints two rows, or the whole list when it has to scroll.
void menuMove(int dir) {
    int previous = menuSel;
    menuSel = (menuSel + dir + APP_COUNT) % APP_COUNT;
    int newTop = menuTop;
    if (menuSel < menuTop) newTop = menuSel;
    if (menuSel >= menuTop + ROWS_VISIBLE) newTop = menuSel - ROWS_VISIBLE + 1;
    if (newTop != menuTop) {
        menuTop = newTop;
        for (int i = menuTop; i < menuTop + ROWS_VISIBLE && i < APP_COUNT; i++) drawMenuItem(i);
        drawScrollbar();
    } else {
        drawMenuItem(previous);
        drawMenuItem(menuSel);
    }
}

void openApp(int i) {
    appIndex = i;
    Serial.printf("Open app: %s\n", APPS[i].name);
    if (APPS[i].enter) APPS[i].enter();
}

void backToMenu() {
    appIndex = -1;
    Serial.println("Back to menu");
    drawMenu();
}

void goHome() {
    appIndex = HOME;
    clockEnter();
    homeTimerShownSec = -2; // the screen was cleared, so the timer line must be drawn again
}

// One step back, shared by the back button and a long BOOT press: a screen inside an app
// (an open document, an active timer) is left first, then the app, then the menu.
void goBack() {
    if (appIndex == APP_READER && !readerInList) readerEnter();
    else if (appIndex == APP_TIMER && timerState != TimerState::Set) timerReset();
    else if (appIndex >= 0) backToMenu();
    else if (appIndex == -1) goHome();
}

// ---------- web ----------

void resetUploadState() {
    pixelsDrawn = 0;
    bytesReceived = 0;
    hasCarry = false;
    uploadStartMs = millis();
}

uint16_t *clockBgStage = nullptr; // receives an upload, so a failed one never shows half a photo
bool clockBgUploadOk = false;

void handleImageUpload() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        resetUploadState();
        appIndex = APP_RECEIVE; // whatever was on screen, the phone's content takes over
        return;
    }
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
        } else if (n == 1) {
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

// Page number and format come from the upload filename ("m3.bin" mono, "p3.bin" RGB565),
// which the client chooses, so it is parsed strictly rather than used as a path.
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
        if (index < 0 || index >= MAX_PAGES || recvSlot < 0) return;
        pageFile = FFat.open(mono ? monoPagePath(recvSlot, index) : colorPagePath(recvSlot, index), FILE_WRITE);
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

void setupWeb() {
    server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", SCREEN_PAGE_HTML); });
    server.on("/clockbg", HTTP_GET, []() {
        server.send(200, "application/json", String("{\"set\":") + (clockBg ? "true" : "false") + "}");
    });
    server.on("/clockbg", HTTP_POST, []() {
        bool ok = clockBgUploadOk && clockBgStage && bytesReceived == PAGE_BYTES;
        if (ok) {
            File f = FFat.open(CLOCK_BG_PATH, FILE_WRITE);
            ok = f && f.write((uint8_t *)clockBgStage, PAGE_BYTES) == PAGE_BYTES;
            if (f) f.close();
        }
        if (!ok) {
            FFat.remove(CLOCK_BG_PATH); // never leave a half-written photo to load at boot
            return server.send(400, "text/plain; charset=utf-8", "Ảnh sai kích thước hoặc không lưu được");
        }
        std::swap(clockBg, clockBgStage); // the old photo's buffer becomes the next staging area
        goHome();
        server.send(200, "text/plain; charset=utf-8", "Đã đổi hình nền đồng hồ");
    }, []() {
        HTTPUpload &up = server.upload();
        if (up.status == UPLOAD_FILE_START) {
            resetUploadState();
            if (!clockBgStage) clockBgStage = (uint16_t *)ps_malloc(PAGE_BYTES);
            clockBgUploadOk = clockBgStage != nullptr;
        } else if (up.status == UPLOAD_FILE_WRITE && clockBgUploadOk) {
            if (bytesReceived + up.currentSize > PAGE_BYTES) clockBgUploadOk = false;
            else memcpy((uint8_t *)clockBgStage + bytesReceived, up.buf, up.currentSize);
            bytesReceived += up.currentSize;
        }
    });
    server.on("/clockbg/clear", HTTP_POST, []() {
        FFat.remove(CLOCK_BG_PATH);
        free(clockBg);
        clockBg = nullptr;
        goHome();
        server.send(200, "text/plain; charset=utf-8", "Đã bỏ hình nền");
    });
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
    server.on("/doc", HTTP_GET, []() { // the document being read (the web page shows this)
        server.send(200, "application/json", "{\"count\":" + String(pageCount) + ",\"current\":" + String(currentPage) +
            ",\"slot\":" + String(curDoc) + ",\"free\":" + String(FFat.freeBytes()) + "}");
    });
    server.on("/docs", HTTP_GET, []() { // the whole library
        String json = "{\"free\":" + String(FFat.freeBytes()) + ",\"docs\":[";
        bool first = true;
        for (int s = 0; s < MAX_DOCS; s++) {
            if (!docExists(s)) continue;
            if (!first) json += ",";
            first = false;
            json += "{\"slot\":" + String(s) + ",\"name\":\"" + jsonEscape(docName(s)) + "\",\"pages\":" +
                String(docPages(s)) + ",\"page\":" + String(s == curDoc ? currentPage : docSavedPage(s)) +
                ",\"reading\":" + (s == curDoc ? "true" : "false") + "}";
        }
        server.send(200, "application/json; charset=utf-8", json + "],\"max\":" + String(MAX_DOCS) + "}");
    });
    server.on("/doc/open", HTTP_POST, []() {
        int slot = server.arg("slot").toInt();
        if (!openDoc(slot)) return server.send(404, "text/plain; charset=utf-8", "Không có tài liệu này");
        appIndex = APP_READER;
        readerInList = false;
        refreshDocCache();
        readerSelectSlot(slot);
        server.send(200, "text/plain; charset=utf-8", "Đang đọc: " + docName(slot));
    });
    server.on("/doc/delete", HTTP_POST, []() {
        int slot = server.arg("slot").toInt();
        if (!docExists(slot)) return server.send(404, "text/plain; charset=utf-8", "Không có tài liệu này");
        String name = docName(slot);
        docErase(slot);
        if (slot == curDoc) { // fall back to any other document
            curDoc = -1;
            pageCount = 0;
            for (int s = 0; s < MAX_DOCS && curDoc < 0; s++) {
                if (docExists(s)) openDoc(s);
            }
        }
        if (appIndex == APP_READER) readerEnter();
        server.send(200, "text/plain; charset=utf-8", "Đã xóa: " + name);
    });
    server.on("/doc/begin", HTTP_POST, []() {
        int count = server.arg("count").toInt();
        uint32_t perPage = server.arg("mono") == "1" ? MONO_BYTES : PAGE_BYTES;
        if (count < 1 || count > MAX_PAGES) {
            return server.send(400, "text/plain; charset=utf-8", "Tài liệu tối đa " + String(MAX_PAGES) + " trang");
        }
        int slot = -1; // reuse the slot of a document with the same name, else the first free one
        String name = server.arg("name");
        name.trim();
        if (name.isEmpty()) name = "Document";
        if (name.length() > 40) name = name.substring(0, 40);
        for (int s = 0; s < MAX_DOCS && slot < 0; s++) {
            if (docExists(s) && docName(s) == name) slot = s;
        }
        for (int s = 0; s < MAX_DOCS && slot < 0; s++) {
            if (!docExists(s)) slot = s;
        }
        if (slot < 0) {
            return server.send(400, "text/plain; charset=utf-8",
                "Board đã có " + String(MAX_DOCS) + " tài liệu, hãy xóa bớt trước");
        }
        docErase(slot);
        FFat.mkdir(docDir(slot));
        const uint32_t CLUSTER = 4096; // FAT allocates whole clusters, so a page costs a bit more
        uint32_t fits = FFat.freeBytes() / ((perPage + CLUSTER - 1) / CLUSTER * CLUSTER);
        if ((uint32_t)count > fits) {
            return server.send(400, "text/plain; charset=utf-8",
                "Không đủ bộ nhớ: chứa được khoảng " + String(fits) + " trang, tài liệu có " + String(count) +
                " trang. Hãy giảm cỡ chữ để bớt số trang.");
        }
        File f = FFat.open(docNamePath(slot), FILE_WRITE);
        if (f) {
            f.print(name);
            f.close();
        }
        recvSlot = slot;
        server.send(200, "text/plain", "ok");
    });
    server.on("/doc/end", HTTP_POST, []() {
        if (recvSlot < 0) return server.send(400, "text/plain; charset=utf-8", "Chưa bắt đầu gửi tài liệu");
        int slot = recvSlot;
        recvSlot = -1;
        int count = docPages(slot);
        if (count == 0) {
            docErase(slot);
            return server.send(400, "text/plain; charset=utf-8", "Không nhận được trang nào");
        }
        docSavePage(slot, 0);
        openDoc(slot);
        appIndex = APP_READER;
        readerInList = false;
        refreshDocCache();
        readerSelectSlot(slot);
        server.send(200, "text/plain; charset=utf-8", "Đã lưu " + String(count) + " trang");
    });
    server.on("/doc/erase", HTTP_POST, []() { // deletes the document being read
        if (curDoc < 0) return server.send(404, "text/plain; charset=utf-8", "Board chưa có tài liệu");
        String name = docName(curDoc);
        docErase(curDoc);
        curDoc = -1;
        pageCount = 0;
        for (int s = 0; s < MAX_DOCS && curDoc < 0; s++) {
            if (docExists(s)) openDoc(s);
        }
        if (appIndex == APP_READER) readerEnter();
        server.send(200, "text/plain; charset=utf-8", "Đã xóa: " + name);
    });
    server.on("/doc/step", HTTP_POST, []() {
        if (pageCount <= 0) return server.send(404, "text/plain; charset=utf-8", "Board chưa có tài liệu");
        appIndex = APP_READER;
        readerInList = false;
        stepPage(server.arg("d").toInt() < 0 ? -1 : 1);
        server.send(200, "text/plain; charset=utf-8", "Đang hiện trang " + String(currentPage + 1) + "/" + String(pageCount));
    });
    server.on("/clear", HTTP_POST, []() {
        backToMenu();
        server.send(200, "text/plain; charset=utf-8", "Đã xóa màn hình");
    });
    // While the board broadcasts its own WiFi, send the phone's connectivity checks to the
    // page, which is what makes it pop up right after joining.
    server.onNotFound([]() {
        if (!apOn) return server.send(404, "text/plain", "Not found");
        server.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
        server.send(302, "text/plain", "");
    });
    // Recovers a panel that came up blank (e.g. powered from a charger) without a power cycle.
    server.on("/screen/reset", HTTP_POST, []() {
        screenInit();
        if (appIndex == HOME) goHome();
        else if (appIndex < 0) drawMenu();
        else openApp(appIndex);
        server.send(200, "text/plain; charset=utf-8", "Đã khởi tạo lại màn hình");
    });
    // Navigation. Numbers from an iPhone in a Vietnamese locale can arrive as "21,0285", so
    // commas are read as decimal points.
    auto number = [](const String &name) {
        String v = server.arg(name);
        v.replace(",", ".");
        v.trim();
        return v.toDouble();
    };
    server.on("/nav", HTTP_GET, []() {
        double dist = nav.hasDest && nav.hasFix ? distanceM(nav.lat, nav.lon, nav.destLat, nav.destLon) : -1;
        String json = String("{\"hasDest\":") + (nav.hasDest ? "true" : "false") +
            ",\"dest\":\"" + jsonEscape(nav.destName) + "\",\"destLat\":" + String(nav.destLat, 6) +
            ",\"destLon\":" + String(nav.destLon, 6) + ",\"hasFix\":" + (nav.hasFix ? "true" : "false") +
            ",\"lat\":" + String(nav.lat, 6) + ",\"lon\":" + String(nav.lon, 6) +
            ",\"speedKmh\":" + String(nav.speedKmh, 1) + ",\"course\":" + String(nav.course, 0) +
            ",\"fixAgeS\":" + String(nav.hasFix ? (long)((millis() - nav.fixMs) / 1000) : -1L) +
            ",\"distanceM\":" + String(dist, 0) +
            ",\"posHits\":" + String(navPosHits) + ",\"posAgeS\":" + String(navPosHits ? (long)((millis() - navPosLastMs) / 1000) : -1L) +
            ",\"posFrom\":\"" + navPosLastFrom + "\",\"posBody\":\"" + jsonEscape(navPosLastBody) + "\"}";
        server.send(200, "application/json; charset=utf-8", json);
    });
    server.on("/nav/dest", HTTP_POST, [number]() { // from the web page: a Google Maps link or lat/lon
        if (server.hasArg("clear")) {
            nav.hasDest = false;
            navSaveDest();
            if (appIndex == APP_NAV) navEnter();
            return server.send(200, "text/plain", "cleared");
        }
        double lat = number("lat"), lon = number("lon");
        String name = server.arg("name");
        if (server.hasArg("link")) { // pasted from Google Maps: a link, the shared text, or bare coordinates
            String text = server.arg("link");
            text.trim();
            if (text.indexOf("http") < 0) { // a link pasted without its scheme: "maps.app.goo.gl/..."
                int g = text.indexOf("goo.gl");
                if (g < 0) g = text.indexOf("google.");
                if (g >= 0) {
                    int start = g;
                    while (start > 0 && text[start - 1] != ' ' && text[start - 1] != '\n') start--;
                    text = text.substring(0, start) + "https://" + text.substring(start);
                }
            }
            int u = text.indexOf("http");
            String url = u >= 0 ? text.substring(u) : "";
            int cut = url.indexOf(' ');
            if (cut < 0) cut = url.indexOf('\n');
            if (cut > 0) url = url.substring(0, cut);
            if (name.isEmpty() && u > 0) { // Google's shared text puts the place name before the link
                name = text.substring(0, u);
                name.trim();
            }
            if (!parseCoords(url.length() ? url : text, lat, lon)) {
                if (url.isEmpty()) return server.send(400, "text/plain; charset=utf-8", "Không đọc được tọa độ");
                String full = resolveMapsLink(url);
                if (!parseCoords(full, lat, lon)) {
                    return server.send(400, "text/plain; charset=utf-8", "Không đọc được tọa độ từ link này");
                }
                if (name.isEmpty()) name = nameFromMapsUrl(full);
            } else if (name.isEmpty()) {
                name = nameFromMapsUrl(url);
            }
        } else if (!server.hasArg("lat") || !server.hasArg("lon")) {
            return server.send(400, "text/plain", "need lat and lon");
        }
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) {
            return server.send(400, "text/plain", "bad coordinates");
        }
        nav.destLat = lat;
        nav.destLon = lon;
        nav.destName = name.substring(0, 60);
        nav.hasDest = true;
        navSaveDest();
        openApp(APP_NAV); // show the new destination right away
        server.send(200, "text/plain; charset=utf-8", "Destination set: " + nav.destName);
    });
    server.on("/nav/pos", HTTP_POST, [number]() { // OwnTracks JSON, or form lat/lon from a Shortcut
        String body = server.arg("plain");
        navPosHits++;
        navPosLastMs = millis();
        navPosLastFrom = server.client().remoteIP().toString();
        navPosLastBody = body.length() ? body.substring(0, 160) : "form lat=" + server.arg("lat") + " lon=" + server.arg("lon");
        if (body.startsWith("{")) {
            JsonDocument doc;
            if (deserializeJson(doc, body)) return server.send(400, "text/plain", "bad json");
            if (doc["_type"] == "location") { // OwnTracks also sends other message types; ignore those
                navUpdateFix(doc["lat"] | 0.0, doc["lon"] | 0.0, doc["vel"] | -1.0f, doc["cog"] | -1.0f);
            }
            return server.send(200, "application/json", "[]"); // OwnTracks expects a JSON array back
        }
        if (!server.hasArg("lat") || !server.hasArg("lon")) return server.send(400, "text/plain", "need lat and lon");
        navUpdateFix(number("lat"), number("lon"), -1, -1);
        server.send(200, "text/plain", "ok");
    });
    server.on("/buttons", HTTP_GET, []() { // live pin levels (1 = released) and press counters
        String json = String("{\"level\":{\"boot\":") + digitalRead(BUTTON) + ",\"knob\":" + digitalRead(ENC_SW) +
            ",\"back\":" + digitalRead(BACK_BTN) + ",\"encA\":" + digitalRead(ENC_A) + ",\"encB\":" + digitalRead(ENC_B) +
            "},\"taps\":{\"boot\":" + bootTaps + ",\"knob\":" + knobTaps + ",\"back\":" + backTaps +
            ",\"rotations\":" + rotations + "},\"edges\":{\"encA\":" + encEdgesA + ",\"encB\":" + encEdgesB + "}}";
        server.send(200, "application/json", json);
    });
    server.on("/rec", HTTP_GET, []() {
        String json = String("{\"recording\":") + (recording ? "true" : "false") +
            ",\"elapsedS\":" + String(recording ? (millis() - recStartMs) / 1000 : 0) +
            ",\"dropped\":" + String(recDropped) + ",\"freeS\":" + String(FFat.freeBytes() / (MIC_RATE * 2)) + ",\"files\":[";
        std::vector<std::pair<String, size_t>> files;
        File dir = FFat.open(REC_DIR);
        if (dir) {
            for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
                String n = f.name();
                n = n.substring(n.lastIndexOf('/') + 1);
                if (!(recording && n == recName)) files.push_back({n, f.size()});
            }
        }
        std::sort(files.begin(), files.end(), [](const std::pair<String, size_t> &a, const std::pair<String, size_t> &b) { return a.first > b.first; }); // newest first
        for (size_t i = 0; i < files.size(); i++) {
            size_t bytes = files[i].second;
            json += String(i ? "," : "") + "{\"name\":\"" + jsonEscape(files[i].first) + "\",\"bytes\":" + String(bytes) +
                ",\"seconds\":" + String(bytes > 44 ? (bytes - 44) / (MIC_RATE * 2.0) : 0, 1) + "}";
        }
        server.send(200, "application/json", json + "]}");
    });
    server.on("/rec/start", HTTP_POST, []() {
        if (appIndex != APP_REC) openApp(APP_REC);
        bool ok = recStart();
        recDraw();
        server.send(ok ? 200 : 409, "text/plain", ok ? "Đang ghi âm" : "Không bắt đầu được (đang ghi, hết bộ nhớ hoặc không có mic)");
    });
    server.on("/rec/stop", HTTP_POST, []() {
        bool was = recording;
        recStop();
        if (appIndex == APP_REC) recDraw();
        server.send(200, "text/plain", was ? "Đã lưu " + recLastSaved : "Không có bản ghi nào đang chạy");
    });
    server.on("/rec/file", HTTP_GET, []() {
        String n = server.arg("name");
        if (!validRecName(n) || (recording && n == recName)) return server.send(400, "text/plain", "bad name");
        File f = FFat.open(String(REC_DIR) + "/" + n, FILE_READ);
        if (!f) return server.send(404, "text/plain", "not found");
        if (server.hasArg("dl")) server.sendHeader("Content-Disposition", "attachment; filename=\"" + n + "\"");
        server.streamFile(f, "audio/wav");
        f.close();
    });
    server.on("/rec/delete", HTTP_POST, []() {
        String n = server.arg("name");
        if (!validRecName(n) || (recording && n == recName)) return server.send(400, "text/plain", "Tên file không hợp lệ");
        bool ok = FFat.remove(String(REC_DIR) + "/" + n);
        if (ok && n == recLastSaved) recLastSaved = "";
        if (appIndex == APP_REC) recDraw();
        server.send(ok ? 200 : 404, "text/plain", ok ? "Đã xóa " + n : "Không tìm thấy file");
    });
    server.on("/ask", HTTP_GET, []() { // never returns the key itself
        static const char *STATES[] = {"idle", "recording", "thinking", "answer", "failed"};
        AskState st = askState;
        server.send(200, "application/json", String("{\"hasKey\":") + (askKey().length() ? "true" : "false") +
            ",\"model\":\"" + jsonEscape(askModel()) + "\",\"state\":\"" + STATES[(int)st] + "\"" +
            ",\"elapsedS\":" + String(st == AskState::Recording ? (millis() - recStartMs) / 1000 : st == AskState::Thinking ? (millis() - askSentMs) / 1000 : 0) +
            ",\"tookMs\":" + String(askTookMs) + ",\"lines\":" + String(askLines.size()) + ",\"scroll\":" + String(askScroll) +
            ",\"question\":\"" + jsonEscape(askQuestion) + "\",\"answer\":\"" + jsonEscape(askAnswer) +
            "\",\"error\":\"" + jsonEscape(askError) + "\"}");
    });
    server.on("/ask/key", HTTP_POST, []() {
        prefs.begin("gemini", false);
        if (server.hasArg("key")) prefs.putString("key", server.arg("key"));
        if (server.hasArg("model")) prefs.putString("model", server.arg("model"));
        prefs.end();
        if (appIndex == APP_ASK && askState == AskState::Idle) askDraw();
        server.send(200, "text/plain", "Đã lưu");
    });
    server.on("/ask/press", HTTP_POST, []() { // same as the knob: start, or stop and send
        if (appIndex != APP_ASK) openApp(APP_ASK);
        askPress();
        static const char *STATES[] = {"idle", "recording", "thinking", "answer", "failed"};
        server.send(200, "text/plain", STATES[(int)(AskState)askState]);
    });
    server.on("/mic/raw", HTTP_GET, []() {
        micCaptureDone = false;
        micCaptureReq = true;
        for (int i = 0; i < 100 && !micCaptureDone; i++) delay(5);
        if (!micCaptureDone) return server.send(503, "text/plain", "no data");
        String out;
        out.reserve(512 * 12);
        for (int i = 0; i < 512; i++) out += String((uint32_t)micCapture[i], HEX) + (i % 2 ? "\n" : " ");
        server.send(200, "text/plain", out);
    });
    server.on("/mic", HTTP_GET, []() {
        String json = String("{\"ready\":") + (micReady ? "true" : "false") + ",\"db\":" + String(micDb, 1) +
            ",\"windows\":" + String(micWindows);
        const char *names[2] = {"left", "right"};
        for (int c = 0; c < 2; c++) {
            MicSlot m = micSlot[c];
            json += String(",\"") + names[c] + "\":{\"rms\":" + String(m.rms, 0) + ",\"db\":" + String(micToDb(m.rms), 1) +
                ",\"min\":" + String(m.min) + ",\"max\":" + String(m.max) + "}";
        }
        server.send(200, "application/json", json + "}");
    });
    server.on("/battery", HTTP_GET, []() {
        float v = batteryVolts();
        server.send(200, "application/json", String("{\"present\":") + (batteryPresent(v) ? "true" : "false") +
            ",\"volts\":" + String(v, 2) + ",\"percent\":" + String(batteryPresent(v) ? batteryPercent(v) : 0) +
            ",\"raw\":" + String(batteryVoltsRaw(), 2) + ",\"spread\":" + String(batterySpread(), 2) + "}");
    });
    server.on("/ap", HTTP_GET, []() {
        server.send(200, "application/json", String("{\"on\":") + (apOn ? "true" : "false") +
            ",\"clients\":" + String(WiFi.softAPgetStationNum()) + "}");
    });
    server.on("/ap", HTTP_POST, []() { // turn the board's own WiFi on or off from the phone
        bool on = server.arg("on") == "1";
        if (on != apOn) {
            on ? apStart() : apStop();
            apSave(apOn);
            if (appIndex == APP_HOTSPOT) hotspotDraw();
        }
        server.send(200, "text/plain", apOn ? "on" : "off");
    });
    server.on("/app", HTTP_POST, []() { // open an app (or the menu) from the phone
        if (server.arg("open") == "menu") {
            backToMenu();
            return server.send(200, "text/plain", "menu");
        }
        if (server.arg("open") == "home") {
            goHome();
            return server.send(200, "text/plain", "home");
        }
        int i = server.arg("open").toInt();
        if (i < 0 || i >= APP_COUNT) return server.send(400, "text/plain", "unknown app");
        openApp(i);
        server.send(200, "text/plain", APPS[i].name);
    });
    server.on("/app", HTTP_GET, []() { // lets the phone (and tests) see what the screen shows
        server.send(200, "application/json", String("{\"app\":\"") + (appIndex == HOME ? "home" : appIndex < 0 ? "menu" : APPS[appIndex].name) +
            "\",\"index\":" + String(appIndex) + ",\"menuSel\":" + String(menuSel) + "}");
    });
}

// ---------- rotary encoder ----------

// Quadrature decoding: every transition of the two contacts scores +1, -1 or 0 (an
// impossible jump), and one detent of an EC11 is four of them. Contact bounce shows up as
// transitions that cancel out, so it is filtered by the arithmetic itself.
const int8_t QUADRATURE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
volatile int8_t encSubStep = 0;
volatile int8_t encSteps = 0;
volatile uint8_t encPrev = 0;

void IRAM_ATTR encoderIsr() {
    uint8_t now = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
    if (now == encPrev) return;
    if ((now ^ encPrev) & 2) encEdgesA++;
    if ((now ^ encPrev) & 1) encEdgesB++;
    encSubStep += QUADRATURE[(encPrev << 2) | now];
    encPrev = now;
    if (encSubStep >= 4) {
        encSubStep = 0;
        encSteps++;
    } else if (encSubStep <= -4) {
        encSubStep = 0;
        encSteps--;
    }
}

void onRotate(int dir) {
    if (appIndex == HOME) {
        return; // the home clock has nothing to scroll
    } else if (appIndex < 0) {
        menuMove(dir);
    } else if (appIndex == APP_TIMER) {
        timerRotate(dir);
    } else if (appIndex == APP_READER) {
        if (!readerInList) {
            stepPage(dir);
            return;
        }
        if (docCacheN == 0) return;
        int previous = readerAt;
        readerAt = (readerAt + dir + docCacheN) % docCacheN;
        readerDrawRow(previous); // only the two rows that changed, so scrolling stays instant
        readerDrawRow(readerAt);
    } else if (appIndex == APP_ASK) {
        askRotate(dir);
    } else if (appIndex == APP_LED) {
        int count = sizeof(PRESETS) / sizeof(PRESETS[0]);
        presetIndex = (presetIndex + dir + count) % count;
        ledShow();
    }
}

void encoderService() {
    noInterrupts();
    int8_t steps = encSteps;
    encSteps = 0;
    interrupts();
    for (; steps > 0; steps--) {
        rotations++;
        onRotate(1);
    }
    for (; steps < 0; steps++) {
        rotations++;
        onRotate(-1);
    }
}

// ---------- button ----------

struct ButtonState {
    bool armed = false; // a button counts only after it has been seen released once
    bool down = false, longFired = false;
    unsigned long pressedMs = 0;
};


// The knob only selects - going back is the dedicated back button's job - so holding it does
// nothing. The BOOT button has to do everything on its own: a tap moves, a hold opens or goes back.
void serviceButton(bool now, bool tapSelects, bool holdActs, ButtonState &s) {
    if (!s.armed) { // pin levels settle for a few milliseconds after boot; ignore that
        if (!now) s.armed = true;
        return;
    }
    if (now && !s.down) {
        s.down = true;
        s.longFired = false;
        s.pressedMs = millis();
    } else if (holdActs && now && s.down && !s.longFired && millis() - s.pressedMs > HOLD_MS) {
        s.longFired = true;
        if (appIndex == HOME) backToMenu();
        else if (appIndex < 0) openApp(menuSel);
        else goBack();
    } else if (!now && s.down) {
        s.down = false;
        if (s.longFired || millis() - s.pressedMs < 40) return; // 40 ms debounce
        tapSelects ? knobTaps++ : bootTaps++;
        if (appIndex == HOME) {
            backToMenu(); // one press on the home clock opens the menu
        } else if (appIndex >= 0) {
            if (APPS[appIndex].press) APPS[appIndex].press();
        } else if (tapSelects) {
            openApp(menuSel);
        } else {
            menuMove(1);
        }
    }
}

void handleButton() {
    static bool armed = false;
    static ButtonState boot, knob;
    bool bootDown = digitalRead(BUTTON) == LOW;
    if (!armed) { // the programmer holds GPIO0 low across a reset; ignore it until released
        if (!bootDown) armed = true;
        bootDown = false;
    }
    serviceButton(bootDown, false, true, boot);
    serviceButton(digitalRead(ENC_SW) == LOW, true, false, knob);

    // Dedicated back button: a tap leaves the app, no need to hold anything.
    static ButtonState back;
    bool backDown = digitalRead(BACK_BTN) == LOW;
    if (!back.armed) {
        if (!backDown) back.armed = true;
        return;
    }
    if (backDown && !back.down) {
        back.down = true;
        back.pressedMs = millis();
    } else if (!backDown && back.down) {
        back.down = false;
        if (millis() - back.pressedMs > 40) {
            backTaps++;
            goBack();
        }
    }
}

// Runs every loop whatever is on screen: ends the countdown, brings the timer screen to the
// front and flashes the LED (for a minute, so a forgotten alarm doesn't blink forever).
void timerService() {
    if (timerState == TimerState::Running && (long)(millis() - timerEndMs) >= 0) {
        timerState = TimerState::Done;
        timerDoneMs = millis();
        if (appIndex != APP_TIMER) openApp(APP_TIMER);
    }
    if (timerState != TimerState::Done) return;
    static unsigned long lastBlinkMs = 0;
    static bool lit = false;
    if (millis() - timerDoneMs > 60000) {
        if (lit) setLed(0, 0, 0);
        lit = false;
        return;
    }
    if (millis() - lastBlinkMs > 250) {
        lastBlinkMs = millis();
        lit = !lit;
        setLed(lit ? 255 : 0, 0, 0);
    }
}

// A running timer also shows on the home clock, under the date.
void homeTimerLine() {
    bool active = timerState != TimerState::Set;
    long s = active ? (timerRemainingMs() + 999) / 1000 : -1;
    if (s == homeTimerShownSec) return;
    homeTimerShownSec = s;
    String text = active ? "Timer " + timerText() + (timerState == TimerState::Paused ? " (paused)" : "") : "";
    clockLine(CLOCK_TIMER_Y, text, tft.color565(255, 140, 0), 2, 19);
}

// ---------- boot log ----------
// The last 12 boots are kept in NVS: why the chip reset, how far setup got and how long it then
// ran, and whether a computer was on the USB port. Runs on a charger (no serial, maybe no WiFi)
// can then be read back over /boots once the board is on the laptop again.
struct BootRecord {
    uint32_t seq;
    uint8_t reason;   // esp_reset_reason_t
    uint8_t stage;    // BootStage reached
    uint8_t usbHost;  // 1 once a USB host was seen talking to the serial port
    uint8_t pad;
    uint32_t upSec;   // uptime at the last checkpoint
    uint16_t battMv;  // battery pin reading at 10 s (0 = not measured yet)
};
enum BootStage : uint8_t { BOOT_START = 1, BOOT_SCREEN, BOOT_STORAGE, BOOT_WIFI_OK, BOOT_WIFI_FAIL, BOOT_READY };
const int BOOT_LOG_N = 12;
BootRecord bootLog[BOOT_LOG_N]; // [0] = this boot

void bootLogSave() {
    prefs.begin("boot", false);
    prefs.putBytes("log", bootLog, sizeof(bootLog));
    prefs.end();
}

void bootLogStart() {
    prefs.begin("boot", true);
    size_t got = prefs.getBytes("log", bootLog, sizeof(bootLog));
    prefs.end();
    if (got != sizeof(bootLog)) memset(bootLog, 0, sizeof(bootLog));
    memmove(&bootLog[1], &bootLog[0], sizeof(BootRecord) * (BOOT_LOG_N - 1));
    bootLog[0] = {bootLog[1].seq + 1, (uint8_t)esp_reset_reason(), BOOT_START, 0, 0, 0, 0};
    bootLogSave();
}

void bootLogStage(BootStage st) {
    bootLog[0].stage = st;
    bootLog[0].upSec = millis() / 1000;
    bootLogSave();
}

// Checkpoints at 10 s, 30 s, 1 min, 2 min and 5 min of uptime; no flash writes after that.
void bootLogTick() {
    static const uint32_t MARKS[] = {10, 30, 60, 120, 300};
    static int next = 0;
    if (next >= 5 || millis() / 1000 < MARKS[next]) return;
    next++;
    bootLog[0].upSec = millis() / 1000;
    if (HWCDC::isConnected()) bootLog[0].usbHost = 1;
    if (!bootLog[0].battMv) bootLog[0].battMv = (uint16_t)(batteryVoltsRaw() * 1000);
    bootLogSave();
}

const char *resetReasonName(uint8_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "reset pin";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "crash";
        case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        default: return "unknown";
    }
}
const char *bootStageName(uint8_t st) {
    static const char *N[] = {"-", "start", "screen", "storage", "wifi ok", "wifi fail", "ready"};
    return st <= BOOT_READY ? N[st] : "?";
}

void setup() {
    Serial.begin(115200);
    bootLogStart();
    pinMode(BUTTON, INPUT_PULLUP);
    pinMode(ENC_A, INPUT_PULLUP);
    pinMode(ENC_B, INPUT_PULLUP);
    pinMode(ENC_SW, INPUT_PULLUP);
    pinMode(BACK_BTN, INPUT_PULLUP);
    encPrev = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
    attachInterrupt(ENC_A, encoderIsr, CHANGE);
    attachInterrupt(ENC_B, encoderIsr, CHANGE);
    initLed();
    setLed(0, 0, 0);
    micBegin();
    recBegin();

    // On a plain charger the board starts the instant power appears, whereas a computer's USB
    // enumeration delays it. Give the 3.3 V rail time to settle before initialising the panel,
    // otherwise it comes up blank and stays blank.
    delay(300);
    screenInit();
    header("Starting...");
    bootLogStage(BOOT_SCREEN);

    // Mount without the format-on-failure flag first: formatting wipes every stored document,
    // so it must be a visible last resort rather than something that happens silently at boot.
    if (!FFat.begin(false)) {
        Serial.println("FFat mount FAILED - formatting, stored documents are lost");
        if (!FFat.begin(true)) Serial.println("FFat format failed too");
    }
    bootLogStage(BOOT_STORAGE);
    prefs.begin("doc", true);
    int slot = prefs.getInt("curdoc", -1);
    prefs.end();
    for (int s = 0; s < MAX_DOCS && !docExists(slot); s++) { // last read document, else any
        if (docExists(s)) slot = s;
    }
    if (docExists(slot)) {
        curDoc = slot;
        pageCount = docPages(slot);
        currentPage = min(docSavedPage(slot), pageCount - 1);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    line(50, "Connecting WiFi...");
    for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(250);
    bootLogStage(WiFi.status() == WL_CONNECTED ? BOOT_WIFI_OK : BOOT_WIFI_FAIL);
    if (WiFi.status() == WL_CONNECTED) {
        MDNS.begin(HOSTNAME);
        MDNS.addService("http", "tcp", 80);
        configTime(7 * 3600, 0, "pool.ntp.org"); // Vietnam time
        Serial.printf("Ready: http://%s/ | stored pages: %d\n", WiFi.localIP().toString().c_str(), pageCount);
    } else {
        Serial.println("No WiFi; the screen apps still work");
    }

    navLoadDest();
    clockBgLoad();

    prefs.begin("ap", true);
    bool apWanted = prefs.getBool("on", false);
    prefs.end();
    if (apWanted) apStart(); // the board keeps broadcasting across reboots if it was left on

    setupWeb();
    server.on("/boots", HTTP_GET, []() {
        String out;
        for (int i = 0; i < BOOT_LOG_N && bootLog[i].seq; i++) {
            const BootRecord &b = bootLog[i];
            char row[140];
            snprintf(row, sizeof(row), "#%u%s reset=%s reached=%s up=%us usbHost=%s batt=%.2fV\n", b.seq,
                i == 0 ? " (now)" : "", resetReasonName(b.reason), bootStageName(b.stage), b.upSec,
                b.usbHost ? "yes" : "no", b.battMv / 1000.0);
            out += row;
        }
        server.send(200, "text/plain", out);
    });
    server.begin();
    goHome();
    bootLogStage(BOOT_READY);
}

void loop() {
    if (apOn) dns.processNextRequest();
    server.handleClient();
    encoderService();
    handleButton();
    if (appIndex >= 0 && APPS[appIndex].tick) APPS[appIndex].tick();
    if (appIndex == HOME) {
        clockTick();
        homeTimerLine();
    }
    timerService();
    bootLogTick();
    // A recording ends when its screen is left, or before it fills the storage.
    static unsigned long lastRecCheckMs = 0;
    if (recording && millis() - lastRecCheckMs > 1000) {
        lastRecCheckMs = millis();
        if (appIndex != recOwner || FFat.freeBytes() < 256 * 1024 || recSamples >= REC_MAX_SAMPLES) {
            recStop();
            if (appIndex == APP_REC) recDraw();
        }
    }

    static unsigned long lastSampleMs = 0, lastBatteryMs = 0;
    if (millis() - lastSampleMs > 500) {
        lastSampleMs = millis();
        batteryUpdate();
    }
    // Refresh the battery indicator, but not while a document page or a phone image covers
    // the title bar.
    if (millis() - lastBatteryMs > 15000) {
        lastBatteryMs = millis();
        if (appIndex != HOME && appIndex != APP_READER && appIndex != APP_RECEIVE) drawBatteryIcon();
    }
}
