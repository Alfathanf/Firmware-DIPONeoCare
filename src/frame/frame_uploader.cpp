#include "frame/frame_uploader.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

#include "camera/camera_manager.h"
#include "config.h"
#include "network/device_registration.h"
#include "wifi/wifi_manager.h"

namespace {
constexpr uint32_t kClockValidEpoch = 1577836800;
constexpr char kMultipartBoundary[] = "DIPONeoCareFrameBoundary";
constexpr char kNtpServerOne[] = "pool.ntp.org";
constexpr char kNtpServerTwo[] = "time.nist.gov";

TaskHandle_t g_uploaderTask = nullptr;

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
    const uint8_t* image,
    size_t imageLength,
    const String& captureTimestamp,
    const String& captureId) {
    String macAddress = wifiManagerGetMacAddress();
    String url = String(config::kBackendBaseUrl) + String(config::kFrameUploadPath);
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
    HTTPClient http;
    http.setTimeout(5000);

    int httpCode = -1;
    String responseBody;
    bool requestStarted = http.begin(url);
    if (requestStarted) {
        http.addHeader("Content-Type", contentType);
        http.addHeader("Content-Length", String(body.length()));

        String token = deviceRegistrationGetToken();
        if (token.length() > 0) {
            http.addHeader("x-device-token", token);
        }

        httpCode = http.sendRequest("POST", &body, body.length());
        responseBody = http.getString();
    }

    bool success = httpCode >= 200 && httpCode <= 299;
    if (success) {
        Serial.printf("[FRAME] Upload successful: HTTP %d\n", httpCode);
    } else {
        Serial.printf("[FRAME] Upload failed: HTTP %d\n", httpCode);
        if (responseBody.length() > 0) {
            Serial.printf("[FRAME] Backend response: %s\n", responseBody.c_str());
        }
    }

    http.end();
    return success;
}

void captureAndUploadFrame() {
    Serial.println("[FRAME] Capturing frame...");

    struct timeval captureTime;
    gettimeofday(&captureTime, nullptr);

    camera_fb_t* fb = cameraCapture();
    if (fb == nullptr) {
        Serial.println("[FRAME] Capture failed");
        return;
    }

    String captureTimestamp;
    bool timestampValid = formatCaptureTimestamp(captureTime, captureTimestamp);
    String macAddress = wifiManagerGetMacAddress();
    String captureId = createCaptureId(macAddress, captureTimestamp);
    size_t frameLength = fb->len;
    uint8_t* frameCopy = nullptr;

    if (cameraIsPsramAvailable()) {
        frameCopy = static_cast<uint8_t*>(
            heap_caps_malloc(frameLength, MALLOC_CAP_SPIRAM));
    }
    if (frameCopy == nullptr) {
        frameCopy = static_cast<uint8_t*>(malloc(frameLength));
    }

    if (frameCopy == nullptr) {
        cameraRelease(fb);
        Serial.println("[FRAME] Failed to allocate JPEG buffer");
        return;
    }

    memcpy(frameCopy, fb->buf, frameLength);
    cameraRelease(fb);

    if (!timestampValid) {
        free(frameCopy);
        Serial.println("[FRAME] Clock is not synchronized; frame discarded");
        return;
    }

    Serial.printf("[FRAME] Capture timestamp: %s\n", captureTimestamp.c_str());
    Serial.printf("[FRAME] Capture ID: %s\n", captureId.c_str());
    Serial.printf("[FRAME] JPEG size: %u bytes\n", static_cast<unsigned>(frameLength));
    Serial.println("[FRAME] Uploading frame...");
    Serial.printf("[FRAME] Upload URL: %s\n", (String(config::kBackendBaseUrl) + String(config::kFrameUploadPath)).c_str());

    uploadFrame(frameCopy, frameLength, captureTimestamp, captureId);
    free(frameCopy);
}

void frameUploaderTask(void*) {
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(config::kFrameUploadIntervalMs);

    while (true) {
        if (wifiManagerIsConnected() && deviceRegistrationIsRegistered()) {
            captureAndUploadFrame();
        }

        vTaskDelayUntil(&lastWakeTime, interval);
    }
}

}  // namespace

void frameUploaderInit() {
    configTime(0, 0, kNtpServerOne, kNtpServerTwo);
    if (g_uploaderTask == nullptr) {
        xTaskCreatePinnedToCore(
            frameUploaderTask,
            "frame-uploader",
            8192,
            nullptr,
            1,
            &g_uploaderTask,
            1);
    }
    Serial.println("[FRAME] Uploader initialized");
}

void frameUploaderUpdate() {
}
