#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  13

Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

void drawBar(int y, uint16_t color, const char *label) {
    tft.fillRect(0, y, tft.width(), 50, color);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(3);
    tft.setCursor(12, y + 14);
    tft.print(label);
}

void setup() {
    Serial.begin(115200);

    // The board definition's default SPI pins don't match this wiring, so start SPI on ours.
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.init(240, 320);
    tft.setRotation(0); // portrait, 240 wide x 320 tall
    tft.fillScreen(ST77XX_BLACK);

    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(12, 15);
    tft.print("Hello ESP32-S3!");

    // Labeled bars: if the bar labeled RED shows cyan, the panel needs invertDisplay(false).
    drawBar(50, ST77XX_RED, "RED");
    drawBar(105, ST77XX_GREEN, "GREEN");
    drawBar(160, ST77XX_BLUE, "BLUE");
}

void loop() {
    unsigned long secs = millis() / 1000;

    tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK); // background color overwrites old digits
    tft.setTextSize(2);
    tft.setCursor(12, 250);
    tft.printf("Uptime: %lus   ", secs);

    if (secs % 5 == 0) Serial.printf("TFT demo running, uptime %lus\n", secs);
    delay(1000);
}
