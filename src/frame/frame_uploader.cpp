#include "frame/frame_uploader.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <new>
#include <sys/time.h>
#include <time.h>

#include "camera/camera_manager.h"
#include "config.h"
#include "network/device_registration.h"
#include "network/device_status.h"
#include "wifi/wifi_manager.h"

namespace {
constexpr uint32_t kClockValidEpoch = 1577836800;
constexpr char kMultipartBoundary[] = "DIPONeoCareFrameBoundary";
constexpr char kNtpServerOne[] = "pool.ntp.org";
constexpr char kNtpServerTwo[] = "time.nist.gov";

TaskHandle_t g_uploaderTask = nullptr;
TaskHandle_t g_captureTask = nullptr;
QueueHandle_t g_uploadQueue = nullptr;
SemaphoreHandle_t g_captureMutex = nullptr;
constexpr size_t kMaxRecentCaptureEvents = 64;

struct FrameUploadItem {
    uint8_t* jpeg;
    size_t jpegLength;
    bool jpegInPsram;
    String captureTimestamp;
    String captureId;
};
struct FrameUploadQueueEntry {
    FrameUploadItem* item;
    uint32_t capturedAtMs;
};

struct UploadStatistics {
    uint32_t successes;
    uint32_t failures;
    uint32_t dropped;
    uint32_t highWaterMark;
    uint64_t totalDurationMs;
    uint32_t minDurationMs;
    uint32_t maxDurationMs;
};
SemaphoreHandle_t g_uploadStatsMutex = nullptr;
UploadStatistics g_uploadStats = {};

struct RecentCaptureEvent {
    String captureId;
    String captureTimestamp;
};

RecentCaptureEvent g_recentCaptureEvents[kMaxRecentCaptureEvents];
size_t g_recentCaptureCount = 0;

class MultipartFrameStream final : public Stream {
public:
    MultipartFrameStream(
        const String& header,
        const uint8_t* image,
        size_t imageLength,
        const String& footer)
        : header_(header),
          image_(image),
          imageLength_(imageLength),
          footer_(footer),
          position_(0),
          totalLength_(header.length() + imageLength + footer.length()) {}

    int available() override {
        return static_cast<int>(totalLength_ - position_);
    }

    int read() override {
        uint8_t value = 0;
        return read(&value, 1) == 1 ? value : -1;
    }

    int peek() override {
        if (position_ >= totalLength_) {
            return -1;
        }

        if (position_ < header_.length()) {
            return static_cast<uint8_t>(header_[position_]);
        }

        size_t imagePosition = position_ - header_.length();
        if (imagePosition < imageLength_) {
            return image_[imagePosition];
        }

        return static_cast<uint8_t>(footer_[imagePosition - imageLength_]);
    }

    size_t readBytes(char* buffer, size_t length) override {
        return read(reinterpret_cast<uint8_t*>(buffer), length);
    }

    size_t write(uint8_t) override {
        return 0;
    }

    size_t read(uint8_t* buffer, size_t length) {
        size_t remaining = totalLength_ - position_;
        size_t count = length < remaining ? length : remaining;
        size_t copied = 0;

        while (copied < count) {
            if (position_ < header_.length()) {
                size_t availableHeader = header_.length() - position_;
                size_t chunk = count - copied < availableHeader
                    ? count - copied
                    : availableHeader;
                memcpy(buffer + copied, header_.c_str() + position_, chunk);
                position_ += chunk;
                copied += chunk;
                continue;
            }

            size_t imagePosition = position_ - header_.length();
            if (imagePosition < imageLength_) {
                size_t availableImage = imageLength_ - imagePosition;
                size_t chunk = count - copied < availableImage
                    ? count - copied
                    : availableImage;
                memcpy(buffer + copied, image_ + imagePosition, chunk);
                position_ += chunk;
                copied += chunk;
                continue;
            }

            size_t footerPosition = imagePosition - imageLength_;
            size_t availableFooter = footer_.length() - footerPosition;
            size_t chunk = count - copied < availableFooter
                ? count - copied
                : availableFooter;
            memcpy(buffer + copied, footer_.c_str() + footerPosition, chunk);
            position_ += chunk;
            copied += chunk;
        }

        return copied;
    }

