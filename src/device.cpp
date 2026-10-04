#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <StreamString.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <FFat.h>
#include <Preferences.h>
#include <time.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>
#include <esp_adc_cal.h>
#include <vector>
#include <algorithm>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <mbedtls/base64.h>
#include "json_escape.h"
#include "rgb_led.h"
#include "screen_page.h"
#include "wifi_config.h"
#include "ui_fonts.h"
#include "ancs_client.h"

#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  13
#define BUTTON    0 // the BOOT button: tap = next / act, hold = open / back
// Two push buttons replace the rotary encoder (one leg on the pin, the other to GND):
// a tap moves up / down like turning the knob did, holding either one selects.
#define BTN_UP    4
#define BTN_DOWN  5
#define BTN_OK    3 // OK / select button. GPIO3 is a strapping pin, but only for JTAG source selection
                    // when an eFuse is burned (not on this board), so a button here is safe at boot.
#define BACK_BTN  7 // optional "back" button: one leg here, the other to GND

const int W = 240, H = 320;
const uint32_t PAGE_BYTES = (uint32_t)W * H * 2;
const uint32_t MONO_BYTES = 4 + (uint32_t)W * H / 8;
const int MAX_PAGES = 800;
const unsigned long HOLD_MS = 700;
const char *HOSTNAME = "esp32";

// The panel is write-only (no MISO wire), so a copy of what is on it is kept in PSRAM: every
// drawing call lands in both. GET /screen returns that copy as a BMP, which is how a layout is
// checked without anyone looking at the board.
class MirrorTFT : public Adafruit_ST7789 {
public:
    using Adafruit_ST7789::Adafruit_ST7789;
    using Adafruit_ST7789::drawRGBBitmap;
    uint16_t *mirror = nullptr;

    void mirrorFill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
        if (!mirror || !clip(x, y, w, h)) return;
        for (int r = 0; r < h; r++) {
            uint16_t *p = mirror + (y + r) * W + x;
            for (int i = 0; i < w; i++) p[i] = c;
        }
    }
    void drawPixel(int16_t x, int16_t y, uint16_t c) override { mirrorFill(x, y, 1, 1, c); Adafruit_ST7789::drawPixel(x, y, c); }
    void writePixel(int16_t x, int16_t y, uint16_t c) override { mirrorFill(x, y, 1, 1, c); Adafruit_ST7789::writePixel(x, y, c); }
    void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
        mirrorFill(x, y, w, h, c);
        Adafruit_ST7789::writeFillRect(x, y, w, h, c);
    }
    void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override { mirrorFill(x, y, w, 1, c); Adafruit_ST7789::writeFastHLine(x, y, w, c); }
    void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override { mirrorFill(x, y, 1, h, c); Adafruit_ST7789::writeFastVLine(x, y, h, c); }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override { mirrorFill(x, y, w, h, c); Adafruit_ST7789::fillRect(x, y, w, h, c); }
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override { mirrorFill(x, y, w, 1, c); Adafruit_ST7789::drawFastHLine(x, y, w, c); }
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override { mirrorFill(x, y, 1, h, c); Adafruit_ST7789::drawFastVLine(x, y, h, c); }
    void drawRGBBitmap(int16_t x, int16_t y, uint16_t *pc, int16_t w, int16_t h) {
        if (mirror) {
            for (int r = 0; r < h; r++) {
                int yy = y + r;
                if (yy < 0 || yy >= H) continue;
                for (int i = 0; i < w; i++) {
                    int xx = x + i;
                    if (xx >= 0 && xx < W) mirror[yy * W + xx] = pc[r * w + i];
                }
            }
        }
        Adafruit_ST7789::drawRGBBitmap(x, y, pc, w, h);
    }

private:
    static bool clip(int16_t &x, int16_t &y, int16_t &w, int16_t &h) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > W) w = W - x;
        if (y + h > H) h = H - y;
        return w > 0 && h > 0;
    }
};
MirrorTFT tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

// Screen language, picked in Settings and kept in NVS ("ui"/"lang"). The UI fonts carry the
// Vietnamese letters, so both languages are drawn the same way.
enum UiLang : uint8_t { LANG_EN, LANG_VI };
uint8_t uiLang = LANG_EN;
const char *T(const char *en, const char *vi) { return uiLang == LANG_VI ? vi : en; }
WebServer server(80);
Preferences prefs;
String otaError;
size_t otaBytes = 0;

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
long homeTimerShownSec = -2; // second last drawn on the home clock's timer line

// Press counters, so wiring can be checked over the network instead of by watching the screen.
uint32_t bootTaps = 0, knobTaps = 0, backTaps = 0, upTaps = 0, downTaps = 0;

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
// The battery feeds the 3V3 rail through the charger module, so measuring the battery means
// measuring the chip's own supply. No external parts: GPIO1 (ADC1 channel 0, left unconnected)
// has both its internal pull-up (to 3V3) and pull-down (to GND) on, which makes a divider inside
// the chip. Those resistors are not matched, so the ratio is calibrated per board: with USB
// power the rail is the board regulator's 3.30 V (POST /battery/calibrate?volts=3.30).
#define BATTERY_PIN 1
const float BATTERY_DIVIDER_DEFAULT = 3.30f / 1.512f; // measured on this board, 2026-10-03
float batteryDivider = BATTERY_DIVIDER_DEFAULT;
esp_adc_cal_characteristics_t batteryCal;

void batteryBegin() {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &batteryCal);
    rtc_gpio_pullup_en(GPIO_NUM_1);
    rtc_gpio_pulldown_en(GPIO_NUM_1);
    prefs.begin("battery", true);
    batteryDivider = prefs.getFloat("divider", BATTERY_DIVIDER_DEFAULT);
    prefs.end();
}

// Median of a burst in mV at the pin, before the divider.
uint32_t batteryPinMv() {
    const int N = 15;
    uint32_t s[N];
    for (int i = 0; i < N; i++) s[i] = esp_adc_cal_raw_to_voltage(adc1_get_raw(ADC1_CHANNEL_0), &batteryCal);
    std::sort(s, s + N);
    return s[N / 2];
}

// The ADC is noisy by tens of mV and the LiPo curve is steep around 3.7 V, so a raw reading
// would jump several percent between samples. Take the median of a burst (kills spikes from
// WiFi transmit bursts) and then smooth it over time.
float batteryVoltsRaw() {
    const int N = 15;
    uint32_t s[N];
    for (int i = 0; i < N; i++) s[i] = esp_adc_cal_raw_to_voltage(adc1_get_raw(ADC1_CHANNEL_0), &batteryCal);
    for (int i = 1; i < N; i++) { // insertion sort, N is tiny
        uint32_t key = s[i];
        int j = i - 1;
        for (; j >= 0 && s[j] > key; j--) s[j + 1] = s[j];
        s[j + 1] = key;
    }
    return s[N / 2] * batteryDivider / 1000.0f;
}

float batterySmoothed = 0;
float batteryRecent[10]; // last 5 seconds of raw readings
int batteryRecentAt = 0, batteryRecentCount = 0;

void batteryChargeUpdate(float v);
void batteryUpdate() { // called about twice a second: time constant is roughly 10 seconds
    float v = batteryVoltsRaw();
    batterySmoothed = batterySmoothed == 0 ? v : batterySmoothed + (v - batterySmoothed) * 0.05f;
    batteryRecent[batteryRecentAt] = v;
    batteryRecentAt = (batteryRecentAt + 1) % 10;
    if (batteryRecentCount < 10) batteryRecentCount++;
    batteryChargeUpdate(v);
}

float batteryVolts() {
    if (batterySmoothed == 0) batteryUpdate();
    return batterySmoothed;
}

// Charging, inferred from the voltage alone (nothing is wired to the charger's CHRG pin):
// plugging the charger in lifts the battery's terminal voltage by about 0.3 V within seconds
// (measured: 3.66 -> 3.96 V, and 3.95 -> 3.71 V on unplugging) while the readings wobble by up
// to 0.08 V, so a step has to pass 0.15 V.
// A step is the median of the last 3 s against the median of 10-13 s earlier. A slow rise
// over 5 minutes catches a charger that was already plugged in at power-on.
bool batteryCharging = false;
float batteryChargeLift = 0.2f, batteryChargeFrom = 4.0f; // size of the plug-in step, and the voltage right after it
float batteryHist[30]; // 15 s of raw readings
int batteryHistAt = 0, batteryHistN = 0;
float batteryTrend[6]; // the smoothed voltage once a minute
int batteryTrendN = 0;
unsigned long batteryTrendMs = 0;

float median6(const float *v) {
    float t[6];
    memcpy(t, v, sizeof(t));
    std::sort(t, t + 6);
    return (t[2] + t[3]) / 2;
}

void batteryChargeReset() {
    batteryHistN = 0;
    batteryTrendN = 0;
    batteryTrendMs = millis();
}

