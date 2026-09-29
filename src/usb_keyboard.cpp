#include <Arduino.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST  13
#define BUTTON    0 // BOOT button

const char *MESSAGE = "hello from esp32";

Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
USBHIDKeyboard Keyboard;

void show(const char *status, uint16_t color) {
    tft.fillScreen(ST77XX_BLACK);
    tft.fillRect(0, 0, 240, 24, ST77XX_BLUE);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 4);
    tft.print("USB keyboard");
    tft.setTextColor(color);
    tft.setCursor(8, 60);
    tft.print(status);
}

void chord(uint8_t modifier, uint8_t key) {
    Keyboard.press(modifier);
    if (key) Keyboard.press(key);
    delay(80);
    Keyboard.releaseAll();
}

// Written for Ubuntu GNOME: Super opens the app search, which launches Text Editor. Ctrl+T then
// opens a fresh tab, because Text Editor restores the last session and the message must not
// land in someone's existing document. The pauses give the desktop time to react.
void openEditorAndType() {
    show("Typing...", ST77XX_YELLOW);
    chord(KEY_LEFT_GUI, 0);
    delay(900);
    Keyboard.print("text editor");
    delay(1200);
    Keyboard.write(KEY_RETURN);
    delay(3000);
    chord(KEY_LEFT_CTRL, 't');
    delay(600);
    Keyboard.print(MESSAGE);
    show("Done", ST77XX_GREEN);
}

void setup() {
    pinMode(BUTTON, INPUT_PULLUP);
    delay(300); // let the 3.3 V rail settle before initialising the panel
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.init(240, 320);
    tft.setRotation(0);
    tft.setTextWrap(false);

    Keyboard.begin();
    USB.begin(); // from here the computer sees a USB keyboard on the native port
    show("Ready", ST77XX_WHITE);
}

void loop() {
    // Type only on a button press, never on plug-in: keystrokes go to whatever window has focus.
    // The programmer holds GPIO0 low across a reset, so wait for a release before listening.
    static bool armed = false, down = false;
    bool now = digitalRead(BUTTON) == LOW;
    if (!armed) {
        armed = !now;
        return;
    }
    if (now && !down) {
        down = true;
        delay(30); // debounce
    } else if (!now && down) {
        down = false;
        openEditorAndType();
    }
}