    size_t length() const {
        return totalLength_;
    }

private:
    const String& header_;
    const uint8_t* image_;
    size_t imageLength_;
    const String& footer_;
    size_t position_;
    size_t totalLength_;
};

bool formatCaptureTimestamp(const struct timeval& captureTime, String& output) {
    if (captureTime.tv_sec < kClockValidEpoch) {
        return false;
    }

    struct tm utcTime;
    if (gmtime_r(&captureTime.tv_sec, &utcTime) == nullptr) {
        return false;
    }

    char timestamp[32];
    int written = snprintf(
        timestamp,
        sizeof(timestamp),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
        utcTime.tm_year + 1900,
        utcTime.tm_mon + 1,
        utcTime.tm_mday,
        utcTime.tm_hour,
        utcTime.tm_min,
        utcTime.tm_sec,
        static_cast<long>(captureTime.tv_usec / 1000));

    if (written <= 0 || written >= static_cast<int>(sizeof(timestamp))) {
        return false;
    }

    output = timestamp;
    return true;
}

String createCaptureId(const String& macAddress, const String& timestamp) {
    String compactMac;
    compactMac.reserve(macAddress.length());
    for (size_t index = 0; index < macAddress.length(); ++index) {
        if (macAddress[index] != ':') {
            compactMac += macAddress[index];
        }
    }

    String compactTimestamp;
    compactTimestamp.reserve(timestamp.length());
    for (size_t index = 0; index < timestamp.length(); ++index) {
        char character = timestamp[index];
        if ((character >= '0' && character <= '9') || character == 'T' || character == 'Z') {
            compactTimestamp += character;
        }
    }

    return compactMac + "_" + compactTimestamp;
}

bool uploadFrame(
    HTTPClient& http,
    const uint8_t* image,
    size_t imageLength,
    const String& captureTimestamp,
    const String& captureId,
    uint32_t& setupMs,
    uint32_t& requestMs,
    uint32_t& responseBodyMs,
    uint32_t& totalUploadMs,
    int& resultCode) {
    const uint32_t uploadStartedAt = millis();
    const uint32_t preparationStartedAt = millis();
    String macAddress = wifiManagerGetMacAddress();
    String contentType = String("multipart/form-data; boundary=") + kMultipartBoundary;
    String header = String("--") + kMultipartBoundary + "\r\n"
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
    String footer = String("\r\n--") + kMultipartBoundary + "--\r\n";

    MultipartFrameStream body(header, image, imageLength, footer);
    http.addHeader("Content-Type", contentType);
    http.addHeader("Content-Length", String(body.length()));

    String token = deviceRegistrationGetToken();
    if (token.length() > 0) {
        http.addHeader("x-device-token", token);
    }
    setupMs = millis() - preparationStartedAt;

    Serial.printf("[FRAME] Upload started: %s\n", captureId.c_str());
    const uint32_t requestStartedAt = millis();
    const int httpCode = http.sendRequest("POST", &body, body.length());
    resultCode = httpCode;
    requestMs = millis() - requestStartedAt;

    String responseBody;
    const uint32_t responseStartedAt = millis();
    responseBody = http.getString();
    responseBodyMs = millis() - responseStartedAt;
    totalUploadMs = millis() - uploadStartedAt;

    Serial.printf("[FRAME] Upload duration: %lu ms\n", static_cast<unsigned long>(totalUploadMs));
    Serial.printf("[FRAME] Upload result: HTTP %d\n", httpCode);

    bool success = httpCode >= 200 && httpCode <= 299;
    if (success) {
        Serial.printf("[FRAME] Upload successful: HTTP %d\n", httpCode);
        Serial.printf("[FRAME] Confirmed capture ID: %s\n", captureId.c_str());
        deviceStatusRecordBackendSuccess();
        frameUploaderRegisterSuccessfulCapture(captureId, captureTimestamp);
    } else {
        Serial.printf("[FRAME] Upload failed: HTTP %d (%s)\n",
            httpCode,
            HTTPClient::errorToString(httpCode).c_str());
        if (responseBody.length() > 0) {
            Serial.printf("[FRAME] Backend response: %s\n", responseBody.c_str());
        }
    }

    return success;
}

FrameUploadItem* captureFrame() {
    camera_fb_t* fb = cameraCapture();
    if (fb == nullptr) {
        Serial.println("[FRAME] Capture failed");
        return nullptr;
    }

    struct timeval captureTime;
    gettimeofday(&captureTime, nullptr);
    String captureTimestamp;
    const bool timestampValid = formatCaptureTimestamp(captureTime, captureTimestamp);
    String macAddress = wifiManagerGetMacAddress();
    String captureId;
    if (timestampValid) {
        captureId = createCaptureId(macAddress, captureTimestamp);
    }

    const size_t frameLength = fb->len;
    uint8_t* frameCopy = nullptr;
    bool frameCopyInPsram = false;

    if (cameraIsPsramAvailable()) {
        frameCopy = static_cast<uint8_t*>(
            heap_caps_malloc(frameLength, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        frameCopyInPsram = frameCopy != nullptr;
    }
    if (frameCopy == nullptr) {
        frameCopy = static_cast<uint8_t*>(malloc(frameLength));
    }

    if (frameCopy != nullptr) {
        memcpy(frameCopy, fb->buf, frameLength);
    }
    cameraRelease(fb);

    if (frameCopy == nullptr) {
        Serial.println("[FRAME] Failed to allocate JPEG buffer");
        return nullptr;
    }

    if (!timestampValid) {
        if (frameCopyInPsram) {
            heap_caps_free(frameCopy);
        } else {
            free(frameCopy);
        }
        Serial.println("[FRAME] Clock is not synchronized; frame discarded");
        return nullptr;
    }

    FrameUploadItem* item = new (std::nothrow) FrameUploadItem{
        frameCopy,
        frameLength,
        frameCopyInPsram,
        captureTimestamp,
        captureId};

    if (item == nullptr) {
        if (frameCopyInPsram) {
            heap_caps_free(frameCopy);
        } else {
            free(frameCopy);
        }
        if (!timestampValid) {
            Serial.println("[FRAME] Clock is not synchronized; frame discarded");
        } else {
            Serial.println("[FRAME] Failed to allocate frame upload item");
        }
        return nullptr;
    }

    Serial.printf("[FRAME] Actual capture timestamp: %s\n", item->captureTimestamp.c_str());
    Serial.printf("[FRAME] Capture ID: %s\n", item->captureId.c_str());
    Serial.printf("[FRAME] JPEG size: %u bytes\n", static_cast<unsigned>(item->jpegLength));
    return item;
}

void destroyFrameUploadItem(FrameUploadItem* item) {
    if (item == nullptr) {
        return;
    }

    if (item->jpegInPsram) {
        heap_caps_free(item->jpeg);
    } else {
        free(item->jpeg);
    }
    delete item;
}

void recordUploadResult(bool success, uint32_t durationMs) {
    if (g_uploadStatsMutex == nullptr ||
        xSemaphoreTake(g_uploadStatsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (success) {
        ++g_uploadStats.successes;
    } else {
        ++g_uploadStats.failures;
    }
    g_uploadStats.totalDurationMs += durationMs;
    if (durationMs < g_uploadStats.minDurationMs) {
        g_uploadStats.minDurationMs = durationMs;
    }
    if (durationMs > g_uploadStats.maxDurationMs) {
        g_uploadStats.maxDurationMs = durationMs;
    }
    xSemaphoreGive(g_uploadStatsMutex);
}

void recordDroppedFrame() {
    if (g_uploadStatsMutex == nullptr ||
        xSemaphoreTake(g_uploadStatsMutex, 0) != pdTRUE) {
        return;
    }
    ++g_uploadStats.dropped;
    xSemaphoreGive(g_uploadStatsMutex);
}

void recordQueueDepth(uint32_t queueDepth) {
    if (g_uploadStatsMutex == nullptr ||
        xSemaphoreTake(g_uploadStatsMutex, 0) != pdTRUE) {
        return;
    }
    if (queueDepth > g_uploadStats.highWaterMark) {
        g_uploadStats.highWaterMark = queueDepth;
    }
    xSemaphoreGive(g_uploadStatsMutex);
}

void printUploadStatistics(uint32_t elapsedMs) {
    UploadStatistics stats;
    if (g_uploadStatsMutex == nullptr ||
        xSemaphoreTake(g_uploadStatsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    stats = g_uploadStats;
    g_uploadStats = {};
    g_uploadStats.minDurationMs = UINT32_MAX;
    xSemaphoreGive(g_uploadStatsMutex);

    const uint32_t totalUploads = stats.successes + stats.failures;
    const uint32_t averageDurationMs = totalUploads == 0
        ? 0
        : static_cast<uint32_t>(stats.totalDurationMs / totalUploads);
    const float throughput = elapsedMs == 0
        ? 0.0f
        : (static_cast<float>(stats.successes) * 1000.0f) / elapsedMs;

    FrameUploadQueueEntry oldestEntry = {};
    uint32_t oldestAgeMs = 0;
    if (g_uploadQueue != nullptr && xQueuePeek(g_uploadQueue, &oldestEntry, 0) == pdTRUE) {
        oldestAgeMs = millis() - oldestEntry.capturedAtMs;
    }

    Serial.println("========================================");
    Serial.println("[FRAME] Uploader Statistics");
    Serial.printf("[FRAME] Successful uploads: %u\n", static_cast<unsigned>(stats.successes));
    Serial.printf("[FRAME] Failed uploads: %u\n", static_cast<unsigned>(stats.failures));
    Serial.printf("[FRAME] Average upload duration: %u ms\n", static_cast<unsigned>(averageDurationMs));
    Serial.printf("[FRAME] Min upload duration: %u ms\n",
        static_cast<unsigned>(stats.minDurationMs == UINT32_MAX ? 0 : stats.minDurationMs));
    Serial.printf("[FRAME] Max upload duration: %u ms\n", static_cast<unsigned>(stats.maxDurationMs));
    Serial.printf("[FRAME] Upload throughput: %.2f frames/sec\n", throughput);
    Serial.printf("[FRAME] Queue depth: %u (high-water %u)\n",
        static_cast<unsigned>(uxQueueMessagesWaiting(g_uploadQueue)),
        static_cast<unsigned>(stats.highWaterMark));
    Serial.printf("[FRAME] Dropped frames: %u\n", static_cast<unsigned>(stats.dropped));
    Serial.printf("[FRAME] Oldest queued frame age: %u ms\n", static_cast<unsigned>(oldestAgeMs));
    Serial.println("========================================");
}

void frameCaptureTask(void*) {
    const TickType_t interval = pdMS_TO_TICKS(config::kFrameUploadIntervalMs);
    TickType_t nextDeadline = xTaskGetTickCount();
    uint32_t lastCaptureMs = 0;
    uint32_t statsStartMs = millis();
    uint32_t statsFrames = 0;
    uint64_t statsIntervalTotalMs = 0;
    uint32_t statsMinIntervalMs = UINT32_MAX;
    uint32_t statsMaxIntervalMs = 0;
    uint32_t scheduleSequence = 0;
    uint32_t uploadStatsStartMs = millis();

    while (true) {
        if (wifiManagerIsConnected() && deviceRegistrationIsRegistered()) {
            Serial.printf("[FRAME] Scheduled capture: +%lu ms\n",
                static_cast<unsigned long>(scheduleSequence * config::kFrameUploadIntervalMs));
            FrameUploadItem* item = captureFrame();
            const uint32_t capturedAtMs = millis();
            if (item != nullptr) {
                if (lastCaptureMs != 0) {
                    const uint32_t captureIntervalMs = capturedAtMs - lastCaptureMs;
                    statsIntervalTotalMs += captureIntervalMs;
                    if (captureIntervalMs < statsMinIntervalMs) {
                        statsMinIntervalMs = captureIntervalMs;
                    }
                    if (captureIntervalMs > statsMaxIntervalMs) {
                        statsMaxIntervalMs = captureIntervalMs;
                    }
                }
                lastCaptureMs = capturedAtMs;
                ++statsFrames;

                FrameUploadQueueEntry entry = {item, capturedAtMs};
                if (xQueueSend(g_uploadQueue, &entry, 0) != pdTRUE) {
                    recordDroppedFrame();
                    Serial.printf("[FRAME] Upload queue full; dropping capture ID %s (dropped %u)\n",
                        item->captureId.c_str(), static_cast<unsigned>(uxQueueMessagesWaiting(g_uploadQueue)));
                    destroyFrameUploadItem(item);
                } else {
                    const uint32_t queueDepth = uxQueueMessagesWaiting(g_uploadQueue);
                    recordQueueDepth(queueDepth);
                }
            }
        } else {
            lastCaptureMs = 0;
        }

        vTaskDelayUntil(&nextDeadline, interval);

        const TickType_t nowTicks = xTaskGetTickCount();
        if (static_cast<TickType_t>(nowTicks - nextDeadline) >= interval) {
            nextDeadline = nowTicks;
        }

        const uint32_t nowMs = millis();
        if (nowMs - statsStartMs >= config::kFrameTimingStatsIntervalMs) {
            Serial.println("========================================");
            Serial.println("[FRAME] Capture Scheduler Statistics");
            Serial.println("========================================");
            Serial.printf("[FRAME] Target interval: %u ms\n", static_cast<unsigned>(config::kFrameUploadIntervalMs));
            Serial.printf("[FRAME] Frames captured: %u\n", static_cast<unsigned>(statsFrames));
            if (statsFrames > 1) {
                Serial.printf("[FRAME] Avg interval: %u ms\n",
                    static_cast<unsigned>(statsIntervalTotalMs / (statsFrames - 1)));
                Serial.printf("[FRAME] Min interval: %u ms\n", static_cast<unsigned>(statsMinIntervalMs));
                Serial.printf("[FRAME] Max interval: %u ms\n", static_cast<unsigned>(statsMaxIntervalMs));
            }
            Serial.printf("[FRAME] Upload queue depth: %u\n",
                static_cast<unsigned>(uxQueueMessagesWaiting(g_uploadQueue)));
            Serial.println("========================================");
            statsStartMs = nowMs;
            statsFrames = 0;
            statsIntervalTotalMs = 0;
            statsMinIntervalMs = UINT32_MAX;
            statsMaxIntervalMs = 0;
        }

        if (nowMs - uploadStatsStartMs >= config::kFrameUploaderStatsIntervalMs) {
            printUploadStatistics(nowMs - uploadStatsStartMs);
            uploadStatsStartMs = nowMs;
        }
    }
}

void frameUploaderTask(void*) {
    HTTPClient http;
    bool clientInitialized = false;
    const String url = String(config::kBackendBaseUrl) + String(config::kFrameUploadPath);

    while (true) {
        FrameUploadQueueEntry entry = {};
        if (xQueueReceive(g_uploadQueue, &entry, portMAX_DELAY) != pdTRUE || entry.item == nullptr) {
            continue;
        }

        if (!wifiManagerIsConnected() || !deviceRegistrationIsRegistered()) {
            Serial.printf("[FRAME] Connectivity lost; dropping queued capture ID %s\n",
                entry.item->captureId.c_str());
            recordDroppedFrame();
            if (clientInitialized) {
                http.setReuse(false);
                http.end();
                clientInitialized = false;
            }
            destroyFrameUploadItem(entry.item);
            continue;
        }

        const uint32_t queueAgeMs = millis() - entry.capturedAtMs;
        if (queueAgeMs > config::kFrameMaxQueuedAgeMs) {
            Serial.printf("[FRAME] Dropping stale queued frame (%lu ms old): %s\n",
                static_cast<unsigned long>(queueAgeMs), entry.item->captureId.c_str());
            recordDroppedFrame();
            destroyFrameUploadItem(entry.item);
            continue;
        }

        if (!clientInitialized) {
            const uint32_t setupStartedAt = millis();
            clientInitialized = http.begin(url);
            const uint32_t clientSetupMs = millis() - setupStartedAt;
            if (!clientInitialized) {
                Serial.printf("[FRAME] HTTPClient begin failed after %lu ms\n",
                    static_cast<unsigned long>(clientSetupMs));
                recordUploadResult(false, clientSetupMs);
                destroyFrameUploadItem(entry.item);
                recordDroppedFrame();
                continue;
            }
            http.setTimeout(8000);
            http.setReuse(true);
            Serial.printf("[FRAME] HTTPClient URL configured in %lu ms\n",
                static_cast<unsigned long>(clientSetupMs));
        }

        uint32_t preparationMs = 0;
        uint32_t requestMs = 0;
        uint32_t responseBodyMs = 0;
        uint32_t totalUploadMs = 0;
        int httpCode = -1;
        const bool success = uploadFrame(
            http,
            entry.item->jpeg,
            entry.item->jpegLength,
            entry.item->captureTimestamp,
            entry.item->captureId,
            preparationMs,
            requestMs,
            responseBodyMs,
            totalUploadMs,
            httpCode);
        Serial.printf("[FRAME] Multipart/header preparation: %lu ms\n",
            static_cast<unsigned long>(preparationMs));
        Serial.printf("[FRAME] HTTP operation (connect/TLS + transmit + status): %lu ms\n",
            static_cast<unsigned long>(requestMs));
        Serial.printf("[FRAME] Response body read: %lu ms\n",
            static_cast<unsigned long>(responseBodyMs));
        recordUploadResult(success, totalUploadMs);

        if (httpCode < 0) {
            http.setReuse(false);
            http.end();
            clientInitialized = false;
        }
        destroyFrameUploadItem(entry.item);
    }
}

}  // namespace

void frameUploaderRegisterSuccessfulCapture(
    const String& captureId,
    const String& captureTimestamp) {
    if (captureId.length() == 0 || captureTimestamp.length() == 0) {
        return;
    }

    if (g_captureMutex == nullptr ||
        xSemaphoreTake(g_captureMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    if (g_recentCaptureCount >= kMaxRecentCaptureEvents) {
        for (size_t index = 1; index < g_recentCaptureCount; ++index) {
            g_recentCaptureEvents[index - 1] = g_recentCaptureEvents[index];
        }
        --g_recentCaptureCount;
    }

    g_recentCaptureEvents[g_recentCaptureCount].captureId = captureId;
    g_recentCaptureEvents[g_recentCaptureCount].captureTimestamp = captureTimestamp;
    ++g_recentCaptureCount;
    xSemaphoreGive(g_captureMutex);
}

bool frameUploaderGetCaptureIdsForWindow(
    const String& startedAt,
    const String& endedAt,
    String* captureIds,
    String* captureTimestamps,
    size_t maxCaptureIds,
    size_t* actualCaptureCount) {
    if (captureIds == nullptr || captureTimestamps == nullptr ||
        maxCaptureIds == 0 || actualCaptureCount == nullptr) {
        return false;
    }

    *actualCaptureCount = 0;
    if (g_captureMutex == nullptr ||
        xSemaphoreTake(g_captureMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    for (size_t index = 0; index < g_recentCaptureCount && *actualCaptureCount < maxCaptureIds; ++index) {
        const RecentCaptureEvent& event = g_recentCaptureEvents[index];
        if (event.captureTimestamp.compareTo(startedAt) < 0 ||
            event.captureTimestamp.compareTo(endedAt) >= 0) {
            continue;
        }

        bool duplicate = false;
        for (size_t checkIndex = 0; checkIndex < *actualCaptureCount; ++checkIndex) {
            if (captureIds[checkIndex].equals(event.captureId)) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate) {
            captureIds[*actualCaptureCount] = event.captureId;
            captureTimestamps[*actualCaptureCount] = event.captureTimestamp;
            ++(*actualCaptureCount);
        }
    }
    xSemaphoreGive(g_captureMutex);

    return *actualCaptureCount > 0;
}

void frameUploaderInit() {
    configTime(0, 0, kNtpServerOne, kNtpServerTwo);
    if (g_captureMutex == nullptr) {
        g_captureMutex = xSemaphoreCreateMutex();
    }
    if (g_captureMutex == nullptr) {
        Serial.println("[FRAME] ERROR: Failed to create capture confirmation mutex");
        return;
    }
    if (g_uploadStatsMutex == nullptr) {
        g_uploadStatsMutex = xSemaphoreCreateMutex();
    }
    if (g_uploadStatsMutex == nullptr) {
        Serial.println("[FRAME] ERROR: Failed to create uploader statistics mutex");
        return;
    }
    g_uploadStats.minDurationMs = UINT32_MAX;
    if (g_uploadQueue == nullptr) {
        g_uploadQueue = xQueueCreate(
            config::kFrameUploadQueueLength,
            sizeof(FrameUploadQueueEntry));
    }
    if (g_uploadQueue == nullptr) {
        Serial.println("[FRAME] ERROR: Failed to create bounded upload queue");
        return;
    }
    if (g_uploaderTask == nullptr) {
        if (xTaskCreatePinnedToCore(
            frameUploaderTask,
            "frame-uploader",
            8192,
            nullptr,
            1,
            &g_uploaderTask,
            1) != pdPASS) {
            g_uploaderTask = nullptr;
            Serial.println("[FRAME] ERROR: Failed to start upload task");
            return;
        }
    }
    if (g_captureTask == nullptr) {
        if (xTaskCreatePinnedToCore(
            frameCaptureTask,
            "frame-capture",
            6144,
            nullptr,
            2,
            &g_captureTask,
            1) != pdPASS) {
            g_captureTask = nullptr;
            Serial.println("[FRAME] ERROR: Failed to start capture scheduler task");
            return;
        }
    }
    Serial.println("[FRAME] Uploader initialized");
}

void frameUploaderUpdate() {
}