void batteryChargeUpdate(float v) {
    if (v < 3.36f) { // USB power or no battery
        batteryCharging = false;
        batteryChargeReset();
        return;
    }
    batteryHist[batteryHistAt] = v;
    batteryHistAt = (batteryHistAt + 1) % 30;
    if (batteryHistN < 30) batteryHistN++;
    if (batteryHistN == 30) {
        float recent[6], older[6];
        for (int i = 0; i < 6; i++) {
            recent[i] = batteryHist[(batteryHistAt - 1 - i + 60) % 30];
            older[i] = batteryHist[(batteryHistAt - 21 - i + 60) % 30];
        }
        float step = median6(recent) - median6(older);
        if ((step > 0.15f && !batteryCharging) || (step < -0.15f && batteryCharging)) {
            batteryCharging = step > 0;
            if (batteryCharging) {
                batteryChargeLift = min(step, 0.4f);
                batteryChargeFrom = median6(recent);
            }
            Serial.printf("Charger %s (step %+.2f V)\n", batteryCharging ? "plugged in" : "unplugged", step);
            batteryChargeReset();
            return;
        }
    }
    if (millis() - batteryTrendMs < 60000) return;
    batteryTrendMs = millis();
    if (batteryTrendN == 6) {
        memmove(batteryTrend, batteryTrend + 1, sizeof(float) * 5);
        batteryTrendN = 5;
    }
    batteryTrend[batteryTrendN++] = batterySmoothed;
    if (batteryTrendN < 6) return;
    float rise = batteryTrend[5] - batteryTrend[0];
    // Near full the charger stops and restarts by itself, so a slow fall up there is not an unplug.
    if (!batteryCharging && rise > 0.04f) { // no step seen: assume a typical lift
        batteryCharging = true;
        batteryChargeLift = 0.2f;
        batteryChargeFrom = batterySmoothed;
    }
    else if (batteryCharging && rise < -0.03f && v < 4.0f) batteryCharging = false;
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

// On USB the rail sits at the regulator's 3.30 V; a LiPo that low is all but empty (the board
// browns out just below it), so a steady reading under 3.36 V is shown as USB power.
bool batteryPresent(float v) { return v > 3.36f && v < 4.6f && batterySpread() < 0.25f; }

int batteryPercent(float v) { // a LiPo's voltage is not linear in charge, so interpolate a curve
    // The charge current lifts the terminal voltage above the resting one, by the step seen at
    // plug-in; the lift shrinks to nothing as the charger tapers off towards 4.2 V.
    if (batteryCharging && batteryChargeFrom < 4.19f) {
        float left = constrain((4.2f - v) / (4.2f - batteryChargeFrom), 0.0f, 1.0f);
        v -= batteryChargeLift * left;
    }
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

// ---------- UI kit ----------
// Black screens with lifted cards, one blue accent, and anti-aliased Be Vietnam Pro text
// (include/ui_fonts.h). Colours are RGB565.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
const uint16_t UI_BG = 0x0000;
const uint16_t UI_TXT = rgb565(240, 242, 247), UI_TXT2 = rgb565(205, 210, 220), UI_SUB = rgb565(138, 146, 160);
const uint16_t UI_CARD = rgb565(24, 28, 37), UI_CARD_SEL = rgb565(34, 42, 58), UI_LINE = rgb565(44, 50, 62);
const uint16_t ACCENT = rgb565(92, 170, 255), MUTED = UI_SUB, DIVIDER = UI_LINE;
const uint16_t UI_GREEN = rgb565(80, 220, 120), UI_YELLOW = rgb565(255, 204, 0), UI_RED = rgb565(255, 90, 80);
const uint16_t UI_ORANGE = rgb565(255, 159, 10);
const int CONTENT_Y = 66; // below the status bar and the title

const int UI_BUF_PIXELS = W * 100;
uint16_t *uiBuf = nullptr; // a box of text is composed here, then pushed in one go
enum { AL_LEFT, AL_CENTER, AL_RIGHT };

uint32_t utf8Next(const char *&p) {
    uint8_t c = *p++;
    if (c < 0x80) return c;
    int n = (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 0;
    uint32_t cp = c & (0x3F >> n);
    while (n-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    return cp;
}

const AAGlyph *aaFind(const AAFont &f, uint32_t code) {
    int lo = 0, hi = f.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f.glyphs[mid].code == code) return &f.glyphs[mid];
        if (f.glyphs[mid].code < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

int aaWidth(const AAFont &f, const String &s) {
    int w = 0;
    for (const char *p = s.c_str(); *p;) {
        const AAGlyph *g = aaFind(f, utf8Next(p));
        w += g ? g->adv : f.glyphs[0].adv; // unknown characters take a space
    }
    return w;
}

int uiTextH(const AAFont &f) { return f.ascent + f.descent; }

inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a) { // a = coverage 0..15
    if (a >= 15) return fg;
    uint8_t b = 15 - a;
    uint16_t r = ((fg >> 11) * a + (bg >> 11) * b) / 15;
    uint16_t g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * b) / 15;
    uint16_t bl = ((fg & 31) * a + (bg & 31) * b) / 15;
    return (r << 11) | (g << 5) | bl;
}

// Blends `s` into a w x h pixel buffer, pen starting at (penX, baseY) on the baseline.
void aaDraw(const AAFont &f, uint16_t *buf, int bw, int bh, int penX, int baseY, const char *s, uint16_t fg) {
    for (const char *p = s; *p;) {
        const AAGlyph *g = aaFind(f, utf8Next(p));
        if (!g) {
            penX += f.glyphs[0].adv;
            continue;
        }
        const uint8_t *bits = f.bits + g->offset;
        for (int yy = 0; yy < g->h; yy++) {
            int by = baseY + g->dy + yy;
            if (by < 0 || by >= bh) continue;
            for (int xx = 0; xx < g->w; xx++) {
                int bx = penX + g->dx + xx;
                if (bx < 0 || bx >= bw) continue;
                int i = yy * g->w + xx;
                uint8_t a = (bits[i >> 1] >> (i & 1 ? 0 : 4)) & 15;
                if (a) buf[by * bw + bx] = blend565(buf[by * bw + bx], fg, a);
            }
        }
        penX += g->adv;
    }
}

// One line of text in a box (x, y, w, h), drawn together with the box's background and pushed
// to the panel in one go, so a value that changes redraws without flicker. The text is centred
// vertically. `bgImg`, when given, is a full-screen picture the box shows through instead of a
// flat colour; `shadow` adds a dark copy offset by a pixel, for text over a photo.
void uiBox(const AAFont &f, int x, int y, int w, int h, const String &s, uint16_t fg, uint16_t bg,
           uint8_t align = AL_LEFT, const uint16_t *bgImg = nullptr, bool shadow = false) {
    int shift = 0;
    if (x < 0) {
        shift = x;
        w += x;
        x = 0;
    }
    if (x + w > W) w = W - x;
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > H) h = H - y;
    if (!uiBuf || w <= 0 || h <= 0 || w * h > UI_BUF_PIXELS) return;
    for (int r = 0; r < h; r++) {
        if (bgImg) memcpy(uiBuf + r * w, bgImg + (y + r) * W + x, w * 2);
        else for (int i = 0; i < w; i++) uiBuf[r * w + i] = bg;
    }
    int tw = aaWidth(f, s);
    int px = (align == AL_CENTER ? (w - shift - tw) / 2 : align == AL_RIGHT ? w - tw : 0) + shift;
    int base = (h - uiTextH(f)) / 2 + f.ascent;
    if (shadow) aaDraw(f, uiBuf, w, h, px + 1, base + 1, s.c_str(), 0x0000);
    aaDraw(f, uiBuf, w, h, px, base, s.c_str(), fg);
    tft.drawRGBBitmap(x, y, uiBuf, w, h);
}

// Text whose box is exactly as wide as the text; returns that width.
int uiText(const AAFont &f, int x, int y, const String &s, uint16_t fg, uint16_t bg = UI_BG) {
    int w = aaWidth(f, s) + 2;
    uiBox(f, x, y, w, uiTextH(f), s, fg, bg);
    return w;
}

// A horizontal bar with round ends: `frac` of it in `color`, the rest in `track`, outside the
// round ends `bg`. Composed off-screen like text, so a level that updates often doesn't flicker.
void uiBar(int x, int y, int w, int h, float frac, uint16_t color, uint16_t track = UI_LINE, uint16_t bg = UI_BG) {
    if (!uiBuf || w * h > UI_BUF_PIXELS) return;
    int fillW = constrain((int)lroundf(w * frac), 0, w);
    float r = h / 2.0f;
    for (int row = 0; row < h; row++) {
        float dy = row + 0.5f - r;
        int inset = (int)ceilf(r - sqrtf(max(0.0f, r * r - dy * dy)) - 0.5f);
        for (int i = 0; i < w; i++) {
            uint16_t c = i < fillW ? color : track;
            if (i < inset || i >= w - inset) c = bg;
            uiBuf[row * w + i] = c;
        }
    }
    tft.drawRGBBitmap(x, y, uiBuf, w, h);
}

// Battery: the percentage, then a small battery (with a bolt while charging), its right edge at
// `right`; "USB" when no battery is read. `bgImg` as in uiBox, for the clock's photo.
void uiBattery(int right, int y, uint16_t bg = UI_BG, const uint16_t *bgImg = nullptr) {
    float v = batteryVolts();
    const int iw = 21, ih = 11, ix = right - iw - 2, iy = y + 5;
    if (!batteryPresent(v)) {
        uiBox(ui12, right - 60, y, 60, 20, "USB", UI_SUB, bg, AL_RIGHT, bgImg, bgImg != nullptr);
        return;
    }
    int pct = batteryPercent(v);
    uiBox(ui12, ix - 46, y, 42, 20, String(pct) + "%", bgImg ? UI_TXT : UI_SUB, bg, AL_RIGHT, bgImg, bgImg != nullptr);
    uint16_t frame = rgb565(170, 176, 188);
    if (bgImg) {
        for (int r = 0; r < 20; r++) tft.drawRGBBitmap(ix - 4, y + r, (uint16_t *)bgImg + (y + r) * W + ix - 4, iw + 8, 1);
    } else {
        tft.fillRect(ix - 4, y, iw + 8, 20, bg);
    }
    tft.drawRoundRect(ix, iy, iw, ih, 3, frame);
    tft.fillRect(ix + iw, iy + 3, 2, ih - 6, frame);
    uint16_t c = batteryCharging ? UI_GREEN : pct > 20 ? UI_GREEN : UI_RED;
    int fw = max(1, (iw - 4) * pct / 100);
    tft.fillRoundRect(ix + 2, iy + 2, fw, ih - 4, 1, c);
    if (batteryCharging) { // a bolt over the bar
        int cx = ix + iw / 2, cy = iy + ih / 2;
        tft.fillTriangle(cx + 2, cy - 5, cx - 4, cy + 1, cx + 1, cy + 1, UI_TXT);
        tft.fillTriangle(cx - 1, cy - 1, cx + 4, cy - 1, cx - 2, cy + 5, UI_TXT);
    }
}

// Battery indicator in the title bar, so every screen that has one shows it.
// Status bar, on every screen but the clock: the time on the left, the battery on the right.
// It is redrawn only when one of them changes (uiStatusTick).
int statusShownKey = -2;

int statusKey() {
    time_t now = time(nullptr);
    int minute = now > 8 * 3600 ? (int)((now / 60) % 1440) : -1;
    float v = batteryVolts();
    int batt = batteryPresent(v) ? batteryPercent(v) + (batteryCharging ? 1000 : 0) : -1;
    return minute * 4000 + batt;
}

void uiStatusBar() {
    time_t now = time(nullptr);
    String hhmm;
    if (now > 8 * 3600) {
        struct tm t;
        localtime_r(&now, &t);
        char buf[8];
        strftime(buf, sizeof(buf), "%H:%M", &t);
        hhmm = buf;
    }
    uiBox(ui13b, 14, 2, 70, 20, hhmm, UI_TXT, UI_BG);
    uiBattery(W - 12, 2);
    statusShownKey = statusKey();
}

void screenInit() {
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); // the board definition's default SPI pins are other ones
    tft.init(W, H);
    tft.setRotation(0);
    tft.setTextWrap(false); // a long line must be clipped, never wrapped onto the layout below
    if (!tft.mirror) tft.mirror = (uint16_t *)ps_calloc(W * H, 2);
    if (!uiBuf) uiBuf = (uint16_t *)ps_malloc(UI_BUF_PIXELS * 2);
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
// One line of UI text at (x, y) (top-left, as with the built-in font). With `solid`, what was there
// before is overwritten: by the glyph background in the built-in font (callers pad with spaces),
// by clearing the strip first for the Unifont. Unifont text is clipped to the screen width.
// (old built-in-font text helper removed: see uiBox / uiText above)

// A screen's top: status bar, then the title in large type. Content starts at CONTENT_Y.
void header(const char *title) {
    tft.fillScreen(UI_BG);
    uiStatusBar();
    uiBox(ui20b, 14, 30, W - 28, uiTextH(ui20b), title, UI_TXT, UI_BG);
}

// A full-width line of text from the left margin (size 1 small, 2 body, 3 large), clearing
// whatever was on that line before.
const AAFont &uiFontForSize(uint8_t size) { return size <= 1 ? ui12 : size == 2 ? ui15 : ui20b; }
void line(int y, const String &text, uint16_t color = UI_TXT, uint8_t size = 2) {
    const AAFont &f = uiFontForSize(size);
    uiBox(f, 14, y, W - 28, uiTextH(f), text, color, UI_BG);
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
int readerTop = 0; // first row on screen

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

// A card per document: the name, and the page count with where reading stopped.
const int DOC_ROW_H = 50, DOC_ROWS = 5;
void readerDrawRow(int i) {
    if (i < 0 || i >= docCacheN || i < readerTop || i >= readerTop + DOC_ROWS) return;
    const DocInfo &d = docCache[i];
    bool sel = i == readerAt;
    int y = CONTENT_Y + (i - readerTop) * DOC_ROW_H;
    uint16_t bg = sel ? UI_CARD_SEL : UI_BG;
    tft.fillRoundRect(8, y, 224, DOC_ROW_H - 4, 12, bg);
    uiBox(sel ? ui15b : ui15, 20, y + 3, 196, 22, d.name, sel ? UI_TXT : UI_TXT2, bg);
    String sub = String(d.pages) + T(" pages", " trang") +
        (d.slot == curDoc ? String(T(" · on page ", " · đang ở trang ")) + (currentPage + 1) : String());
    uiBox(ui12, 20, y + 25, 196, 18, sub, UI_SUB, bg);
}

void readerMove(int dir) {
    if (docCacheN == 0) return;
    int previous = readerAt;
    readerAt = (readerAt + dir + docCacheN) % docCacheN;
    int top = readerTop;
    if (readerAt < top) top = readerAt;
    if (readerAt >= top + DOC_ROWS) top = readerAt - DOC_ROWS + 1;
    if (top != readerTop) {
        readerTop = top;
        tft.fillRect(0, CONTENT_Y, W, DOC_ROWS * DOC_ROW_H, UI_BG);
        for (int k = readerTop; k < readerTop + DOC_ROWS; k++) readerDrawRow(k);
    } else { // only the two rows that changed, so scrolling stays instant
        readerDrawRow(previous);
        readerDrawRow(readerAt);
    }
}

void readerDrawList() {
    header(T("Documents", "Tài liệu"));
    refreshDocCache();
    if (docCacheN == 0) {
        line(CONTENT_Y, T("No documents", "Chưa có tài liệu"), UI_SUB);
        line(CONTENT_Y + 26, "http://" + WiFi.localIP().toString() + "/", ACCENT, 1);
        return;
    }
    if (readerAt < readerTop || readerAt >= readerTop + DOC_ROWS) readerTop = max(0, readerAt - DOC_ROWS + 1);
    for (int i = readerTop; i < readerTop + DOC_ROWS; i++) readerDrawRow(i);
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
    header(T("From phone", "Từ điện thoại"));
    tft.fillRoundRect(8, CONTENT_Y, 224, 72, 14, UI_CARD);
    uiBox(ui15b, 22, CONTENT_Y + 12, 196, 24, "http://" + WiFi.localIP().toString() + "/", ACCENT, UI_CARD);
    uiBox(ui13b, 22, CONTENT_Y + 38, 196, 22, "http://esp32.local/", UI_TXT2, UI_CARD);
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
        tft.fillRect(0, y, W, h, UI_BG);
        return;
    }
    tft.drawRGBBitmap(0, y, clockBg + y * W, W, h);
}

// A centred line of the clock screen. On black it is padded to `cells` characters and drawn with
// a solid background, so it overwrites the previous text without a clear (no flicker). On a
// photo it repaints its strip of the photo first, then draws the text over a dark shadow.
// A centred, full-width line of the clock screen, over black or over the photo (with a shadow).
void clockText(const AAFont &f, int y, const String &text, uint16_t color) {
    uiBox(f, 0, y, W, uiTextH(f), text, color, UI_BG, AL_CENTER, clockBg, clockBg != nullptr);
}

// Home screen: no title bar and no status lines, just the date and a large time, centred on
// black or on the photo from the phone; the battery sits in the top-right corner.
const int CLOCK_DATE_Y = 90, CLOCK_TIME_Y = 112, CLOCK_TIMER_Y = 262;

// HH:MM at text size 8. The font's colon sits in a full-width cell and leaves wide gaps, so the
// two digit pairs are printed separately around a colon drawn as two squares.
void clockTime(const char *hhmm) { clockText(num68, CLOCK_TIME_Y, hhmm, UI_TXT); }

// Battery in the top-right corner of the home screen: percent and a small battery, with a bolt
// while charging, or "USB". Redrawn only when what it shows changes.
int homeBatteryShown = -2; // pct, or -1 for USB; +1000 while charging; -2 = must redraw

void homeBatteryDraw(bool force = false) {
    float v = batteryVolts();
    bool present = batteryPresent(v);
    int key = present ? batteryPercent(v) + (batteryCharging ? 1000 : 0) : -1;
    if (!force && key == homeBatteryShown) return;
    homeBatteryShown = key;
    uiBattery(W - 12, 6, UI_BG, clockBg);
}

void clockEnter() {
    lastClockMinute = -1;
    clockBgRestore(0, H);
    homeBatteryDraw(true);
    clockText(ui12, 150, T("Waiting for time", "Đang lấy giờ"), UI_SUB);
}

void clockTick() {
    static unsigned long lastBatteryCheckMs = 0;
    if (millis() - lastBatteryCheckMs > 2000) {
        lastBatteryCheckMs = millis();
        homeBatteryDraw();
    }
    time_t now = time(nullptr);
    if (now < 8 * 3600) return; // NTP has not answered yet
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_min == lastClockMinute) return;
    if (lastClockMinute < 0) { // clears the "waiting for time" note
        clockBgRestore(0, H);
        homeBatteryDraw(true);
    }
    lastClockMinute = t.tm_min;

    static const char *DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *DAYS_VI[] = {"Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"};
    static const char *MONTHS[] = {"January", "February", "March", "April", "May", "June", "July",
                                   "August", "September", "October", "November", "December"};
    String date = uiLang == LANG_VI
        ? String(DAYS_VI[t.tm_wday]) + ", " + t.tm_mday + " tháng " + (t.tm_mon + 1)
        : String(DAYS[t.tm_wday]) + ", " + t.tm_mday + " " + MONTHS[t.tm_mon];
    clockText(ui15, CLOCK_DATE_Y, date, UI_SUB);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &t);
    clockTime(buf);
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
// A switch like a phone's settings: a pill with a knob, green when on.
void uiSwitch(int x, int y, bool on, uint16_t bg) {
    tft.fillRoundRect(x, y, 44, 26, 13, on ? UI_GREEN : UI_LINE);
    tft.fillCircle(on ? x + 31 : x + 13, y + 13, 10, UI_TXT);
}

void hotspotDraw() {
    header(T("Hotspot", "Phát WiFi"));
    lastApClients = -1;
    tft.fillRoundRect(8, CONTENT_Y, 224, apOn ? 104 : 64, 14, UI_CARD);
    uiBox(ui15b, 22, CONTENT_Y + 10, 140, 22, AP_SSID, UI_TXT, UI_CARD);
    uiBox(ui12, 22, CONTENT_Y + 32, 140, 18, apOn ? T("No password", "Không mật khẩu") : T("Off", "Đang tắt"), UI_SUB, UI_CARD);
    uiSwitch(176, CONTENT_Y + 19, apOn, UI_CARD);
    if (apOn) {
        tft.drawFastHLine(22, CONTENT_Y + 60, 196, UI_LINE);
        uiBox(ui13b, 22, CONTENT_Y + 70, 196, 22, "http://192.168.4.1", ACCENT, UI_CARD);
    }
}
void hotspotEnter() { hotspotDraw(); }
void hotspotTick() {
    if (!apOn) return;
    int clients = WiFi.softAPgetStationNum();
    if (clients == lastApClients) return;
    lastApClients = clients;
    line(CONTENT_Y + 118, T("Connected devices: ", "Thiết bị đang kết nối: ") + String(clients), UI_SUB, 1);
}
void hotspotPress() {
    apOn ? apStop() : apStart();
    apSave(apOn);
    hotspotDraw();
}

int radarState = 0; // 0 = scanning, 1 = done
void radarEnter() {
    header(T("WiFi scan", "Quét WiFi"));
    line(CONTENT_Y, T("Scanning...", "Đang quét..."), UI_SUB);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    radarState = 0;
}

void uiSignalBars(int x, int y, int rssi) { // four bars, bottom-aligned, the ones reached lit
    int level = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
    uint16_t c = level >= 3 ? UI_GREEN : level == 2 ? UI_YELLOW : UI_RED;
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        tft.fillRoundRect(x + i * 5, y + 13 - h, 3, h, 1, i < level ? c : UI_LINE);
    }
}
void radarTick() {
    if (radarState) return;
    int n = WiFi.scanComplete();
    if (n < 0) return;
    header(T("WiFi scan", "Quét WiFi"));
    const int rowH = 26;
    int shown = min(n, 9);
    for (int i = 0; i < shown; i++) {
        int y = CONTENT_Y + i * rowH;
        uiBox(ui15, 14, y + 2, 150, 22, WiFi.SSID(i), UI_TXT, UI_BG);
        uiBox(ui12, 164, y + 2, 40, 22, String(WiFi.RSSI(i)), UI_SUB, UI_BG, AL_RIGHT);
        uiSignalBars(210, y + 5, WiFi.RSSI(i));
    }
    line(CONTENT_Y + shown * rowH + 4, String(n) + T(" networks", " mạng"), UI_SUB, 1);
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
const char *PRESET_NAMES_VI[] = {"Tắt", "Đỏ", "Cam", "Vàng", "Xanh lá", "Xanh lơ", "Xanh dương", "Tím", "Hồng", "Trắng"};
int presetIndex = 0;
void ledShow() { // the swatch, the name and a dot per preset; the header stays
    const Preset &p = PRESETS[presetIndex];
    setLed(p.r, p.g, p.b);
    const int sy = CONTENT_Y + 14;
    tft.fillRect(0, sy, W, 200, UI_BG);
    if (presetIndex == 0) tft.drawRoundRect(48, sy, 144, 120, 24, UI_LINE);
    else tft.fillRoundRect(48, sy, 144, 120, 24, tft.color565(p.r, p.g, p.b));
    uiBox(ui20b, 0, sy + 140, W, uiTextH(ui20b), T(p.name, PRESET_NAMES_VI[presetIndex]), UI_TXT, UI_BG, AL_CENTER);
    int count = sizeof(PRESETS) / sizeof(PRESETS[0]);
    int x0 = (W - (count - 1) * 14) / 2;
    for (int i = 0; i < count; i++) tft.fillCircle(x0 + i * 14, sy + 196, 3, i == presetIndex ? UI_TXT : UI_LINE);
}
void ledEnter() {
    header(T("RGB light", "Đèn RGB"));
    ledShow();
}
void ledPress() {
    presetIndex = (presetIndex + 1) % (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
    ledShow();
}

// Label on the left, value on the right; labels once, values every second.
const int INFO_ROW_H = 24;
void infoRow(int i, const String &label, const String &value, bool withLabel) {
    int y = CONTENT_Y + i * INFO_ROW_H;
    if (withLabel) uiBox(ui12, 14, y, 100, INFO_ROW_H, label, UI_SUB, UI_BG);
    uiBox(ui13b, 100, y, W - 114, INFO_ROW_H, value, UI_TXT, UI_BG, AL_RIGHT);
}

void infoRows(bool withLabels) {
    float v = batteryVolts();
    String batt = batteryPresent(v) ? String(batteryPercent(v)) + "% · " + String(v, 2) + "V" + (batteryCharging ? T(" · charging", " · đang sạc") : "")
                                    : String(T("USB power", "Nguồn USB"));
    int i = 0;
    infoRow(i++, "WiFi", WiFi.status() == WL_CONNECTED ? String(WIFI_SSID) : String(T("not connected", "chưa kết nối")), withLabels);
    infoRow(i++, "IP", WiFi.localIP().toString(), withLabels);
    infoRow(i++, T("Signal", "Sóng"), String((int)WiFi.RSSI()) + " dBm", withLabels);
    infoRow(i++, T("Uptime", "Đã chạy"), String(millis() / 60000) + T(" min", " phút"), withLabels);
    infoRow(i++, T("Free RAM", "RAM trống"), String(ESP.getFreeHeap() / 1024) + " KB", withLabels);
    infoRow(i++, "PSRAM", String(ESP.getFreePsram() / 1024) + " KB", withLabels);
    infoRow(i++, T("Storage", "Bộ nhớ trống"), String(FFat.freeBytes() / 1048576) + " MB", withLabels);
    infoRow(i++, T("Chip temp", "Nhiệt độ chip"), String(temperatureRead(), 1) + " °C", withLabels);
    infoRow(i++, T("Battery", "Pin"), batt, withLabels);
}

void infoTick() {
    static unsigned long last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    infoRows(false);
}
void infoEnter() {
    header(T("Info", "Thông tin"));
    infoRows(true);
}

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
const int GRAPH_X = 14, GRAPH_W = 212, GRAPH_TOP = 150, GRAPH_H = 156;
int micGraphX = 0;
float micMax = 0;

uint16_t micColor(float db) {
    return db < 60 ? UI_GREEN : db < 80 ? UI_YELLOW : UI_RED;
}
int micScale(float db, int span) {
    return constrain((int)((db - MIC_DB_LO) * span / (MIC_DB_HI - MIC_DB_LO)), 0, span);
}
void micEnter() {
    header(T("Sound meter", "Đo âm thanh"));
    micGraphX = 0;
    micMax = 0;
    uiText(ui15, 86, CONTENT_Y + 12, "dB", UI_SUB);
    uiText(ui12, 156, CONTENT_Y, T("Max", "Tối đa"), UI_SUB);
    for (int g = 60; g <= 80; g += 20) { // faint guide lines at 60 and 80 dB
        int gy = GRAPH_TOP + GRAPH_H - micScale(g, GRAPH_H);
        for (int x = GRAPH_X; x < GRAPH_X + GRAPH_W; x += 4) tft.drawPixel(x, gy, UI_LINE);
    }
}
void micTick() {
    static unsigned long last = 0;
    if (millis() - last < 100) return;
    last = millis();
    if (!micReady) {
        line(CONTENT_Y, T("No microphone", "Không có micro"), UI_RED);
        return;
    }
    float db = micDb;
    micMax = max(micMax, db);
    uint16_t c = micColor(db);
    uiBox(num34, 14, CONTENT_Y - 2, 68, 40, String((int)lroundf(db)), c, UI_BG, AL_RIGHT);
    uiBox(ui15b, 156, CONTENT_Y + 18, 70, 22, String((int)lroundf(micMax)) + " dB", micColor(micMax), UI_BG);
    uiBar(14, CONTENT_Y + 52, 212, 10, micScale(db, 1000) / 1000.0f, c);

    // Sweep the graph like an oscilloscope: one column per update, a blank column ahead of it
    // marks where the trace is, and nothing is ever cleared wholesale.
    int x = GRAPH_X + micGraphX;
    int h = micScale(db, GRAPH_H);
    tft.drawFastVLine(x, GRAPH_TOP, GRAPH_H - h, UI_BG);
    tft.drawFastVLine(x, GRAPH_TOP + GRAPH_H - h, h, c);
    for (int g = 60; g <= 80; g += 20) {
        int gy = GRAPH_TOP + GRAPH_H - micScale(g, GRAPH_H);
        if (gy < GRAPH_TOP + GRAPH_H - h && x % 4 == GRAPH_X % 4) tft.drawPixel(x, gy, UI_LINE);
    }
    micGraphX = (micGraphX + 1) % GRAPH_W;
    tft.drawFastVLine(GRAPH_X + micGraphX, GRAPH_TOP, GRAPH_H, UI_BG);
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

const int REC_CY = 132;
void recDot(bool on) {
    tft.fillCircle(120, REC_CY, 40, on ? UI_RED : rgb565(70, 20, 20));
}
void recDraw() {
    tft.fillRect(0, CONTENT_Y, W, H - CONTENT_Y, UI_BG);
    tft.drawCircle(120, REC_CY, 48, UI_LINE);
    tft.drawCircle(120, REC_CY, 49, UI_LINE);
    recDot(recording);
    uiBox(num34, 0, 188, W, 40, mmss(recording ? (millis() - recStartMs) / 1000 : 0), UI_TXT, UI_BG, AL_CENTER);
    if (recording) {
        uiBox(ui15, 0, 230, W, 24, T("Recording", "Đang ghi"), UI_RED, UI_BG, AL_CENTER);
    } else if (recLastSaved.length()) {
        uiBox(ui15, 0, 230, W, 24, T("Saved", "Đã lưu"), UI_GREEN, UI_BG, AL_CENTER);
        uiBox(ui12, 0, 254, W, 18, recLastSaved, UI_SUB, UI_BG, AL_CENTER);
    } else {
        uiBox(ui15, 0, 230, W, 24, T("Ready", "Sẵn sàng"), UI_SUB, UI_BG, AL_CENTER);
    }
    uiBox(ui12, 0, 296, W, 18, String(recFileCount()) + T(" files · ", " tệp · ") + mmss(FFat.freeBytes() / (MIC_RATE * 2)) + T(" free", " trống"),
          UI_SUB, UI_BG, AL_CENTER);
}
void recEnter() {
    header(T("Recorder", "Ghi âm"));
    recDraw();
}
void recTick() {
    static unsigned long last = 0;
    if (!recording || millis() - last < 200) return;
    last = millis();
    recDot((millis() - recStartMs) % 1000 < 600); // blinking while recording
    uiBox(num34, 0, 188, W, 40, mmss((millis() - recStartMs) / 1000), UI_TXT, UI_BG, AL_CENTER);
    float db = micDb;
    uiBar(40, 274, 160, 8, micScale(db, 1000) / 1000.0f, micColor(db));
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
// Consecutive questions alternate between two files, so a cancelled request that is still
// uploading its recording never reads the file the next question is being recorded into.
const char *ASK_PATHS[2] = {"/ask0.wav", "/ask1.wav"};
uint8_t askSlot = 0;          // which of the two the current recording uses
volatile uint32_t askJobId = 0; // bumped by every send and every cancel; replies to older ids are dropped
bool askWasCancelled = false;
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

// Feeds HTTPClient the request body without holding it in RAM: a prefix, the WAV file (base64-
// encoded on the fly for JSON, or as-is for a multipart upload), then a suffix.
class FileBody : public Stream {
public:
    FileBody(const String &prefix, File file, const String &suffix, bool base64 = true)
        : _prefix(prefix), _suffix(suffix), _file(file), _base64(base64) {}
    static size_t base64Length(size_t raw) { return 4 * ((raw + 2) / 3); }
    size_t fileLength() const { return _base64 ? base64Length(_file.size()) : _file.size(); }
    size_t total() const { return _prefix.length() + fileLength() + _suffix.length(); }
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
            } else if (!_base64 && _file.available()) {
                size_t n = _file.read(out + done, len - done);
                done += n;
                _sent += n;
            } else if (_file.available()) {
                uint8_t raw[300]; // a multiple of 3, so no padding appears mid-stream
                size_t n = _file.read(raw, sizeof(raw));
                _b64Pos = 0;
                mbedtls_base64_encode(_b64, sizeof(_b64), &_b64Len, raw, n);
            } else {
                size_t at = _sent - _prefix.length() - fileLength();
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
    bool _base64;
    size_t _sent = 0, _b64Len = 0, _b64Pos = 0;
    uint8_t _b64[404];
};

// Two providers: Gemini hears the WAV itself; Groq turns it into text with Whisper and then a
// chat model answers. Each keeps its own key and model in its own NVS namespace, and "ask" holds
// which one is in use. A request reads these into askJob first, in the loop task, because the
// shared Preferences object must not be used from the request task.
enum AskProvider : uint8_t { ASK_GEMINI, ASK_GROQ, ASK_PROVIDER_COUNT };
const char *ASK_PROVIDER_IDS[] = {"gemini", "groq"};
const char *ASK_PROVIDER_NAMES[] = {"Gemini", "Groq"};
const char *GROQ_DEFAULT_MODEL = "openai/gpt-oss-120b";
const char *GROQ_FALLBACK_MODEL = "qwen/qwen3.8-27b";
const char *GROQ_STT_MODEL = "whisper-large-v3-turbo";
const char *GROQ_STT_FALLBACK = "whisper-large-v3";
const char *ASK_TEXT_SYSTEM =
    "Bạn là trợ lý trên một thiết bị nhỏ có màn hình 240x320. Câu hỏi của người dùng được chuyển từ giọng nói "
    "thành chữ nên có thể sai chính tả, hãy hiểu theo ý gần nhất. Trả lời bằng tiếng Việt có dấu, ngắn gọn "
    "(không quá khoảng 120 từ), văn bản thuần, không dùng markdown. Nếu người dùng hỏi bằng ngôn ngữ khác thì "
    "trả lời bằng ngôn ngữ đó.";

AskProvider askProvider() {
    prefs.begin("ask", true);
    String id = prefs.getString("provider", "gemini");
    prefs.end();
    return id == "groq" ? ASK_GROQ : ASK_GEMINI;
}
void askSetProvider(AskProvider p) {
    prefs.begin("ask", false);
    prefs.putString("provider", ASK_PROVIDER_IDS[p]);
    prefs.end();
}
String askKey(AskProvider p) {
    prefs.begin(ASK_PROVIDER_IDS[p], true);
    String k = prefs.getString("key", "");
    prefs.end();
    return k;
}
String askModel(AskProvider p) {
    const char *def = p == ASK_GROQ ? GROQ_DEFAULT_MODEL : ASK_DEFAULT_MODEL;
    prefs.begin(ASK_PROVIDER_IDS[p], true);
    String m = prefs.getString("model", def);
    prefs.end();
    return m.length() ? m : String(def);
}
String askKey() { return askKey(askProvider()); }

// Everything a request needs, copied for its task: a cancelled request may still be running
// when the next one starts.
struct AskJob {
    AskProvider provider;
    String key, model;
    const char *path;
    uint32_t id;
    uint8_t lang; // screen language: the answer and the transcription use it
};

// The instructions for the model, in the screen language. `fromAudio`: Gemini gets the recording
// and must also write down what it heard; Groq gets Whisper's text.
String askSystemPrompt(bool fromAudio, uint8_t lang) {
    if (lang == LANG_VI) return fromAudio ? String(ASK_SYSTEM) : String(ASK_TEXT_SYSTEM);
    return fromAudio
        ? "You are a voice assistant on a small device with a 240x320 screen. The user asks by voice in the "
          "recording. Write down exactly what the user said in the question field, then answer in the answer "
          "field in English, briefly (about 120 words at most), plain text, no markdown. If the user speaks "
          "another language, answer in that language. If you cannot hear clearly, say so in answer."
        : "You are an assistant on a small device with a 240x320 screen. The question was transcribed from "
          "speech and may contain mistakes; go with the closest meaning. Answer in English, briefly (about "
          "120 words at most), plain text, no markdown. If the user speaks another language, answer in that language.";
}
struct AskResult {
    bool ok = false;
    String question, answer, error;
    void fail(const String &why) { error = why; }
};

void askFail(const String &why) {
    askError = why;
    askState = AskState::Failed;
    Serial.println("Ask failed: " + why);
}

// One HTTPS POST. Returns the HTTP status (<= 0 when the connection failed) and the reply body.
int httpsPost(const String &url, const String &contentType, const char *authName, const String &authValue,
              Stream *body, size_t length, String &payload) {
    WiFiClientSecure secure;
    secure.setInsecure(); // no root CA bundle on the board; the key still travels over TLS
    HTTPClient http;
    http.setTimeout(60000);
    if (!http.begin(secure, url)) return -101;
    http.addHeader("Content-Type", contentType);
    http.addHeader(authName, authValue);
    int status = http.sendRequest("POST", body, length);
    payload = http.getString();
    http.end();
    Serial.printf("Ask POST %s -> HTTP %d after %lu ms, %u bytes\n", url.c_str(), status, millis() - askSentMs, payload.length());
    return status;
}

String apiError(int status, const String &payload) {
    if (status == -100) return "Recording missing";
    if (status <= 0) return "Connection error " + String(status);
    JsonDocument doc;
    const char *msg = deserializeJson(doc, payload) ? nullptr : (const char *)doc["error"]["message"];
    return "HTTP " + String(status) + ": " + String(msg ? msg : payload.substring(0, 120).c_str());
}

// Gemini: one generateContent call with the recording inline.
int geminiRequest(const AskJob &job, const String &model, String &payload) {
    File wav = FFat.open(job.path, FILE_READ);
    if (!wav) return -100;
    String prefix = String("{\"system_instruction\":{\"parts\":[{\"text\":\"") + jsonEscape(askSystemPrompt(true, job.lang)) + "\"}]}," +
        "\"contents\":[{\"role\":\"user\",\"parts\":[{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
    String suffix = "\"}}]}],\"generationConfig\":{\"responseMimeType\":\"application/json\",\"responseSchema\":"
        "{\"type\":\"OBJECT\",\"properties\":{\"question\":{\"type\":\"STRING\"},\"answer\":{\"type\":\"STRING\"}},"
        "\"required\":[\"question\",\"answer\"]}}}";
    FileBody body(prefix, wav, suffix);
    Serial.printf("Ask Gemini %s: %u byte WAV\n", model.c_str(), wav.size());
    int status = httpsPost("https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent",
        "application/json", "x-goog-api-key", job.key, &body, body.total(), payload);
    wav.close();
    return status;
}

void askGemini(const AskJob &job, AskResult &r) {
    String payload;
    int status = geminiRequest(job, job.model, payload);
    if ((status == 503 || status == 429) && job.model != ASK_FALLBACK_MODEL) {
        status = geminiRequest(job, ASK_FALLBACK_MODEL, payload); // demand spikes are per model
    }
    if (status != 200) return r.fail(apiError(status, payload));
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return r.fail("HTTP 200: unreadable reply");
    String text;
    for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
        if (!(part["thought"] | false)) text += (const char *)(part["text"] | "");
    }
    JsonDocument reply;
    if (text.isEmpty()) {
        const char *why = doc["promptFeedback"]["blockReason"] | doc["candidates"][0]["finishReason"] | "empty";
        r.fail(String("No answer (") + why + ")");
    } else if (deserializeJson(reply, text) || !reply["answer"].is<const char *>()) {
        r.answer = text; // not the JSON asked for, but still worth showing
        r.ok = true;
    } else {
        r.question = (const char *)(reply["question"] | "");
        r.answer = (const char *)reply["answer"];
        r.ok = true;
    }
}

// Groq step 1: Whisper transcription, the WAV sent as a multipart file upload.
int groqTranscribe(const AskJob &job, const char *model, String &payload) {
    File wav = FFat.open(job.path, FILE_READ);
    if (!wav) return -100;
    const String B = "esp32askboundary";
    String prefix = "--" + B + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + model + "\r\n" +
        "--" + B + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n" +
        // Whisper's language detection is unreliable on a few seconds of speech (a Vietnamese
        // question came back as Japanese), so it is told the screen language.
        "--" + B + "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n" + (job.lang == LANG_VI ? "vi" : "en") + "\r\n" +
        "--" + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"ask.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    FileBody body(prefix, wav, "\r\n--" + B + "--\r\n", false);
    Serial.printf("Ask Groq %s: %u byte WAV\n", model, wav.size());
    int status = httpsPost("https://api.groq.com/openai/v1/audio/transcriptions", "multipart/form-data; boundary=" + B,
        "Authorization", "Bearer " + job.key, &body, body.total(), payload);
    wav.close();
    return status;
}

// Groq step 2: a chat completion answering the transcribed question.
int groqChat(const AskJob &job, const String &model, const String &question, String &payload) {
    JsonDocument req;
    req["model"] = model;
    req["messages"][0]["role"] = "system";
    req["messages"][0]["content"] = askSystemPrompt(false, job.lang);
    req["messages"][1]["role"] = "user";
    req["messages"][1]["content"] = question;
    if (model.startsWith("openai/gpt-oss")) req["reasoning_effort"] = "low"; // reasoning models: keep it quick
    else if (model.startsWith("qwen/")) req["reasoning_format"] = "hidden";
    String body;
    serializeJson(req, body);
    StreamString stream;
    stream.print(body);
    return httpsPost("https://api.groq.com/openai/v1/chat/completions", "application/json", "Authorization",
        "Bearer " + job.key, &stream, body.length(), payload);
}

void askGroq(const AskJob &job, AskResult &r) {
    String payload;
    int status = groqTranscribe(job, GROQ_STT_MODEL, payload);
    if (status == 404 || status == 400) status = groqTranscribe(job, GROQ_STT_FALLBACK, payload); // model retired
    if (status != 200) return r.fail(apiError(status, payload));
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return r.fail("Transcription unreadable");
    String question = (const char *)(doc["text"] | "");
    question.trim();
    if (question.isEmpty()) return r.fail(T("Nothing heard", "Không nghe thấy gì"));
    r.question = question;

    status = groqChat(job, job.model, question, payload);
    if ((status == 503 || status == 429 || status == 404 || status == 400) && job.model != GROQ_FALLBACK_MODEL) {
        status = groqChat(job, GROQ_FALLBACK_MODEL, question, payload);
    }
    if (status != 200) return r.fail(apiError(status, payload));
    doc.clear();
    if (deserializeJson(doc, payload)) return r.fail("HTTP 200: unreadable reply");
    String answer = (const char *)(doc["choices"][0]["message"]["content"] | "");
    int think = answer.indexOf("</think>"); // a reasoning model that ignored reasoning_format
    if (think >= 0) answer = answer.substring(think + 8);
    answer.trim();
    if (answer.isEmpty()) return r.fail(T("No answer", "Không có câu trả lời"));
    r.answer = answer;
    r.ok = true;
}

void askTask(void *arg) {
    AskJob job = *(AskJob *)arg;
    delete (AskJob *)arg;
    AskResult r;
    if (job.provider == ASK_GROQ) askGroq(job, r);
    else askGemini(job, r);
    if (job.id != askJobId) { // cancelled, or a newer question has been sent since
        Serial.printf("Ask: reply to cancelled question %u dropped\n", job.id);
    } else {
        askTookMs = millis() - askSentMs;
        askQuestion = r.question;
        askAnswer = r.answer;
        if (r.ok) askState = AskState::Answer;
        else askFail(r.error);
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
const int ASK_TOP = CONTENT_Y, ASK_LINE_H = 22, ASK_ROWS = (320 - ASK_TOP) / ASK_LINE_H, ASK_TEXT_W = 212;

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
            if (cur.length() && aaWidth(ui15, next) > ASK_TEXT_W) {
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
        int i = askScroll + r;
        bool has = i < (int)askLines.size();
        uiBox(ui15, 14, ASK_TOP + r * ASK_LINE_H, ASK_TEXT_W + 4, ASK_LINE_H, has ? askLines[i].text : String(),
              has ? askLines[i].color : UI_TXT, UI_BG);
    }
    int total = askLines.size();
    tft.fillRect(233, ASK_TOP, 4, ASK_ROWS * ASK_LINE_H, UI_BG);
    if (total > ASK_ROWS) { // scrollbar
        int h = ASK_ROWS * ASK_LINE_H, thumb = max(16, h * ASK_ROWS / total);
        int y = ASK_TOP + (h - thumb) * askScroll / (total - ASK_ROWS);
        tft.fillRoundRect(233, y, 3, thumb, 1, rgb565(90, 96, 110));
    }
}

// Provider logos for the Ready screen, drawn from shapes like the menu icons.

// Gemini: a four-point sparkle with concave sides, |x|^e + |y|^e <= r^e with e < 1, shaded from
// blue at the top left through purple to pink at the bottom right. Drawn a row at a time.
void drawGeminiLogo(int cx, int cy, int r) {
    const float e = 0.72f, re = powf(r, e);
    const uint8_t stops[3][3] = {{60, 130, 245}, {150, 110, 235}, {230, 110, 150}};
    static uint16_t row[2 * 60 + 1];
    for (int dy = -r; dy <= r; dy++) {
        float rem = re - powf(abs(dy), e);
        if (rem <= 0) continue;
        int w = (int)powf(rem, 1 / e);
        if (w < 1 && abs(dy) > r - 3) continue; // no one-pixel spikes at the tips
        for (int x = -w; x <= w; x++) {
            float t = constrain((x + dy) / (2.0f * r) + 0.5f, 0.0f, 1.0f);
            const uint8_t *a = stops[t < 0.5f ? 0 : 1], *b = stops[t < 0.5f ? 1 : 2];
            float u = t < 0.5f ? t * 2 : (t - 0.5f) * 2;
            row[x + w] = tft.color565(a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u);
        }
        tft.drawRGBBitmap(cx - w, cy + dy, row, 2 * w + 1, 1);
    }
}

// Rows `from`..`to` (relative to cy) of a ring, as horizontal spans.
void fillRingRows(int cx, int cy, int rin, int rout, int from, int to, uint16_t color) {
    for (int dy = from; dy <= to; dy++) {
        if (abs(dy) > rout) continue;
        int wo = (int)sqrtf(rout * rout - dy * dy);
        if (abs(dy) < rin) {
            int wi = (int)ceilf(sqrtf(rin * rin - dy * dy));
            tft.drawFastHLine(cx - wo, cy + dy, wo - wi + 1, color);
            tft.drawFastHLine(cx + wi, cy + dy, wo - wi + 1, color);
        } else {
            tft.drawFastHLine(cx - wo, cy + dy, 2 * wo + 1, color);
        }
    }
}

// Groq: a white lowercase "g" (ring bowl, right stem, bottom hook) on an orange rounded square.
void drawGroqLogo(int cx, int cy) {
    const uint16_t orange = tft.color565(245, 80, 54);
    const int ro = 16, ri = 9, gx = cx - 3, bowlY = cy - 8, hookY = cy + 12;
    tft.fillRoundRect(cx - 42, cy - 42, 84, 84, 20, orange);
    fillRingRows(gx, bowlY, ri, ro, -ro, ro, UI_TXT);
    tft.fillRect(gx + ri, bowlY - ro + 2, ro - ri + 1, hookY - (bowlY - ro + 2) + 1, UI_TXT);
    fillRingRows(gx, hookY, ri, ro, 1, ro, UI_TXT);
}

// A soft glow behind the provider logo: discs from large to small, each a little closer to `c`.
void uiGlow(int cx, int cy, int r, uint8_t cr, uint8_t cg, uint8_t cb) {
    for (int k = r; k > 0; k -= 2) {
        float a = 1.0f - (float)k / r;
        a = a * a;
        tft.fillCircle(cx, cy, k, rgb565(cr * a, cg * a, cb * a));
    }
}

const int ASK_LOGO_Y = 140;
void askDraw() {
    header(T("Ask AI", "Hỏi AI"));
    switch (askState) {
        case AskState::Idle: {
            AskProvider pv = askProvider();
            if (pv == ASK_GROQ) {
                uiGlow(120, ASK_LOGO_Y, 72, 110, 40, 20);
                drawGroqLogo(120, ASK_LOGO_Y);
            } else {
                uiGlow(120, ASK_LOGO_Y, 72, 60, 45, 140);
                drawGeminiLogo(120, ASK_LOGO_Y, 44);
            }
            uiBox(ui15b, 0, 214, W, 24, ASK_PROVIDER_NAMES[pv], UI_TXT, UI_BG, AL_CENTER);
            if (askKey().isEmpty()) uiBox(ui12, 0, 238, W, 20, T("No API key", "Chưa có khóa API"), UI_RED, UI_BG, AL_CENTER);
            else if (askWasCancelled) uiBox(ui12, 0, 238, W, 20, T("Cancelled", "Đã hủy"), UI_YELLOW, UI_BG, AL_CENTER);
            else uiBox(ui12, 0, 238, W, 20, T("Ready", "Sẵn sàng"), UI_SUB, UI_BG, AL_CENTER);
            int x0 = 120 - (ASK_PROVIDER_COUNT - 1) * 6; // a dot per provider, the one in use lit
            for (int i = 0; i < ASK_PROVIDER_COUNT; i++) tft.fillCircle(x0 + i * 12, 280, 3, i == pv ? UI_TXT : UI_LINE);
            break;
        }
        case AskState::Recording:
            tft.drawCircle(120, ASK_LOGO_Y, 48, UI_LINE);
            tft.drawCircle(120, ASK_LOGO_Y, 49, UI_LINE);
            uiBox(ui15, 0, 236, W, 24, T("Listening", "Đang nghe"), UI_RED, UI_BG, AL_CENTER);
            break;
        case AskState::Thinking:
            uiBox(ui15b, 0, 180, W, 24, T("Thinking", "Đang nghĩ"), UI_TXT, UI_BG, AL_CENTER);
            break;
        case AskState::Answer:
            askLines.clear();
            askScroll = 0;
            if (askQuestion.length()) {
                askWrap(askQuestion, ACCENT);
                askLines.push_back({"", UI_TXT});
            }
            askWrap(askAnswer, UI_TXT);
            askDrawText();
            break;
        case AskState::Failed:
            uiBox(ui15b, 14, CONTENT_Y, 200, 24, T("Error", "Lỗi"), UI_RED, UI_BG);
            askLines.clear();
            askWrap(askError, UI_TXT2);
            for (int i = 0; i < (int)askLines.size() && i < 10; i++) {
                uiBox(ui15, 14, CONTENT_Y + 30 + i * ASK_LINE_H, ASK_TEXT_W, ASK_LINE_H, askLines[i].text, UI_TXT2, UI_BG);
            }
            break;
    }
}

AskState askShown = AskState::Idle; // what askDraw last drew, so the tick sees task updates
// Opening the app starts on the Ready screen, where the knob picks the provider. The one exception
// is an answer that arrived while the app was closed (askShown is still Thinking): that is shown.
void askEnter() {
    if (askState == AskState::Recording && !recording) askState = AskState::Idle; // left mid-question
    bool unseen = askShown == AskState::Thinking;
    askWasCancelled = false;
    if ((askState == AskState::Answer || askState == AskState::Failed) && !unseen) askState = AskState::Idle;
    askShown = askState;
    askDraw();
}

void askSend() {
    recStop();
    if (recSamples < MIC_RATE / 2) {
        askFail(T("Too short", "Quá ngắn"));
    } else {
        askError = "";
        AskProvider pv = askProvider();
        AskJob *job = new AskJob{pv, askKey(pv), askModel(pv), ASK_PATHS[askSlot], ++askJobId, uiLang};
        askState = AskState::Thinking;
        askSentMs = millis();
        xTaskCreatePinnedToCore(askTask, "ask", 12288, job, 1, nullptr, 1);
    }
}

// A press on a long answer pages down first (one line of overlap, so reading stays continuous);
// only on the last page does it start the next question. This is how a board with just BOOT and
// the back button reads a long answer.
// Back while listening or waiting: the recording is dropped, or the reply on its way is ignored,
// and the app returns to Ready instead of leaving.
void askCancel() {
    if (askState == AskState::Recording) recStop();
    askJobId++;
    askState = AskState::Idle;
    askWasCancelled = true;
    askShown = askState;
    askDraw();
    Serial.println("Ask cancelled");
}

void askPress() {
    if (askState == AskState::Thinking) return;
    askWasCancelled = false;
    int maxScroll = max(0, (int)askLines.size() - ASK_ROWS);
    if (askState == AskState::Answer && askScroll < maxScroll) {
        askScroll = min(askScroll + ASK_ROWS - 1, maxScroll);
        askDrawText();
        return;
    }
    if (askState == AskState::Recording) {
        askSend();
    } else if (askKey().isEmpty()) {
        askFail(T("No API key. Add it on the web page.", "Chưa có khóa API. Thêm trên trang web."));
    } else if (recStart(ASK_PATHS[askSlot ^ 1])) {
        askSlot ^= 1;
        askState = AskState::Recording;
    } else {
        askFail(T("Cannot record", "Không ghi âm được"));
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
        tft.fillCircle(120, ASK_LOGO_Y, 40, on ? UI_RED : rgb565(70, 20, 20));
        uiBox(num34, 0, 192, W, 40, mmss(sec), UI_TXT, UI_BG, AL_CENTER);
        float db = micDb;
        uiBar(40, 270, 160, 8, micScale(db, 1000) / 1000.0f, micColor(db));
    } else if (askState == AskState::Thinking) {
        int phase = (millis() - askSentMs) / 300 % 3; // three dots, one lit at a time
        for (int i = 0; i < 3; i++) tft.fillCircle(100 + i * 20, ASK_LOGO_Y, 6, i == phase ? ACCENT : UI_LINE);
        uiBox(ui12, 0, 206, W, 20, String((millis() - askSentMs) / 1000) + " s", UI_SUB, UI_BG, AL_CENTER);
    }
}

void askRotate(int dir) {
    if (askState == AskState::Idle || askState == AskState::Failed) { // pick the provider
        askSetProvider((AskProvider)((askProvider() + (dir > 0 ? 1 : ASK_PROVIDER_COUNT - 1)) % ASK_PROVIDER_COUNT));
        askState = AskState::Idle;
        askWasCancelled = false;
        askShown = askState;
        askDraw();
        return;
    }
    if (askState != AskState::Answer) return;
    int maxScroll = max(0, (int)askLines.size() - ASK_ROWS);
    int next = constrain(askScroll + dir, 0, maxScroll);
    if (next == askScroll) return;
    askScroll = next;
    askDrawText();
}

// ---------- iPhone notifications ----------
// Received over Bluetooth (include/ancs_client.h). The newest 20 are kept; a new one pops up as
// a banner at the top of whatever is on screen for a few seconds.
const char *BLE_NAME = "ESP32 Device";
struct Note {
    AncsNote n;
    unsigned long ms;
};
std::vector<Note> notes; // newest first
int notesSel = 0, notesTop = 0;
bool notesDetail = false;
unsigned long bannerUntilMs = 0;
int unreadNotes = 0;

String appLabel(const String &bundle) { // a friendly name for the apps people get messages from
    static const char *MAP[][2] = {{"vn.com.vng.zingalo", "Zalo"}, {"com.vng.zalo", "Zalo"}, {"com.facebook.Messenger", "Messenger"},
        {"com.apple.MobileSMS", "Tin nhắn"}, {"com.apple.mobilephone", "Điện thoại"}, {"com.google.Gmail", "Gmail"},
        {"com.apple.mobilemail", "Mail"}, {"net.whatsapp.WhatsApp", "WhatsApp"}, {"ph.telegra.Telegraph", "Telegram"},
        {"com.facebook.Facebook", "Facebook"}, {"com.burbn.instagram", "Instagram"}, {"com.tinyspeck.chatlyio", "Slack"},
        {"com.apple.reminders", "Lời nhắc"}, {"com.apple.mobilecal", "Lịch"}, {"com.google.ios.youtube", "YouTube"}};
    for (auto &m : MAP) {
        if (bundle == m[0]) return uiLang == LANG_VI ? m[1] : (strcmp(m[0], "com.apple.MobileSMS") == 0 ? "Messages"
            : strcmp(m[0], "com.apple.mobilephone") == 0 ? "Phone" : strcmp(m[0], "com.apple.reminders") == 0 ? "Reminders"
            : strcmp(m[0], "com.apple.mobilecal") == 0 ? "Calendar" : m[1]);
    }
    int dot = bundle.lastIndexOf('.');
    return dot >= 0 ? bundle.substring(dot + 1) : bundle;
}

String noteAgo(unsigned long ms) {
    unsigned long s = (millis() - ms) / 1000;
    if (s < 60) return T("now", "vừa xong");
    if (s < 3600) return String(s / 60) + T(" min", " phút");
    return String(s / 3600) + T(" h", " giờ");
}

String oneLine(String s) {
    s.replace("\n", " ");
    s.replace("\r", " ");
    return s;
}

const int NOTE_ROW_H = 66, NOTE_ROWS = 3;
void notesDrawRow(int i) {
    if (i < notesTop || i >= notesTop + NOTE_ROWS) return;
    int y = CONTENT_Y + 22 + (i - notesTop) * NOTE_ROW_H;
    if (i >= (int)notes.size()) {
        tft.fillRect(0, y, W, NOTE_ROW_H, UI_BG);
        return;
    }
    const AncsNote &n = notes[i].n;
    bool sel = i == notesSel;
    uint16_t bg = sel ? UI_CARD_SEL : UI_CARD;
    tft.fillRect(0, y, W, NOTE_ROW_H, UI_BG);
    tft.fillRoundRect(8, y, 224, NOTE_ROW_H - 4, 12, bg);
    uiBox(ui12, 18, y + 2, 140, 21, appLabel(n.app), n.category == 1 ? UI_GREEN : ACCENT, bg);
    uiBox(ui12, 158, y + 2, 64, 21, noteAgo(notes[i].ms), UI_SUB, bg, AL_RIGHT);
    uiBox(ui15b, 18, y + 20, 204, 23, oneLine(n.title), UI_TXT, bg);
    uiBox(ui12, 18, y + 41, 204, 20, oneLine(n.message), UI_TXT2, bg);
}

void notesDrawStatus() {
    bool ok = ancs::ready;
    String s = ok ? String(T("iPhone connected", "iPhone đã kết nối"))
             : ancs::connected ? String(T("Pairing...", "Đang ghép đôi..."))
             : String(T("Not connected · ", "Chưa kết nối · ")) + BLE_NAME;
    tft.fillCircle(18, CONTENT_Y + 8, 4, ok ? UI_GREEN : ancs::connected ? UI_YELLOW : UI_SUB);
    uiBox(ui12, 28, CONTENT_Y - 1, 200, 18, s, UI_SUB, UI_BG);
}

void notesDraw() {
    header(T("Notifications", "Thông báo"));
    notesDrawStatus();
    if (notesDetail && notesSel < (int)notes.size()) { // one notification in full
        const AncsNote &n = notes[notesSel].n;
        uiBox(ui12, 14, CONTENT_Y + 22, 212, 18, appLabel(n.app) + " · " + noteAgo(notes[notesSel].ms), ACCENT, UI_BG);
        askLines.clear();
        askWrap(n.title, UI_TXT);
        askWrap(n.message, UI_TXT2);
        for (int i = 0; i < (int)askLines.size() && i < 10; i++) {
            uiBox(i == 0 ? ui15b : ui15, 14, CONTENT_Y + 42 + i * 22, 212, 22, askLines[i].text, askLines[i].color, UI_BG);
        }
        return;
    }
    if (notes.empty()) {
        line(CONTENT_Y + 30, T("No notifications", "Chưa có thông báo"), UI_SUB);
        return;
    }
    for (int i = notesTop; i < notesTop + NOTE_ROWS; i++) notesDrawRow(i);
}

void notesEnter() {
    notesDetail = false;
    unreadNotes = 0;
    notesSel = constrain(notesSel, 0, max(0, (int)notes.size() - 1));
    notesDraw();
}
void notesPress() { // OK opens the selected one in full, or goes back to the list
    if (notes.empty()) return;
    notesDetail = !notesDetail;
    notesDraw();
}
void notesRotate(int dir) {
    if (notes.empty() || notesDetail) return;
    int prev = notesSel;
    notesSel = constrain(notesSel + dir, 0, (int)notes.size() - 1);
    if (notesSel == prev) return;
    if (notesSel < notesTop || notesSel >= notesTop + NOTE_ROWS) {
        notesTop = constrain(notesSel - (dir > 0 ? NOTE_ROWS - 1 : 0), 0, max(0, (int)notes.size() - NOTE_ROWS));
        for (int i = notesTop; i < notesTop + NOTE_ROWS; i++) notesDrawRow(i);
    } else {
        notesDrawRow(prev);
        notesDrawRow(notesSel);
    }
}
void notesTick() {
    static bool lastReady = false, lastConn = false;
    if (ancs::ready != lastReady || ancs::connected != lastConn) {
        lastReady = ancs::ready;
        lastConn = ancs::connected;
        notesDrawStatus();
    }
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
// Minutes and seconds in fixed boxes either side of the colon, so the digits don't shift as they
// change; the field being set is blue and underlined. A bar shows the time left while running.
const int TIMER_Y = 92, TIMER_H = 70;
void timerDraw() {
    bool setting = timerState == TimerState::Set;
    const char *label = setting ? (timerField == 0 ? T("Set minutes", "Chỉnh phút") : T("Set seconds", "Chỉnh giây"))
                      : timerState == TimerState::Running ? T("Running", "Đang chạy")
                      : timerState == TimerState::Paused ? T("Paused", "Tạm dừng") : T("Time's up!", "Hết giờ!");
    uint16_t base = timerState == TimerState::Done ? UI_RED : UI_TXT;
    uint16_t labelColor = timerState == TimerState::Running ? UI_GREEN : timerState == TimerState::Paused ? UI_YELLOW
                        : timerState == TimerState::Done ? UI_RED : UI_SUB;
    String t = timerText();
    uiBox(num68, 8, TIMER_Y, 98, TIMER_H, t.substring(0, 2), setting && timerField == 0 ? ACCENT : base, UI_BG, AL_RIGHT);
    uiBox(num68, 106, TIMER_Y, 28, TIMER_H, ":", base, UI_BG, AL_CENTER);
    uiBox(num68, 134, TIMER_Y, 98, TIMER_H, t.substring(3, 5), setting && timerField == 1 ? ACCENT : base, UI_BG, AL_LEFT);
    tft.fillRoundRect(28, TIMER_Y + TIMER_H + 4, 72, 4, 2, setting && timerField == 0 ? ACCENT : UI_BG);
    tft.fillRoundRect(140, TIMER_Y + TIMER_H + 4, 72, 4, 2, setting && timerField == 1 ? ACCENT : UI_BG);
    uiBox(ui15, 0, 196, W, 24, label, labelColor, UI_BG, AL_CENTER);
    if (setting || timerSetMs() == 0) tft.fillRect(30, 238, 180, 8, UI_BG);
    else uiBar(30, 238, 180, 8, (float)timerRemainingMs() / timerSetMs(), labelColor);
}

void timerEnter() {
    header(T("Timer", "Hẹn giờ"));
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
const char *WEATHER_CITY = "Hà Nội";

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
        case WX_SUN: return T("Clear", "Trời quang");
        case WX_PARTLY: return T("Partly cloudy", "Ít mây");
        case WX_RAIN: return code >= 80 ? T("Showers", "Mưa rào") : code <= 57 ? T("Drizzle", "Mưa phùn") : T("Rain", "Mưa");
        case WX_STORM: return T("Thunderstorm", "Dông");
        default: return code >= 45 && code <= 48 ? T("Fog", "Sương mù") : T("Cloudy", "Nhiều mây");
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
        weather.error = T("No connection", "Mất kết nối");
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
    header(T("Weather", "Thời tiết"));
    if (!weather.ok) {
        line(CONTENT_Y, T("Unavailable", "Không có dữ liệu"), UI_RED);
        line(CONTENT_Y + 26, weather.error, UI_SUB, 1);
        return;
    }
    uiText(ui12, 14, CONTENT_Y - 6, WEATHER_CITY, UI_SUB);
    drawWeatherIcon(weatherKind(weather.code), 62, 112, 2);
    uiBox(num34, 112, 88, 114, 40, String(lround(weather.temp)) + "°", UI_TXT, UI_BG);
    uiBox(ui15b, 112, 128, 114, 22, weatherText(weather.code), UI_TXT, UI_BG);
    uiBox(ui12, 14, 160, W - 28, 18, T("Humidity ", "Độ ẩm ") + String(weather.humidity) + "%   ·   " + T("Wind ", "Gió ") +
          String(lround(weather.wind)) + " km/h", UI_SUB, UI_BG);
    const int cy = 190;
    tft.fillRoundRect(8, cy, 224, 120, 14, UI_CARD);
    for (int i = 0; i < 4; i++) {
        int cx = 8 + 28 + i * 56;
        uiBox(ui12, cx - 26, cy + 8, 52, 18, String(weather.hourH[i]) + "h", UI_SUB, UI_CARD, AL_CENTER);
        drawWeatherIcon(weatherKind(weather.hourCode[i]), cx, cy + 56, 1);
        uiBox(ui15b, cx - 26, cy + 88, 52, 22, String(weather.hourTemp[i]) + "°", UI_TXT, UI_CARD, AL_CENTER);
    }
}

void weatherEnter() {
    if (!weather.ok || millis() - weather.fetchedMs > 10 * 60000UL) { // refresh at most every 10 minutes
        header(T("Weather", "Thời tiết"));
        line(CONTENT_Y, T("Loading...", "Đang tải..."), UI_SUB);
        weatherFetch();
    }
    weatherDraw();
}

void weatherPress() { // a press forces a refresh
    header(T("Weather", "Thời tiết"));
    line(CONTENT_Y, T("Loading...", "Đang tải..."), UI_SUB);
    weatherFetch();
    weatherDraw();
}

// ---------- markets ----------
// Gold (vang.today: SJC bars and rings, world spot), USD/VND (Vietcombank), VN-Index (SSI) and
// Bitcoin (CoinGecko): all free and keyless. Fetched in a background task so the screen and
// buttons stay live; refreshed at most every 5 minutes (Vietcombank asks for no more than that),
// or at once with OK.
struct Quote {
    bool ok = false;
    double value = 0, value2 = 0, change = 0, changePct = 0; // value2: a second price (buy, or VND)
};
struct Markets {
    Quote goldBar, goldRing, goldWorld, usd, vnindex, btc;
    unsigned long fetchedMs = 0;
    String error;
} markets;
volatile bool marketsFetching = false, marketsChanged = false;

// Number with grouped thousands, in the screen language's convention (1.234,5 or 1,234.5).
String fmtNum(double v, int decimals) {
    char raw[32];
    snprintf(raw, sizeof(raw), "%.*f", decimals, fabs(v));
    String s = raw, frac;
    int dot = s.indexOf('.');
    if (dot >= 0) {
        frac = s.substring(dot + 1);
        s = s.substring(0, dot);
    }
    bool vi = uiLang == LANG_VI;
    String out;
    for (int i = 0; i < (int)s.length(); i++) {
        if (i && (s.length() - i) % 3 == 0) out += vi ? '.' : ',';
        out += s[i];
    }
    if (frac.length()) out += String(vi ? ',' : '.') + frac;
    return (v < 0 ? "-" : "") + out;
}

bool httpsGet(const char *url, String &body) {
    WiFiClientSecure secure;
    secure.setInsecure(); // public data; no root CA bundle on the board
    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(secure, url)) return false;
    http.addHeader("User-Agent", "Mozilla/5.0 (ESP32)");
    int status = http.GET();
    body = status == 200 ? http.getString() : String();
    http.end();
    if (status != 200) Serial.printf("GET %s -> %d\n", url, status);
    return status == 200;
}

// "25,760.00" -> 25760
double parseGrouped(const String &s) {
    String digits;
    for (char c : s) {
        if (isdigit((unsigned char)c) || c == '.') digits += c;
    }
    return digits.toDouble();
}

void marketsTask(void *) {
    Markets m;
    String body;
    JsonDocument doc;
    if (httpsGet("https://www.vang.today/api/prices", body) && !deserializeJson(doc, body)) {
        auto take = [&](const char *key, Quote &q) {
            JsonObject p = doc["prices"][key];
            if (p.isNull()) return;
            q.value = p["sell"] | 0.0;
            q.value2 = p["buy"] | 0.0;
            q.change = p["change_sell"] | 0.0;
            q.ok = q.value > 0;
        };
        take("SJL1L10", m.goldBar);
        take("SJ9999", m.goldRing);
        JsonObject w = doc["prices"]["XAUUSD"];
        m.goldWorld.value = w["buy"] | 0.0;
        m.goldWorld.ok = m.goldWorld.value > 0;
    }
    if (httpsGet("https://portal.vietcombank.com.vn/Usercontrols/TVPortal.TyGia/pXML.aspx", body)) {
        int at = body.indexOf("CurrencyCode=\"USD\"");
        auto attr = [&](const char *name) {
            int a = body.indexOf(String(name) + "=\"", at);
            if (a < 0) return 0.0;
            a += strlen(name) + 2;
            return parseGrouped(body.substring(a, body.indexOf('"', a)));
        };
        if (at >= 0) {
            m.usd.value = attr("Sell");
            m.usd.value2 = attr("Buy");
            m.usd.ok = m.usd.value > 0;
        }
    }
    doc.clear();
    if (httpsGet("https://iboard-query.ssi.com.vn/exchange-index/VNINDEX?hasVN30=true", body) && !deserializeJson(doc, body)) {
        JsonObject d = doc["data"];
        m.vnindex.value = d["indexValue"] | 0.0;
        m.vnindex.change = d["change"] | 0.0;
        m.vnindex.changePct = d["changePercent"] | 0.0;
        m.vnindex.ok = m.vnindex.value > 0;
    }
    doc.clear();
    if (httpsGet("https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd,vnd&include_24hr_change=true", body) &&
        !deserializeJson(doc, body)) {
        JsonObject b = doc["bitcoin"];
        m.btc.value = b["usd"] | 0.0;
        m.btc.value2 = b["vnd"] | 0.0;
        m.btc.changePct = b["usd_24h_change"] | 0.0;
        m.btc.ok = m.btc.value > 0;
    }
    m.fetchedMs = millis();
    int okCount = m.goldBar.ok + m.usd.ok + m.vnindex.ok + m.btc.ok;
    m.error = okCount ? "" : T("No connection", "Mất kết nối");
    Serial.printf("Markets: gold %d usd %d vnindex %d btc %d\n", m.goldBar.ok, m.usd.ok, m.vnindex.ok, m.btc.ok);
    markets = m;
    marketsFetching = false;
    marketsChanged = true;
    vTaskDelete(nullptr);
}

void marketsRefresh() {
    if (marketsFetching || WiFi.status() != WL_CONNECTED) return;
    marketsFetching = true;
    xTaskCreatePinnedToCore(marketsTask, "markets", 12288, nullptr, 1, nullptr, 1);
}

// A coloured pill with an up or down triangle; grey and flat when unchanged.
void uiChangePill(int right, int y, double change, const String &text) {
    bool up = change > 0, down = change < 0;
    uint16_t fg = up ? UI_GREEN : down ? UI_RED : UI_SUB;
    uint16_t bg = up ? rgb565(18, 56, 34) : down ? rgb565(66, 24, 24) : UI_LINE;
    int w = aaWidth(ui12, text) + (up || down ? 26 : 16);
    int x = right - w;
    tft.fillRoundRect(x, y, w, 20, 10, bg);
    if (up) tft.fillTriangle(x + 9, y + 13, x + 15, y + 13, x + 12, y + 7, fg);
    if (down) tft.fillTriangle(x + 9, y + 7, x + 15, y + 7, x + 12, y + 13, fg);
    uiBox(ui12, x + (up || down ? 19 : 8), y + 1, w - (up || down ? 26 : 16), 18, text, fg, bg);
}

const int MKT_CARD_H = 58;
void marketCard(int i, const String &label, const Quote &q, const String &value, const String &sub, const String &pill, double dir) {
    int y = CONTENT_Y + i * (MKT_CARD_H + 4);
    tft.fillRoundRect(8, y, 224, MKT_CARD_H, 14, UI_CARD);
    uiBox(ui12, 20, y + 6, 120, 18, label, UI_SUB, UI_CARD);
    if (!q.ok) {
        uiBox(ui15, 20, y + 26, 200, 24, "—", UI_SUB, UI_CARD);
        return;
    }
    uiBox(ui20b, 20, y + 24, 132, 30, value, UI_TXT, UI_CARD);
    uiBox(ui12, 112, y + 34, 108, 18, sub, UI_SUB, UI_CARD, AL_RIGHT);
    if (pill.length()) uiChangePill(220, y + 6, dir, pill);
}

void marketsDraw() {
    header(T("Markets", "Thị trường"));
    if (!markets.fetchedMs) {
        line(CONTENT_Y, marketsFetching ? T("Loading...", "Đang tải...") : markets.error, UI_SUB);
        return;
    }
    bool vi = uiLang == LANG_VI;
    const Markets &m = markets;
    String unitTr = vi ? " tr" : "M";
    marketCard(0, T("SJC gold · per tael", "Vàng SJC · lượng"), m.goldBar, fmtNum(m.goldBar.value / 1e6, 1) + unitTr,
               T("Buy ", "Mua ") + fmtNum(m.goldBar.value2 / 1e6, 1),
               m.goldBar.change ? (m.goldBar.change > 0 ? "+" : "") + fmtNum(m.goldBar.change / 1e6, 1) + unitTr : String(T("flat", "đứng")),
               m.goldBar.change);
    marketCard(1, T("USD · Vietcombank", "USD · Vietcombank"), m.usd, fmtNum(m.usd.value, 0),
               T("Buy ", "Mua ") + fmtNum(m.usd.value2, 0), "", 0);
    marketCard(2, "VN-Index", m.vnindex, fmtNum(m.vnindex.value, 2),
               (m.vnindex.change > 0 ? "+" : "") + fmtNum(m.vnindex.change, 2),
               (m.vnindex.changePct > 0 ? "+" : "") + fmtNum(m.vnindex.changePct, 2) + "%", m.vnindex.changePct);
    marketCard(3, "Bitcoin", m.btc, "$" + fmtNum(m.btc.value, 0),
               "~" + fmtNum(m.btc.value2 / 1e9, 2) + (vi ? " tỷ" : "B"),
               (m.btc.changePct > 0 ? "+" : "") + fmtNum(m.btc.changePct, 2) + "%", m.btc.changePct);
}

void marketsEnter() {
    if (!markets.fetchedMs || millis() - markets.fetchedMs > 5 * 60000UL) marketsRefresh();
    marketsDraw();
}
void marketsTick() {
    if (marketsChanged) {
        marketsChanged = false;
        marketsDraw();
    }
}
void marketsPress() {
    marketsRefresh();
    line(H - 22, T("Refreshing...", "Đang cập nhật..."), UI_SUB, 1);
}

// ---------- LED from the internet ----------
// The board keeps a connection open to ntfy.sh (a free pub/sub service over HTTP) on a secret
// topic, so anyone with the topic can drive the LED from anywhere without opening the router:
//   curl -d red https://ntfy.sh/<topic>        curl -d "#ff8800" ...    curl -d "blink blue" ...
// Commands: a colour name (English or Vietnamese), #rrggbb, "r,g,b", off/tắt, "blink <colour>",
// rainbow. The task only queues them; the loop drives the LED.
QueueHandle_t remoteLedQueue;
String remoteLedLast, remoteLedLastId;
unsigned long remoteLedLastMs = 0;
volatile bool remoteLedOnline = false;

void remoteLedTask(void *) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        WiFiClientSecure client;
        client.setInsecure();
        if (!client.connect("ntfy.sh", 443)) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        // HTTP/1.0 so the stream comes without chunked encoding: one JSON object per line. After a
        // reconnect, `since` asks for whatever was sent while the connection was down.
        client.printf("GET /%s/json%s HTTP/1.0\r\nHost: ntfy.sh\r\nUser-Agent: esp32-device\r\n\r\n", NTFY_TOPIC,
                      remoteLedLastId.length() ? ("?since=" + remoteLedLastId).c_str() : "");
        client.setTimeout(2000);
        bool body = false;
        remoteLedOnline = true;
        unsigned long lastDataMs = millis();
        while (client.connected()) {
            // Wait for data without spinning: this task shares core 0 with the idle task, and a
            // busy loop here starves it and trips the task watchdog.
            if (!client.available()) {
                if (millis() - lastDataMs > 120000) break; // no keepalive (every ~45 s) for 2 minutes
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            lastDataMs = millis();
            String ln = client.readStringUntil('\n');
            ln.trim();
            if (!body) {
                if (ln.isEmpty()) body = true;
                continue;
            }
            if (ln.isEmpty()) continue;
            JsonDocument doc;
            if (deserializeJson(doc, ln) || doc["event"] != "message") continue;
            remoteLedLastId = (const char *)(doc["id"] | "");
            char cmd[64];
            strlcpy(cmd, doc["message"] | "", sizeof(cmd));
            xQueueSend(remoteLedQueue, cmd, 0);
        }
        remoteLedOnline = false;
        client.stop();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

bool parseColor(String c, uint8_t &r, uint8_t &g, uint8_t &b) {
    c.trim();
    c.toLowerCase();
    struct Named { const char *name; uint8_t r, g, b; };
    static const Named NAMES[] = {{"off", 0, 0, 0}, {"tắt", 0, 0, 0}, {"red", 255, 0, 0}, {"đỏ", 255, 0, 0},
        {"orange", 255, 64, 0}, {"cam", 255, 64, 0}, {"yellow", 255, 176, 0}, {"vàng", 255, 176, 0},
        {"green", 0, 255, 0}, {"xanh lá", 0, 255, 0}, {"cyan", 0, 255, 255}, {"xanh lơ", 0, 255, 255},
        {"blue", 0, 0, 255}, {"xanh dương", 0, 0, 255}, {"xanh", 0, 0, 255}, {"purple", 128, 0, 255},
        {"tím", 128, 0, 255}, {"pink", 255, 0, 96}, {"hồng", 255, 0, 96}, {"white", 255, 255, 255},
        {"trắng", 255, 255, 255}, {"on", 255, 255, 255}, {"bật", 255, 255, 255}};
    for (const Named &n : NAMES) {
        if (c == n.name) {
            r = n.r, g = n.g, b = n.b;
            return true;
        }
    }
    if (c.startsWith("#") && c.length() == 7) {
        long v = strtol(c.c_str() + 1, nullptr, 16);
        r = v >> 16, g = v >> 8, b = v;
        return true;
    }
    int a1 = c.indexOf(','), a2 = c.indexOf(',', a1 + 1);
    if (a1 > 0 && a2 > a1) {
        r = constrain(c.substring(0, a1).toInt(), 0, 255);
        g = constrain(c.substring(a1 + 1, a2).toInt(), 0, 255);
        b = constrain(c.substring(a2 + 1).toInt(), 0, 255);
        return true;
    }
    return false;
}

// Effects run from the loop: a few blinks, or a minute of rainbow; any new command replaces them.
enum class LedFx { None, Blink, Rainbow };
LedFx ledFx = LedFx::None;
uint8_t ledFxR, ledFxG, ledFxB;
unsigned long ledFxStartMs = 0;

void remoteLedService() {
    char cmd[64];
    if (remoteLedQueue && xQueueReceive(remoteLedQueue, cmd, 0) == pdTRUE) {
        String c = cmd;
        c.trim();
        String lower = c;
        lower.toLowerCase();
        uint8_t r, g, b;
        bool ok = true;
        if (lower == "rainbow" || lower == "cầu vồng") {
            ledFx = LedFx::Rainbow;
            ledFxStartMs = millis();
        } else if ((lower.startsWith("blink ") || lower.startsWith("nháy ")) && parseColor(c.substring(c.indexOf(' ') + 1), r, g, b)) {
            ledFx = LedFx::Blink;
            ledFxR = r, ledFxG = g, ledFxB = b;
            ledFxStartMs = millis();
        } else if (parseColor(c, r, g, b)) {
            ledFx = LedFx::None;
            setLed(r, g, b);
        } else {
            ok = false;
        }
        remoteLedLast = c + (ok ? "" : " ?");
        remoteLedLastMs = millis();
        Serial.printf("Remote LED: \"%s\"%s\n", cmd, ok ? "" : " (not understood)");
    }
    unsigned long t = millis() - ledFxStartMs;
    if (ledFx == LedFx::Blink) {
        if (t > 4000) {
            ledFx = LedFx::None;
            setLed(0, 0, 0);
        } else {
            bool on = (t / 250) % 2 == 0;
            if ((ledNow[0] || ledNow[1] || ledNow[2]) != on) setLed(on ? ledFxR : 0, on ? ledFxG : 0, on ? ledFxB : 0);
        }
    } else if (ledFx == LedFx::Rainbow) {
        static unsigned long lastMs = 0;
        if (t > 60000) {
            ledFx = LedFx::None;
            setLed(0, 0, 0);
        } else if (millis() - lastMs > 30) {
            lastMs = millis();
            float h = fmodf(t / 3000.0f, 1.0f) * 6;
            int i = (int)h;
            float f = h - i;
            uint8_t q = 255 * (1 - f), u = 255 * f;
            uint8_t rgbv[6][3] = {{255, u, 0}, {q, 255, 0}, {0, 255, u}, {0, q, 255}, {u, 0, 255}, {255, 0, q}};
            setLed(rgbv[i][0], rgbv[i][1], rgbv[i][2]);
        }
    }
}

void remoteLedBegin() {
    if (!strlen(NTFY_TOPIC)) return;
    remoteLedQueue = xQueueCreate(8, 64);
    xTaskCreatePinnedToCore(remoteLedTask, "ntfy", 8192, nullptr, 1, nullptr, 0);
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

const int NAV_CX = 120, NAV_CY = 150, NAV_R = 50;

void navDraw() {
    if (!nav.hasDest) {
        line(CONTENT_Y, T("No destination", "Chưa có điểm đến"), UI_SUB);
        return;
    }
    String name = nav.destName.length() ? nav.destName : String(T("Destination", "Điểm đến"));
    uiBox(ui15b, 14, CONTENT_Y - 2, W - 28, 24, name, ACCENT, UI_BG);

    unsigned long ageS = nav.hasFix ? (millis() - nav.fixMs) / 1000 : 0;
    tft.fillCircle(NAV_CX, NAV_CY, NAV_R + 6, UI_CARD);
    if (!nav.hasFix) {
        uiBox(ui15, 0, 220, W, 24, T("Waiting for GPS", "Đang chờ GPS"), UI_YELLOW, UI_BG, AL_CENTER);
        tft.fillRect(0, 248, W, 60, UI_BG);
        return;
    }
    double dist = distanceM(nav.lat, nav.lon, nav.destLat, nav.destLon);
    double toDest = bearingDeg(nav.lat, nav.lon, nav.destLat, nav.destLon);
    bool arrived = dist < 30;
    // With a known direction of travel the arrow is relative to where you are heading;
    // standing still there is none, so the arrow is drawn north-up and says so.
    bool headingUp = nav.course >= 0 && nav.speedKmh >= 2;
    float angle = headingUp ? toDest - nav.course : toDest;
    drawArrow(NAV_CX, NAV_CY, NAV_R - 8, angle, arrived ? UI_GREEN : UI_TXT);
    if (!headingUp) uiBox(ui12, NAV_CX - 8, NAV_CY - NAV_R - 2, 16, 14, "N", UI_RED, UI_CARD, AL_CENTER);

    if (arrived) uiBox(ui20b, 0, 214, W, 40, T("Arrived", "Đã đến"), UI_GREEN, UI_BG, AL_CENTER);
    else uiBox(num34, 0, 214, W, 40, formatDistance(dist), UI_TXT, UI_BG, AL_CENTER);

    String speed = nav.speedKmh >= 0 ? String((int)lround(nav.speedKmh)) + " km/h" : String("-- km/h");
    String eta = !arrived && nav.speedKmh >= 2 ? "  ·  ~" + String((int)ceil(dist / 1000.0 / nav.speedKmh * 60)) + T(" min", " phút")
                                              : String("");
    uiBox(ui13b, 0, 256, W, 22, speed + eta, UI_TXT2, UI_BG, AL_CENTER);
    String gps = ageS <= 15 ? "GPS · " + String(ageS) + T("s ago", " giây trước") : T("GPS lost · ", "Mất GPS · ") + String(ageS) + "s";
    uiBox(ui12, 0, 282, W, 20, gps, ageS <= 15 ? UI_SUB : UI_RED, UI_BG, AL_CENTER);
}

void navEnter() {
    header(T("Navigate", "Chỉ đường"));
    navDraw();
}

void navTick() { // redraw once a second, and at once when a new fix or destination arrives
    static unsigned long lastDrawMs = 0, lastFixMs = 0;
    if (millis() - lastDrawMs < 1000 && nav.fixMs == lastFixMs) return;
    lastDrawMs = millis();
    lastFixMs = nav.fixMs;
    navDraw();
}

// ---------- settings ----------
// One setting so far, the screen language. The knob moves the highlight and a press applies it;
// a press on the language already in use switches to the other one, so BOOT alone can change it.
const char *LANG_NAMES[] = {"English", "Tiếng Việt"};
int settingsSel = 0;

// A list row like the menu's: a rounded highlight when selected, and a radio for the one in use.
void settingsDraw() {
    header(T("Settings", "Cài đặt"));
    uiText(ui12, 14, CONTENT_Y - 4, T("LANGUAGE", "NGÔN NGỮ"), UI_SUB);
    for (int i = 0; i < 2; i++) {
        const int x = 8, y = CONTENT_Y + 20 + i * 44, w = 224, h = 40;
        bool sel = i == settingsSel;
        uint16_t bg = sel ? UI_CARD_SEL : UI_CARD;
        tft.fillRoundRect(x, y, w, h, 12, bg);
        tft.drawCircle(x + 22, y + h / 2, 8, i == uiLang ? ACCENT : UI_SUB);
        if (i == uiLang) tft.fillCircle(x + 22, y + h / 2, 4, ACCENT);
        uiBox(sel ? ui15b : ui15, x + 42, y + 8, 160, 24, LANG_NAMES[i], sel ? UI_TXT : UI_TXT2, bg);
    }
}
void settingsEnter() {
    settingsSel = uiLang;
    settingsDraw();
}
void settingsRotate(int) {
    settingsSel ^= 1;
    settingsDraw();
}
void uiLangSet(uint8_t lang) {
    uiLang = lang == LANG_VI ? LANG_VI : LANG_EN;
    prefs.begin("ui", false);
    prefs.putUChar("lang", uiLang);
    prefs.end();
    Serial.printf("Language: %s\n", LANG_NAMES[uiLang]);
}
void settingsPress() {
    if (settingsSel == uiLang) settingsSel ^= 1;
    uiLangSet(settingsSel);
    settingsDraw();
}

enum Icon { ICON_TIMER, ICON_WEATHER, ICON_NAV, ICON_DOC, ICON_PHONE, ICON_WIFI, ICON_RGB, ICON_HOTSPOT, ICON_MIC, ICON_REC, ICON_ASK, ICON_SETTINGS, ICON_MARKET, ICON_BELL, ICON_INFO };

struct App {
    const char *name, *nameVi;
    Icon icon;
    void (*enter)();
    void (*tick)();
    void (*press)();
};
// The clock is not listed: it is the home screen, shown at power-on and below the menu.
const App APPS[] = {
    {"Ask AI", "Hỏi AI", ICON_ASK, askEnter, askTick, askPress},
    {"Notifications", "Thông báo", ICON_BELL, notesEnter, notesTick, notesPress},
    {"Timer", "Hẹn giờ", ICON_TIMER, timerEnter, timerTick, timerPress},
    {"Weather", "Thời tiết", ICON_WEATHER, weatherEnter, nullptr, weatherPress},
    {"Markets", "Thị trường", ICON_MARKET, marketsEnter, marketsTick, marketsPress},
    {"Navigate", "Chỉ đường", ICON_NAV, navEnter, navTick, nullptr},
    {"Documents", "Tài liệu", ICON_DOC, readerEnter, nullptr, readerPress},
    {"From phone", "Từ điện thoại", ICON_PHONE, receiveEnter, nullptr, nullptr},
    {"WiFi scan", "Quét WiFi", ICON_WIFI, radarEnter, radarTick, radarPress},
    {"RGB light", "Đèn RGB", ICON_RGB, ledEnter, nullptr, ledPress},
    {"Hotspot", "Phát WiFi", ICON_HOTSPOT, hotspotEnter, hotspotTick, hotspotPress},
    {"Sound meter", "Đo âm thanh", ICON_MIC, micEnter, micTick, micPress},
    {"Recorder", "Ghi âm", ICON_REC, recEnter, recTick, recPress},
    {"Settings", "Cài đặt", ICON_SETTINGS, settingsEnter, nullptr, settingsPress},
    {"Info", "Thông tin", ICON_INFO, infoEnter, infoTick, nullptr},
};
const int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);
// Indices looked up by entry function, so reordering the list never breaks them.
int appIndexOf(void (*enter)()) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (APPS[i].enter == enter) return i;
    }
    return -100;
}
const int APP_ASK = appIndexOf(askEnter);
const int APP_TIMER = appIndexOf(timerEnter);
const int APP_NAV = appIndexOf(navEnter);
const int APP_READER = appIndexOf(readerEnter);
const int APP_RECEIVE = appIndexOf(receiveEnter);
const int APP_LED = appIndexOf(ledEnter);
const int APP_HOTSPOT = appIndexOf(hotspotEnter);
const int APP_REC = appIndexOf(recEnter);
const int APP_SETTINGS = appIndexOf(settingsEnter);
const int APP_NOTES = appIndexOf(notesEnter);
const int HOME = -2; // appIndex value for the home clock (-1 is the menu)

// Drawing text with a background color overwrites the old glyphs in place, so a row can be
// refreshed without clearing it first - clearing is what makes the menu flicker.
// Menu as a vertical list (it is scrolled with a knob, so one axis): each row is a card with an
// icon drawn from shapes (no image assets) and the name. 7 rows fit; the list scrolls past that.
const int ROW_X = 8, ROW_W = 222, ROW_H = 38, ROW_GAP = 4, ROW_TOP = CONTENT_Y, ROWS_VISIBLE = 6;
int menuTop = 0; // first visible row

// App icons: a white glyph on a rounded tile in the app's colour, like a phone's home screen.
uint16_t iconColor(Icon icon) {
    switch (icon) {
        case ICON_ASK: return rgb565(124, 92, 255);
        case ICON_TIMER: return rgb565(255, 149, 0);
        case ICON_WEATHER: return rgb565(10, 132, 255);
        case ICON_NAV: return rgb565(48, 209, 88);
        case ICON_DOC: return rgb565(255, 69, 58);
        case ICON_PHONE: return rgb565(50, 173, 230);
        case ICON_WIFI: return rgb565(94, 92, 230);
        case ICON_RGB: return rgb565(58, 63, 75);
        case ICON_HOTSPOT: return rgb565(52, 199, 89);
        case ICON_MIC: return rgb565(255, 45, 85);
        case ICON_REC: return rgb565(255, 55, 95);
        case ICON_SETTINGS: return rgb565(142, 142, 147);
        case ICON_MARKET: return rgb565(255, 179, 0);
        case ICON_BELL: return rgb565(255, 59, 48);
        default: return rgb565(99, 120, 160);
    }
}

void drawIcon(Icon icon, int cx, int cy) {
    const uint16_t c = UI_TXT, tile = iconColor(icon);
    tft.fillRoundRect(cx - 15, cy - 15, 30, 30, 8, tile);
    switch (icon) {
        case ICON_ASK: // a four-point sparkle
            tft.fillTriangle(cx, cy - 10, cx - 3, cy, cx + 3, cy, c);
            tft.fillTriangle(cx, cy + 10, cx - 3, cy, cx + 3, cy, c);
            tft.fillTriangle(cx - 10, cy, cx, cy - 3, cx, cy + 3, c);
            tft.fillTriangle(cx + 10, cy, cx, cy - 3, cx, cy + 3, c);
            break;
        case ICON_TIMER: // stopwatch
            tft.drawCircle(cx, cy + 2, 8, c);
            tft.drawCircle(cx, cy + 2, 7, c);
            tft.fillRect(cx - 3, cy - 10, 7, 2, c);
            tft.drawFastVLine(cx, cy - 3, 6, c);
            tft.drawFastVLine(cx + 1, cy - 3, 6, c);
            break;
        case ICON_WEATHER: // sun behind a cloud
            tft.fillCircle(cx - 4, cy - 4, 5, rgb565(255, 214, 10));
            tft.fillCircle(cx - 4, cy + 3, 5, c);
            tft.fillCircle(cx + 3, cy, 6, c);
            tft.fillRoundRect(cx - 9, cy + 2, 19, 7, 3, c);
            break;
        case ICON_NAV: // a location arrow
            drawArrow(cx, cy, 10, 45, c);
            break;
        case ICON_DOC: // a page with lines
            tft.drawRoundRect(cx - 7, cy - 9, 14, 18, 2, c);
            tft.drawRoundRect(cx - 6, cy - 8, 12, 16, 2, c);
            for (int i = 0; i < 3; i++) tft.drawFastHLine(cx - 3, cy - 3 + i * 4, 7, c);
            break;
        case ICON_PHONE: // a phone with an arrow coming in
            tft.drawRoundRect(cx - 6, cy - 10, 12, 20, 3, c);
            tft.drawRoundRect(cx - 5, cy - 9, 10, 18, 2, c);
            tft.fillCircle(cx, cy + 6, 1, c);
            tft.fillTriangle(cx, cy + 1, cx - 3, cy - 3, cx + 3, cy - 3, c);
            tft.fillRect(cx - 1, cy - 7, 3, 5, c);
            break;
        case ICON_WIFI: // signal arcs
            for (int r = 4; r <= 12; r += 4) {
                tft.drawCircleHelper(cx, cy + 6, r, 0x1 | 0x2, c);
                tft.drawCircleHelper(cx, cy + 6, r + 1, 0x1 | 0x2, c);
            }
            tft.fillCircle(cx, cy + 6, 2, c);
            break;
        case ICON_RGB: // three colour dots
            tft.fillCircle(cx - 5, cy - 3, 5, rgb565(255, 69, 58));
            tft.fillCircle(cx + 5, cy - 3, 5, rgb565(48, 209, 88));
            tft.fillCircle(cx, cy + 5, 5, rgb565(10, 132, 255));
            break;
        case ICON_HOTSPOT: // a mast broadcasting to both sides
            tft.fillCircle(cx, cy - 3, 2, c);
            tft.drawLine(cx, cy - 1, cx - 4, cy + 10, c);
            tft.drawLine(cx, cy - 1, cx + 4, cy + 10, c);
            tft.drawCircleHelper(cx, cy - 3, 6, 0x1 | 0x8, c);
            tft.drawCircleHelper(cx, cy - 3, 6, 0x2 | 0x4, c);
            tft.drawCircleHelper(cx, cy - 3, 10, 0x1 | 0x8, c);
            tft.drawCircleHelper(cx, cy - 3, 10, 0x2 | 0x4, c);
            break;
        case ICON_MIC: // a microphone
            tft.fillRoundRect(cx - 3, cy - 10, 7, 12, 3, c);
            tft.drawCircleHelper(cx, cy - 1, 7, 0x4 | 0x8, c);
            tft.drawFastVLine(cx, cy + 6, 4, c);
            tft.drawFastHLine(cx - 4, cy + 10, 9, c);
            break;
        case ICON_REC: // a record button
            tft.drawCircle(cx, cy, 9, c);
            tft.fillCircle(cx, cy, 6, c);
            break;
        case ICON_SETTINGS: // a gear
            for (int k = 0; k < 8; k++) {
                float a = k * PI / 4;
                tft.fillCircle(cx + (int)lroundf(8 * cosf(a)), cy + (int)lroundf(8 * sinf(a)), 2, c);
            }
            tft.fillCircle(cx, cy, 7, c);
            tft.fillCircle(cx, cy, 3, tile);
            break;
        case ICON_BELL: // a bell
            tft.fillCircle(cx, cy - 2, 7, c);
            tft.fillRect(cx - 7, cy - 2, 15, 7, c);
            tft.fillRoundRect(cx - 10, cy + 4, 21, 3, 1, c);
            tft.fillCircle(cx, cy + 9, 2, c);
            tft.fillCircle(cx, cy - 10, 1, c);
            break;
        case ICON_MARKET: // a rising chart line
            tft.drawLine(cx - 9, cy + 6, cx - 3, cy - 1, c);
            tft.drawLine(cx - 9, cy + 7, cx - 3, cy, c);
            tft.drawLine(cx - 3, cy - 1, cx + 1, cy + 3, c);
            tft.drawLine(cx - 3, cy, cx + 1, cy + 4, c);
            tft.drawLine(cx + 1, cy + 3, cx + 9, cy - 6, c);
            tft.drawLine(cx + 1, cy + 4, cx + 9, cy - 5, c);
            tft.fillTriangle(cx + 10, cy - 8, cx + 4, cy - 7, cx + 9, cy - 2, c);
            break;
        case ICON_INFO: // an "i"
            tft.fillCircle(cx, cy - 6, 2, c);
            tft.fillRoundRect(cx - 1, cy - 2, 3, 10, 1, c);
            break;
    }
}

// One row, painted over its own area, so switching the selection repaints two rows only.
void drawMenuItem(int i) {
    if (i < menuTop || i >= menuTop + ROWS_VISIBLE) return; // scrolled out of view
    int y = ROW_TOP + (i - menuTop) * (ROW_H + ROW_GAP);
    bool sel = i == menuSel;
    uint16_t bg = sel ? UI_CARD_SEL : UI_BG;
    tft.fillRoundRect(ROW_X, y, ROW_W, ROW_H, 12, bg);
    drawIcon(APPS[i].icon, ROW_X + 23, y + ROW_H / 2);
    uiBox(sel ? ui15b : ui15, ROW_X + 46, y + 8, 140, 22, T(APPS[i].name, APPS[i].nameVi), sel ? UI_TXT : UI_TXT2, bg);
    if (sel) uiBox(ui15, ROW_X + ROW_W - 22, y + 8, 14, 22, "›", UI_SUB, bg, AL_CENTER);
}

void drawScrollbar() { // thin bar on the right showing where the visible rows sit in the list
    if (APP_COUNT <= ROWS_VISIBLE) return;
    const int top = ROW_TOP + 4, h = ROWS_VISIBLE * (ROW_H + ROW_GAP) - ROW_GAP - 8;
    int thumbH = h * ROWS_VISIBLE / APP_COUNT;
    int thumbY = top + (h - thumbH) * menuTop / (APP_COUNT - ROWS_VISIBLE);
    tft.fillRect(W - 7, top, 3, h, UI_BG);
    tft.fillRoundRect(W - 7, thumbY, 3, thumbH, 1, rgb565(70, 76, 90));
}

void drawMenu() {
    header(T("Apps", "Ứng dụng"));
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
    if (appIndex == APP_ASK && (askState == AskState::Recording || askState == AskState::Thinking)) askCancel();
    else if (appIndex == APP_NOTES && notesDetail) notesPress();
    else if (appIndex == APP_READER && !readerInList) readerEnter();
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

void showBanner(const AncsNote &n);
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
        String json = String("{\"level\":{\"boot\":") + digitalRead(BUTTON) + ",\"ok\":" + digitalRead(BTN_OK) +
            ",\"back\":" + digitalRead(BACK_BTN) + ",\"up\":" + digitalRead(BTN_UP) + ",\"down\":" + digitalRead(BTN_DOWN) +
            "},\"taps\":{\"boot\":" + bootTaps + ",\"select\":" + knobTaps + ",\"back\":" + backTaps +
            ",\"up\":" + upTaps + ",\"down\":" + downTaps + "}}";
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
        AskProvider pv = askProvider();
        String keys;
        for (int i = 0; i < ASK_PROVIDER_COUNT; i++) {
            keys += String(i ? "," : "") + "\"" + ASK_PROVIDER_IDS[i] + "\":" + (askKey((AskProvider)i).length() ? "true" : "false");
        }
        server.send(200, "application/json", String("{\"provider\":\"") + ASK_PROVIDER_IDS[pv] + "\",\"keys\":{" + keys + "}" +
            ",\"hasKey\":" + (askKey(pv).length() ? "true" : "false") +
            ",\"model\":\"" + jsonEscape(askModel(pv)) + "\",\"state\":\"" + STATES[(int)st] + "\"" +
            ",\"elapsedS\":" + String(st == AskState::Recording ? (millis() - recStartMs) / 1000 : st == AskState::Thinking ? (millis() - askSentMs) / 1000 : 0) +
            ",\"tookMs\":" + String(askTookMs) + ",\"lines\":" + String(askLines.size()) + ",\"scroll\":" + String(askScroll) +
            ",\"question\":\"" + jsonEscape(askQuestion) + "\",\"answer\":\"" + jsonEscape(askAnswer) +
            "\",\"error\":\"" + jsonEscape(askError) + "\"}");
    });
    // provider=<id> picks the one in use; key/model, when given, are saved for that provider.
    server.on("/ask/key", HTTP_POST, []() {
        AskProvider pv = askProvider();
        if (server.hasArg("provider")) {
            String id = server.arg("provider");
            int found = -1;
            for (int i = 0; i < ASK_PROVIDER_COUNT; i++) {
                if (id == ASK_PROVIDER_IDS[i]) found = i;
            }
            if (found < 0) return server.send(400, "text/plain", "Nền tảng không hợp lệ");
            pv = (AskProvider)found;
            askSetProvider(pv);
        }
        prefs.begin(ASK_PROVIDER_IDS[pv], false);
        if (server.hasArg("key")) prefs.putString("key", server.arg("key"));
        if (server.hasArg("model")) prefs.putString("model", server.arg("model"));
        prefs.end();
        if (appIndex == APP_ASK && (askState == AskState::Idle || askState == AskState::Failed)) {
            askState = AskState::Idle;
            askWasCancelled = false;
            askShown = askState;
            askDraw();
        }
        server.send(200, "text/plain", String("Đã lưu (") + ASK_PROVIDER_NAMES[pv] + ")");
    });
    server.on("/lang", HTTP_GET, []() { server.send(200, "text/plain", uiLang == LANG_VI ? "vi" : "en"); });
    server.on("/lang", HTTP_POST, []() { // same as Settings on the device: lang=en|vi, redraws the current screen
        String l = server.arg("lang");
        if (l != "en" && l != "vi") return server.send(400, "text/plain", "lang=en|vi");
        uiLangSet(l == "vi" ? LANG_VI : LANG_EN);
        if (appIndex == HOME) goHome();
        else if (appIndex < 0) drawMenu();
        else openApp(appIndex);
        server.send(200, "text/plain", l);
    });
    server.on("/ask/cancel", HTTP_POST, []() { // same as the back button while asking
        bool active = askState == AskState::Recording || askState == AskState::Thinking;
        if (active) askCancel();
        server.send(200, "text/plain", active ? "Đã hủy" : "Không có câu hỏi nào đang chạy");
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
    // Firmware update over WiFi: POST the .bin as multipart to /ota?key=<OTA_KEY> (tools/ota_upload.sh).
    // It is written to the other app slot and the board boots it only if the whole image checks out.
    server.on("/ota", HTTP_POST, []() {
        if (otaError.length()) {
            server.send(otaError == "bad key" ? 403 : 400, "text/plain", "OTA failed: " + otaError);
            if (appIndex >= 0) openApp(appIndex);
            else goHome();
            return;
        }
        server.send(200, "text/plain", "OTA ok, " + String(otaBytes) + " bytes, restarting");
        delay(300);
        ESP.restart();
    }, []() {
        HTTPUpload &up = server.upload();
        if (up.status == UPLOAD_FILE_START) {
            otaError = "";
            otaBytes = 0;
            if (strlen(OTA_KEY) < 12 || !strcmp(OTA_KEY, "change-me") || server.arg("key") != OTA_KEY) {
                otaError = "bad key";
                return;
            }
            if (recording) recStop();
            header(T("Updating", "Dang cap nhat"));
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaError = Update.errorString();
            Serial.println("OTA start");
        } else if (up.status == UPLOAD_FILE_WRITE) {
            if (otaError.length()) return;
            if (Update.write(up.buf, up.currentSize) != up.currentSize) otaError = Update.errorString();
            otaBytes += up.currentSize;
            static size_t shown = 0;
            if (otaBytes - shown >= 32768 || otaBytes < shown) {
                shown = otaBytes;
                line(CONTENT_Y, String(otaBytes / 1024) + " KB", UI_TXT, 3);
            }
        } else if (up.status == UPLOAD_FILE_END) {
            if (!otaError.length() && !Update.end(true)) otaError = Update.errorString();
            Serial.printf("OTA end: %u bytes %s\n", otaBytes, otaError.length() ? otaError.c_str() : "ok");
        } else if (up.status == UPLOAD_FILE_ABORTED) {
            Update.abort();
            otaError = "aborted";
        }
    });
    server.on("/led", HTTP_GET, []() { // the LED's colour and the remote control's state
        char hex[8];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x", ledNow[0], ledNow[1], ledNow[2]);
        server.send(200, "application/json", String("{\"color\":\"") + hex + "\",\"remoteOnline\":" +
            (remoteLedOnline ? "true" : "false") + ",\"last\":\"" + jsonEscape(remoteLedLast) + "\",\"lastAgeS\":" +
            String(remoteLedLastMs ? (long)((millis() - remoteLedLastMs) / 1000) : -1L) + "}");
    });
    server.on("/markets", HTTP_GET, []() {
        const Markets &m = markets;
        auto q = [](const char *name, const Quote &x) {
            return String("\"") + name + "\":{\"ok\":" + (x.ok ? "true" : "false") + ",\"value\":" + String(x.value, 2) +
                ",\"value2\":" + String(x.value2, 2) + ",\"change\":" + String(x.change, 2) + ",\"changePct\":" + String(x.changePct, 2) + "}";
        };
        server.send(200, "application/json", String("{\"fetching\":") + (marketsFetching ? "true" : "false") +
            ",\"ageS\":" + String(m.fetchedMs ? (long)((millis() - m.fetchedMs) / 1000) : -1L) + "," +
            q("goldBar", m.goldBar) + "," + q("goldRing", m.goldRing) + "," + q("goldWorld", m.goldWorld) + "," +
            q("usd", m.usd) + "," + q("vnindex", m.vnindex) + "," + q("btc", m.btc) + "}");
    });
    server.on("/notifications", HTTP_GET, []() {
        String json = String("{\"connected\":") + (ancs::connected ? "true" : "false") + ",\"encrypted\":" +
            (ancs::encrypted ? "true" : "false") + ",\"ready\":" + (ancs::ready ? "true" : "false") +
            ",\"bonds\":" + String(NimBLEDevice::getNumBonds()) + ",\"log\":\"" + jsonEscape(ancs::log) + "\",\"notes\":[";
        for (size_t i = 0; i < notes.size(); i++) {
            json += String(i ? "," : "") + "{\"app\":\"" + jsonEscape(notes[i].n.app) + "\",\"title\":\"" + jsonEscape(notes[i].n.title) +
                "\",\"message\":\"" + jsonEscape(notes[i].n.message) + "\",\"category\":" + notes[i].n.category +
                ",\"ageS\":" + String((millis() - notes[i].ms) / 1000) + "}";
        }
        server.send(200, "application/json", json + "]}");
    });
    server.on("/notifications/test", HTTP_POST, []() { // a fake notification, to check the banner and list
        AncsNote n{0xFFFFFF00u + (uint32_t)notes.size(), 4, "com.vng.zalo", server.arg("title"), server.arg("message")};
        notes.insert(notes.begin(), {n, millis()});
        if (appIndex != APP_NOTES) showBanner(n);
        else notesDraw();
        server.send(200, "text/plain", "ok");
    });
    server.on("/screen", HTTP_GET, []() { // what the panel shows, as a 24-bit BMP (from the PSRAM mirror)
        if (!tft.mirror) return server.send(503, "text/plain", "no mirror");
        const uint32_t rowBytes = W * 3, size = 54 + rowBytes * H;
        uint8_t hdr[54] = {'B', 'M'};
        auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) hdr[at + i] = v >> (8 * i); };
        put32(2, size);
        put32(10, 54);
        put32(14, 40);
        put32(18, W);
        put32(22, H);
        hdr[26] = 1;
        hdr[28] = 24;
        put32(34, rowBytes * H);
        server.setContentLength(size);
        server.send(200, "image/bmp", "");
        server.sendContent((const char *)hdr, 54);
        static uint8_t row[W * 3];
        for (int y = H - 1; y >= 0; y--) { // BMP rows go bottom-up, pixels as B, G, R
            for (int x = 0; x < W; x++) {
                uint16_t c = tft.mirror[y * W + x];
                row[x * 3] = (c & 31) << 3 | (c & 31) >> 2;
                row[x * 3 + 1] = ((c >> 5) & 63) << 2 | ((c >> 5) & 63) >> 4;
                row[x * 3 + 2] = (c >> 11) << 3 | (c >> 11) >> 2;
            }
            server.sendContent((const char *)row, sizeof(row));
        }
    });
    server.on("/battery/calibrate", HTTP_POST, []() { // volts = the rail's true voltage right now
        float volts = server.arg("volts").toFloat();
        uint32_t mv = batteryPinMv();
        if (volts < 2.8f || volts > 4.4f || mv < 500) return server.send(400, "text/plain", "volts=2.8..4.4");
        batteryDivider = volts * 1000.0f / mv;
        prefs.begin("battery", false);
        prefs.putFloat("divider", batteryDivider);
        prefs.end();
        batterySmoothed = 0;
        batteryRecentCount = 0;
        server.send(200, "text/plain", "pin " + String(mv) + " mV, divider " + String(batteryDivider, 4));
    });
    server.on("/battery", HTTP_GET, []() {
        float v = batteryVolts();
        server.send(200, "application/json", String("{\"present\":") + (batteryPresent(v) ? "true" : "false") +
            ",\"volts\":" + String(v, 2) + ",\"percent\":" + String(batteryPresent(v) ? batteryPercent(v) : 0) +
            ",\"raw\":" + String(batteryVoltsRaw(), 2) + ",\"spread\":" + String(batterySpread(), 2) + ",\"charging\":" + (batteryCharging ? "true" : "false") + ",\"pinMv\":" + String(batteryPinMv()) +
            ",\"divider\":" + String(batteryDivider, 4) + "}");
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

// ---------- moving (up / down buttons) ----------

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
        readerMove(dir);
    } else if (appIndex == APP_ASK) {
        askRotate(dir);
    } else if (appIndex == APP_NOTES) {
        notesRotate(dir);
    } else if (appIndex == APP_SETTINGS) {
        settingsRotate(dir);
    } else if (appIndex == APP_LED) {
        int count = sizeof(PRESETS) / sizeof(PRESETS[0]);
        presetIndex = (presetIndex + dir + count) % count;
        ledShow();
    }
}

// ---------- button ----------

struct ButtonState {
    bool armed = false; // a button counts only after it has been seen released once
    bool down = false, longFired = false;
    unsigned long pressedMs = 0, lastStepMs = 0, releasedMs = 0;
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

// Select, the same thing a knob press used to do: open the menu from the clock, open the app
// under the cursor, or the app's own action.
void selectAction() {
    knobTaps++;
    if (appIndex == HOME) backToMenu();
    else if (appIndex < 0) openApp(menuSel);
    else if (APPS[appIndex].press) APPS[appIndex].press();
}

// Up / down buttons: a press moves one step at once; held, it keeps stepping like a key on a
// keyboard, after a short pause and then faster. On the home clock a press opens the menu.
const unsigned long NAV_REPEAT_DELAY_MS = 400, NAV_REPEAT_MS = 120, NAV_FAST_AFTER_MS = 1500, NAV_FAST_MS = 50;
void navStep(int dir, bool first) {
    dir > 0 ? downTaps++ : upTaps++;
    if (appIndex == HOME) {
        if (first) backToMenu();
    } else {
        onRotate(dir);
    }
}

void serviceNavButton(bool now, int dir, ButtonState &s) {
    if (!s.armed) {
        if (!now) s.armed = true;
        return;
    }
    unsigned long t = millis();
    if (now && !s.down) {
        if (t - s.releasedMs < 40) return; // contact bounce right after a release
        s.down = true;
        s.pressedMs = s.lastStepMs = t;
        navStep(dir, true);
    } else if (now && s.down) {
        unsigned long held = t - s.pressedMs;
        unsigned long every = held > NAV_FAST_AFTER_MS ? NAV_FAST_MS : NAV_REPEAT_MS;
        if (held > NAV_REPEAT_DELAY_MS && t - s.lastStepMs >= every) {
            s.lastStepMs = t;
            navStep(dir, false);
        }
    } else if (!now && s.down) {
        if (t - s.pressedMs < 30) return; // a press must last 30 ms before its release counts
        s.down = false;
        s.releasedMs = t;
    }
}

void handleButton() {
    static ButtonState up, down;
    serviceNavButton(digitalRead(BTN_UP) == LOW, -1, up);
    serviceNavButton(digitalRead(BTN_DOWN) == LOW, 1, down);

    static bool armed = false;
    static ButtonState boot, knob;
    bool bootDown = digitalRead(BUTTON) == LOW;
    if (!armed) { // the programmer holds GPIO0 low across a reset; ignore it until released
        if (!bootDown) armed = true;
        bootDown = false;
    }
    serviceButton(bootDown, false, true, boot);
    serviceButton(digitalRead(BTN_OK) == LOW, true, false, knob); // OK: a tap selects

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
    const int h = 28;
    clockBgRestore(CLOCK_TIMER_Y, h);
    if (!active) return;
    String text = T("Timer  ", "Hẹn giờ  ") + timerText() + (timerState == TimerState::Paused ? T("  paused", "  tạm dừng") : "");
    int w = aaWidth(ui13b, text) + 32;
    const uint16_t pill = rgb565(58, 38, 10);
    tft.fillRoundRect((W - w) / 2, CLOCK_TIMER_Y, w, h, h / 2, pill);
    uiBox(ui13b, (W - w) / 2 + 14, CLOCK_TIMER_Y + 4, w - 28, h - 8, text, UI_ORANGE, pill, AL_CENTER);
}

// ---------- notification banner ----------
// Puts back whatever the banner covered. Screens whose state an app re-entry would lose (an open
// document, an answer being read, a phone image) are redrawn their own way or not covered at all.
void restoreScreen() {
    if (appIndex == HOME) goHome();
    else if (appIndex < 0) drawMenu();
    else if (appIndex == APP_READER && !readerInList) showStoredPage(currentPage);
    else if (appIndex == APP_ASK && askState != AskState::Idle) askDraw();
    else if (appIndex == APP_NOTES) notesDraw();
    else openApp(appIndex);
}

void showBanner(const AncsNote &n) {
    const int y = 4, h = 70;
    uint16_t bg = UI_CARD_SEL;
    tft.fillRoundRect(6, y, 228, h, 16, bg);
    tft.drawRoundRect(6, y, 228, h, 16, UI_LINE);
    drawIcon(n.category == 1 ? ICON_PHONE : ICON_BELL, 30, y + 22);
    uiBox(ui12, 52, y + 4, 172, 21, appLabel(n.app), n.category == 1 ? UI_GREEN : ACCENT, bg);
    uiBox(ui15b, 52, y + 22, 172, 23, oneLine(n.title), UI_TXT, bg);
    uiBox(ui12, 18, y + 46, 206, 21, oneLine(n.message), UI_TXT2, bg);
    bannerUntilMs = millis() + (n.category == 1 ? 10000 : 5000);
}

void notesService() {
    AncsNote n;
    while (ancs::take(n)) {
        Serial.printf("Notification: %s | %s | %s\n", n.app.c_str(), n.title.c_str(), n.message.c_str());
        notes.insert(notes.begin(), {n, millis()});
        if (notes.size() > 20) notes.pop_back();
        if (appIndex == APP_NOTES && !notesDetail) {
            notesSel = 0;
            notesTop = 0;
            notesDraw();
        } else {
            unreadNotes++;
            bool covered = appIndex == APP_RECEIVE || otaBytes || (appIndex == APP_ASK && askState == AskState::Recording);
            if (!covered) showBanner(n);
        }
    }
    uint32_t uid;
    while (ancs::takeRemoved(uid)) { // dismissed on the phone: drop it here too
        for (size_t i = 0; i < notes.size(); i++) {
            if (notes[i].n.uid == uid) {
                notes.erase(notes.begin() + i);
                if (appIndex == APP_NOTES && !notesDetail) {
                    notesSel = constrain(notesSel, 0, max(0, (int)notes.size() - 1));
                    notesDraw();
                }
                break;
            }
        }
    }
    if (bannerUntilMs && (long)(millis() - bannerUntilMs) >= 0) {
        bannerUntilMs = 0;
        restoreScreen();
    }
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
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_OK, INPUT_PULLUP);
    pinMode(BACK_BTN, INPUT_PULLUP);
    initLed();
    setLed(0, 0, 0);
    micBegin();
    recBegin();
    batteryBegin();

    // On a plain charger the board starts the instant power appears, whereas a computer's USB
    // enumeration delays it. Give the 3.3 V rail time to settle before initialising the panel,
    // otherwise it comes up blank and stays blank.
    delay(300);
    prefs.begin("ui", true);
    uiLang = prefs.getUChar("lang", LANG_EN) == LANG_VI ? LANG_VI : LANG_EN;
    prefs.end();
    screenInit();
    header(T("Starting...", "Đang khởi động..."));
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
    line(CONTENT_Y, T("Connecting WiFi...", "Đang kết nối WiFi..."), UI_SUB);
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
    remoteLedBegin();
    ancs::begin(BLE_NAME);
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
    handleButton();
    if (appIndex >= 0 && APPS[appIndex].tick) APPS[appIndex].tick();
    if (appIndex == HOME) {
        clockTick();
        homeTimerLine();
    }
    timerService();
    bootLogTick();
    remoteLedService();
    notesService();
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
    // Keep the status bar's time and battery current, but not while a document page or a phone
    // image covers the top of the screen.
    if (millis() - lastBatteryMs > 2000) {
        lastBatteryMs = millis();
        bool covered = appIndex == HOME || appIndex == APP_RECEIVE || (appIndex == APP_READER && !readerInList) || otaBytes;
        if (!covered && statusKey() != statusShownKey) uiStatusBar();
    }
}
