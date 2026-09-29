#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include "rgb_led.h"

#define DEVICE_NAME         "ESP32-S3-Test"
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLECharacteristic *pCharacteristic;
volatile bool deviceConnected = false;
bool wasConnected = false;
uint32_t counter = 0;

// Callbacks run in the BLE stack's task: only set flags here, act on them in loop().
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *) override { deviceConnected = true; }
    void onDisconnect(BLEServer *) override { deviceConnected = false; }
};

void setup() {
    Serial.begin(115200);
    initLed();
    setLed(0, 0, 255); // Blue = advertising, waiting for a phone

    BLEDevice::init(DEVICE_NAME);
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pCharacteristic->addDescriptor(new BLE2902());
    pService->start();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    // Name goes in the scan response: name + 128-bit UUID don't fit in one 31-byte packet.
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();
}

void loop() {
    if (deviceConnected && !wasConnected) {
        setLed(0, 255, 0); // Green = phone connected
        Serial.println("Phone connected");
    }
    if (!deviceConnected && wasConnected) {
        setLed(0, 0, 255);
        delay(500); // let the BLE stack settle before advertising again
        BLEDevice::startAdvertising();
        Serial.println("Phone disconnected, advertising again");
    }
    wasConnected = deviceConnected;

    if (deviceConnected) {
        counter++;
        pCharacteristic->setValue(counter);
        pCharacteristic->notify();
    }

    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 5000) {
        lastPrint = millis();
        Serial.printf("BLE \"%s\" %s | counter=%lu\n", DEVICE_NAME,
            deviceConnected ? "CONNECTED" : "advertising", (unsigned long)counter);
    }
    delay(1000);
}
