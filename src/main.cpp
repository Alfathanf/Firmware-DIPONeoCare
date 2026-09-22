#include <Arduino.h>

#include "camera/camera_manager.h"
#include "config.h"
#include "frame/frame_uploader.h"
#include "network/device_registration.h"
#include "network/heartbeat.h"
#include "secrets.h"
#include "streaming/stream_server.h"
#include "wifi/wifi_manager.h"
#include "wifi/wifi_provisioning.h"

namespace {
void printStartupBanner() {
    Serial.println();
    Serial.println("==============================");
    Serial.println("DIPONeoCare - ESP32-S3 Camera Base");
    Serial.println("==============================");
}

void printSystemStatus() {
    if (psramFound()) {
        Serial.println("[BOOT] PSRAM: Detected");
        Serial.printf("[BOOT] PSRAM size: %u bytes\n", ESP.getPsramSize());
    } else {
        Serial.println("[BOOT] PSRAM: Not detected");
    }

    Serial.printf("[BOOT] Flash size: %u bytes\n", ESP.getFlashChipSize());
}

void printStreamUrl() {
    if (wifiManagerIsConnected()) {
        Serial.println();
        Serial.println("==============================");
        Serial.print("[STREAM] http://");
        Serial.print(wifiManagerGetLocalIp());
        Serial.println("/stream");
        Serial.println("==============================");
    }
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1000);

    printStartupBanner();
    printSystemStatus();

    if (!cameraInit()) {
        Serial.println("[BOOT] Camera initialization failed. System halted.");
        return;
    }

    wifiManagerInit(WIFI_SSID, WIFI_PASSWORD);
    deviceRegistrationInit();
    heartbeatInit();
    frameUploaderInit();
    if (wifiManagerIsConnected()) {
        streamServerStart(config::kStreamPort);
        printStreamUrl();
    } else if (wifiManagerIsProvisioning()) {
        Serial.println("[BOOT] Waiting for Wi-Fi configuration");
    }
}

void loop() {
    wifiManagerUpdate();
    if (wifiManagerIsConnected()) {
        deviceRegistrationUpdate();
        heartbeatUpdate();
        frameUploaderUpdate();
        if (!streamServerIsRunning()) {
            streamServerStart(config::kStreamPort);
            printStreamUrl();
        }
    } else if (!wifiManagerIsProvisioning()) {
        if (streamServerIsRunning()) {
            streamServerStop();
        }
    }

    static uint32_t lastUrlPrintMs = 0;
    const uint32_t now = millis();
    if (wifiManagerIsConnected() && (now - lastUrlPrintMs >= 30000)) {
        lastUrlPrintMs = now;
        printStreamUrl();
    }

    delay(100);
}

