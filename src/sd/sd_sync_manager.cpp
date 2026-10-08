#include "sd/sd_sync_manager.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "network/device_registration.h"
#include "network/device_status.h"
#include "sd/sd_manager.h"
#include "wifi/wifi_manager.h"

namespace {
constexpr uint32_t kSyncRetryMs = 15000;
constexpr uint32_t kSyncLoopMs = 2000;
constexpr char kMultipartBoundary[] = "DIPONeoCareFrameBoundary";

TaskHandle_t g_syncTask = nullptr;
bool g_initialized = false;
bool g_syncActive = false;
uint32_t g_lastAttemptMs = 0;

bool shouldSync() {
    return wifiManagerIsConnected() &&
        deviceRegistrationIsRegistered() &&
        sdManager::isAvailable() &&
        sdManager::pendingFileCount() > 0;
}

bool uploadPendingFile() {
    String jpegPath;
    String metaPath;
    if (!sdManager::nextPendingFile(jpegPath, metaPath)) {
        return false;
    }

    String captureId;
    String captureTimestamp;
    if (!sdManager::readPendingMetadata(metaPath, captureId, captureTimestamp)) {
        Serial.printf("[SYNC] Missing metadata for %s; removing stale record\n", jpegPath.c_str());
        sdManager::deletePendingFile(jpegPath, metaPath);
        return false;
    }

    uint8_t* jpegBuffer = nullptr;
    size_t jpegLength = 0;
    if (!sdManager::readPendingJpeg(jpegPath, &jpegBuffer, jpegLength)) {
        Serial.printf("[SYNC] Failed to read pending frame %s\n", jpegPath.c_str());
        return false;
    }

    const String url = String(config::kBackendBaseUrl) + String(config::kFrameUploadPath);
    const String macAddress = wifiManagerGetMacAddress();
    const String contentType = String("multipart/form-data; boundary=") + kMultipartBoundary;
    const String header = String("--") + kMultipartBoundary + "\r\n"
        + "Content-Disposition: form-data; name=\"macAddress\"\r\n\r\n"
        + macAddress + "\r\n"
        + "--" + kMultipartBoundary + "\r\n"
        + "Content-Disposition: form-data; name=\"captureId\"\r\n\r\n"
        + captureId + "\r\n"
        + "--" + kMultipartBoundary + "\r\n"
        + "Content-Disposition: form-data; name=\"timestamp\"\r\n\r\n"
        + captureTimestamp + "\r\n"
        + "--" + kMultipartBoundary + "\r\n"
        + "Content-Disposition: form-data; name=\"image\"; filename=\"frame.jpg\"\r\n"
        + "Content-Type: image/jpeg\r\n\r\n";
    const String footer = String("\r\n--") + kMultipartBoundary + "--\r\n";

    const size_t totalLength = header.length() + jpegLength + footer.length();
    uint8_t* payload = static_cast<uint8_t*>(malloc(totalLength));
    if (payload == nullptr) {
        free(jpegBuffer);
        Serial.println("[SYNC] Failed to allocate upload payload for SD sync");
        return false;
    }

    memcpy(payload, header.c_str(), header.length());
    memcpy(payload + header.length(), jpegBuffer, jpegLength);
    memcpy(payload + header.length() + jpegLength, footer.c_str(), footer.length());

    HTTPClient http;
    http.setTimeout(8000);
    http.setReuse(true);
    const bool requestStarted = http.begin(url);
    if (!requestStarted) {
        free(payload);
        free(jpegBuffer);
        Serial.printf("[SYNC] Failed to begin HTTP client for %s\n", url.c_str());
        return false;
    }

    http.addHeader("Content-Type", contentType);
    http.addHeader("Content-Length", String(totalLength));
    const String token = deviceRegistrationGetToken();
    if (token.length() > 0) {
        http.addHeader("x-device-token", token);
    }

    const int httpCode = http.sendRequest("POST", payload, totalLength);
    bool success = httpCode >= 200 && httpCode <= 299;

    if (success) {
        Serial.printf("[SYNC] Uploaded pending frame %s: HTTP %d\n",
            jpegPath.c_str(),
            httpCode);
        deviceStatusRecordBackendSuccess();
        sdManager::deletePendingFile(jpegPath, metaPath);
    } else {
        Serial.printf("[SYNC] Failed to upload pending frame %s: HTTP %d\n",
            jpegPath.c_str(),
            httpCode);
    }

    free(payload);
    free(jpegBuffer);
    http.end();
    return success;
}

void syncTask(void*) {
    while (true) {
        if (!shouldSync()) {
            vTaskDelay(pdMS_TO_TICKS(kSyncLoopMs));
            continue;
        }

        if (millis() - g_lastAttemptMs < kSyncRetryMs) {
            vTaskDelay(pdMS_TO_TICKS(kSyncLoopMs));
            continue;
        }

        g_lastAttemptMs = millis();
        g_syncActive = true;
        const bool uploaded = uploadPendingFile();
        g_syncActive = false;

        if (!uploaded) {
            Serial.println("[SYNC] Waiting before retrying pending-frame upload");
        }

        vTaskDelay(pdMS_TO_TICKS(kSyncLoopMs));
    }
}
}  // namespace

void sdSyncManagerInit() {
    if (g_initialized) {
        return;
    }

    g_initialized = true;
    if (g_syncTask == nullptr) {
        xTaskCreatePinnedToCore(
            syncTask,
            "sd-sync",
            8192,
            nullptr,
            2,
            &g_syncTask,
            1);
    }

    Serial.println("[SYNC] SD synchronization manager started");
}

void sdSyncManagerUpdate() {
    if (!g_initialized) {
        return;
    }

    const uint32_t pendingUploads = sdManager::pendingFileCount();
    if (pendingUploads > 0) {
        Serial.printf("[SYNC] Pending SD frames: %u\n", static_cast<unsigned>(pendingUploads));
    }
}

bool isActive() {
    return g_syncActive;
}

uint32_t pendingCount() {
    return sdManager::pendingFileCount();
}

}  // namespace sdSyncManager
