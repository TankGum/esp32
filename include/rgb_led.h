#pragma once
#include <Arduino.h>

// WS2812 RGB LED: GPIO48 on ESP32-S3-DevKitC-1 v1.0 clones, GPIO38 on v1.1. Drive both.
// (neopixelWrite() can only drive one pin: it binds to the first pin it is called with.)
static const int LED_PINS[] = {48, 38};
static rmt_obj_t *ledRmt[2];

static void initLed() {
    for (int i = 0; i < 2; i++) {
        ledRmt[i] = rmtInit(LED_PINS[i], RMT_TX_MODE, RMT_MEM_64);
        if (ledRmt[i]) rmtSetTick(ledRmt[i], 100); // 100ns per tick
    }
}

static uint8_t ledNow[3]; // the colour last sent, so it can be reported

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
    ledNow[0] = r;
    ledNow[1] = g;
    ledNow[2] = b;
    rmt_data_t data[24];
    uint8_t color[3] = {g, r, b}; // WS2812 expects GRB order
    int i = 0;
    for (int c = 0; c < 3; c++) {
        for (int bit = 7; bit >= 0; bit--, i++) {
            bool one = color[c] & (1 << bit);
            data[i].level0 = 1;
            data[i].duration0 = one ? 8 : 4;
            data[i].level1 = 0;
            data[i].duration1 = one ? 4 : 8;
        }
    }
    for (rmt_obj_t *rmt : ledRmt) {
        if (rmt) rmtWriteBlocking(rmt, data, 24);
    }
}
