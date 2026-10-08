#include "audio/audio_manager.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <driver/adc.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "frame/frame_uploader.h"
#include "network/device_registration.h"
#include "network/device_status.h"
#include "wifi/wifi_manager.h"

namespace {
constexpr int MIC_ADC_PIN = 1;
constexpr adc1_channel_t kMicAdcChannel = ADC1_CHANNEL_0;
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kRecordingSeconds = 5;
constexpr uint32_t kSampleCount = kSampleRate * kRecordingSeconds;
constexpr uint32_t kPcmBytes = kSampleCount * sizeof(int16_t);
constexpr uint32_t kWavHeaderBytes = 44;
constexpr uint32_t kWavBytes = kWavHeaderBytes + kPcmBytes;
constexpr uint32_t kClockValidEpoch = 1577836800;
constexpr int32_t kPcmScale = 16;
constexpr char kMultipartBoundary[] = "DIPONeoCareAudioBoundary";

TaskHandle_t g_audioTask = nullptr;
hw_timer_t* g_sampleTimer = nullptr;
uint8_t* g_wavBuffer = nullptr;
volatile uint32_t g_sampleIndex = 0;
volatile bool g_sampleComplete = false;
volatile bool g_recording = false;
volatile bool g_uploading = false;
volatile bool g_started = false;

class MultipartAudioStream final : public Stream {
public:
    MultipartAudioStream(
        const String& header,
        const uint8_t* wav,
        size_t wavLength,
        const String& footer)
        : header_(header),
          wav_(wav),
          wavLength_(wavLength),
          footer_(footer),
          position_(0),
          totalLength_(header.length() + wavLength + footer.length()) {}

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

        const size_t wavPosition = position_ - header_.length();
        if (wavPosition < wavLength_) {
            return wav_[wavPosition];
        }

        return static_cast<uint8_t>(footer_[wavPosition - wavLength_]);
    }

    size_t readBytes(char* buffer, size_t length) override {
        return read(reinterpret_cast<uint8_t*>(buffer), length);
    }

    size_t write(uint8_t) override {
        return 0;
    }

    size_t read(uint8_t* buffer, size_t length) {
        const size_t remaining = totalLength_ - position_;
        const size_t count = length < remaining ? length : remaining;
        size_t copied = 0;

        while (copied < count) {
            if (position_ < header_.length()) {
                const size_t sourcePosition = position_;
                const size_t available = header_.length() - sourcePosition;
                const size_t chunk = count - copied < available
                    ? count - copied
                    : available;
                memcpy(buffer + copied, header_.c_str() + sourcePosition, chunk);
                position_ += chunk;
                copied += chunk;
                continue;
            }

            const size_t wavPosition = position_ - header_.length();
            if (wavPosition < wavLength_) {
                const size_t available = wavLength_ - wavPosition;
                const size_t chunk = count - copied < available
                    ? count - copied
                    : available;
                memcpy(buffer + copied, wav_ + wavPosition, chunk);
                position_ += chunk;
                copied += chunk;
                continue;
            }

            const size_t footerPosition = wavPosition - wavLength_;
            const size_t available = footer_.length() - footerPosition;
            const size_t chunk = count - copied < available
                ? count - copied
                : available;
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
    const uint8_t* wav_;
    size_t wavLength_;
    const String& footer_;
    size_t position_;
    size_t totalLength_;
};

void writeLittleEndian16(uint8_t* destination, uint16_t value) {
    destination[0] = static_cast<uint8_t>(value & 0xff);
    destination[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void writeLittleEndian32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value & 0xff);
    destination[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    destination[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    destination[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void writeWavHeader(uint8_t* buffer) {
    memcpy(buffer + 0, "RIFF", 4);
    writeLittleEndian32(buffer + 4, kWavBytes - 8);
    memcpy(buffer + 8, "WAVE", 4);
    memcpy(buffer + 12, "fmt ", 4);
    writeLittleEndian32(buffer + 16, 16);
    writeLittleEndian16(buffer + 20, 1);
    writeLittleEndian16(buffer + 22, 1);
    writeLittleEndian32(buffer + 24, kSampleRate);
    writeLittleEndian32(buffer + 28, kSampleRate * sizeof(int16_t));
    writeLittleEndian16(buffer + 32, sizeof(int16_t));
    writeLittleEndian16(buffer + 34, 16);
    memcpy(buffer + 36, "data", 4);
    writeLittleEndian32(buffer + 40, kPcmBytes);
}

bool formatTimestamp(const struct timeval& captureTime, String& output) {
    if (captureTime.tv_sec < kClockValidEpoch) {
        return false;
    }

    struct tm utcTime;
    if (gmtime_r(&captureTime.tv_sec, &utcTime) == nullptr) {
        return false;
    }

    char timestamp[32];
    const int written = snprintf(
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

int16_t convertSample(uint16_t rawSample, int32_t dcOffset) {
    const int32_t raw = rawSample & 0x0fff;
    int32_t centered = (raw - dcOffset) * kPcmScale;
    if (centered > 32767) {
        centered = 32767;
    } else if (centered < -32768) {
        centered = -32768;
    }
    return static_cast<int16_t>(centered);
}

String createAudioWindowId(const String& macAddress, const struct timeval& recordingStart);

void IRAM_ATTR sampleTimerCallback() {
    if (!g_recording || g_sampleIndex >= kSampleCount) {
        return;
    }

    uint16_t* rawSamples = reinterpret_cast<uint16_t*>(
        g_wavBuffer + kWavHeaderBytes);
    rawSamples[g_sampleIndex++] = static_cast<uint16_t>(
        adc1_get_raw(kMicAdcChannel));

    if (g_sampleIndex >= kSampleCount) {
        g_sampleComplete = true;
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(g_audioTask, &higherPriorityTaskWoken);
        if (higherPriorityTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

bool configureAdc() {
    if (adc1_config_width(ADC_WIDTH_BIT_12) != ESP_OK ||
        adc1_config_channel_atten(kMicAdcChannel, ADC_ATTEN_DB_12) != ESP_OK) {
        Serial.println("[AUDIO] ERROR: ADC configuration failed");
        return false;
    }

    g_sampleTimer = timerBegin(0, 40, true);
    if (g_sampleTimer == nullptr) {
        Serial.println("[AUDIO] ERROR: Hardware sample timer initialization failed");
        return false;
    }

    timerAttachInterrupt(g_sampleTimer, &sampleTimerCallback, true);
    timerAlarmWrite(g_sampleTimer, 125, true);
    return true;
}

bool recordWav(
    struct timeval& recordingStart,
    String& startedAt,
    String& audioWindowId,
    String& endedAt) {
    gettimeofday(&recordingStart, nullptr);
    if (!formatTimestamp(recordingStart, startedAt)) {
        Serial.println("[AUDIO] ERROR: Recording timestamp unavailable; audio discarded");
        return false;
    }

    audioWindowId = createAudioWindowId(wifiManagerGetMacAddress(), recordingStart);
    if (audioWindowId.length() == 0) {
        Serial.println("[AUDIO] ERROR: Audio window ID generation failed; audio discarded");
        return false;
    }

    struct timeval recordingEnd = recordingStart;
    recordingEnd.tv_sec += kRecordingSeconds;
    if (!formatTimestamp(recordingEnd, endedAt)) {
        Serial.println("[AUDIO] ERROR: Recording end timestamp unavailable");
        return false;
    }

    Serial.printf("[AUDIO] Window ID: %s\n", audioWindowId.c_str());
    Serial.printf("[AUDIO] Window start: %s\n", startedAt.c_str());
    Serial.printf("[AUDIO] Window end:   %s\n", endedAt.c_str());

    g_sampleIndex = 0;
    g_sampleComplete = false;
    g_recording = true;
    timerWrite(g_sampleTimer, 0);
    timerAlarmEnable(g_sampleTimer);

    const uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(6000));
    timerAlarmDisable(g_sampleTimer);
    g_recording = false;

    if (notified == 0 || !g_sampleComplete || g_sampleIndex != kSampleCount) {
        return false;
    }

    uint16_t* rawSamples = reinterpret_cast<uint16_t*>(
        g_wavBuffer + kWavHeaderBytes);
    uint64_t total = 0;
    for (uint32_t index = 0; index < kSampleCount; ++index) {
        total += rawSamples[index] & 0x0fff;
    }
    const int32_t dcOffset = static_cast<int32_t>(total / kSampleCount);

    int16_t* pcm = reinterpret_cast<int16_t*>(g_wavBuffer + kWavHeaderBytes);
    for (uint32_t index = 0; index < kSampleCount; ++index) {
        pcm[index] = convertSample(rawSamples[index], dcOffset);
    }
    writeWavHeader(g_wavBuffer);
    return true;
}

String createAudioWindowId(const String& macAddress, const struct timeval& recordingStart) {
    String compactMac;
    compactMac.reserve(macAddress.length());
    for (size_t index = 0; index < macAddress.length(); ++index) {
        if (macAddress[index] != ':') {
            compactMac += macAddress[index];
        }
    }

    struct tm utcTime;
    if (gmtime_r(&recordingStart.tv_sec, &utcTime) == nullptr) {
        return String();
    }

    char windowTimestamp[32];
    const int written = snprintf(
        windowTimestamp,
        sizeof(windowTimestamp),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
        utcTime.tm_year + 1900,
        utcTime.tm_mon + 1,
        utcTime.tm_mday,
        utcTime.tm_hour,
        utcTime.tm_min,
        utcTime.tm_sec,
        static_cast<long>(recordingStart.tv_usec / 1000));

    if (written <= 0 || written >= static_cast<int>(sizeof(windowTimestamp))) {
        return String();
    }

    return compactMac + "_" + String(windowTimestamp);
}

String buildCaptureIdsText(const String* captureIds, size_t captureCount) {
    String text;
    for (size_t index = 0; index < captureCount; ++index) {
        if (index > 0) {
            text += ", ";
        }
        text += captureIds[index];
    }
    return text;
}

bool uploadWav(
    const String& audioWindowId,
    const String& startedAt,
    const String* captureIds,
    size_t captureCount) {
    if (!deviceIsOnline()) {
        Serial.println("[AUDIO] Upload skipped: device is not online");
        return false;
    }

    if (captureCount != 5) {
        Serial.printf("[AUDIO] ERROR: Only %u valid capture IDs available\n", static_cast<unsigned>(captureCount));
        Serial.println("[AUDIO] Required: 5");
        Serial.println("[AUDIO] Audio upload postponed/skipped");
        return false;
    }

    const String url = String(config::kBackendBaseUrl) + String(config::kAudioUploadPath);
    const String macAddress = wifiManagerGetMacAddress();
    const String contentType = String("multipart/form-data; boundary=") + kMultipartBoundary;
    String header = String("--") + kMultipartBoundary + "\r\n"
        + "Content-Disposition: form-data; name=\"macAddress\"\r\n\r\n"
        + macAddress + "\r\n"
        + "--" + kMultipartBoundary + "\r\n"
        + "Content-Disposition: form-data; name=\"audioWindowId\"\r\n\r\n"
        + audioWindowId + "\r\n"
        + "--" + kMultipartBoundary + "\r\n"
        + "Content-Disposition: form-data; name=\"startedAt\"\r\n\r\n"
        + startedAt + "\r\n"
        + "--" + kMultipartBoundary + "\r\n"
        + "Content-Disposition: form-data; name=\"durationSeconds\"\r\n\r\n"
        + "5\r\n";

    for (size_t index = 0; index < captureCount; ++index) {
        header += String("--") + kMultipartBoundary + "\r\n"
            + "Content-Disposition: form-data; name=\"captureIds\"\r\n\r\n"
            + captureIds[index] + "\r\n";
    }

    header += String("--") + kMultipartBoundary + "\r\n"
        + "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.wav\"\r\n"
        + "Content-Type: audio/wav\r\n\r\n";

    const String footer = String("\r\n--") + kMultipartBoundary + "--\r\n";
    MultipartAudioStream body(header, g_wavBuffer, kWavBytes, footer);

    HTTPClient http;
    http.setTimeout(5000);
    int httpCode = -1;
    String responseBody;
    const bool requestStarted = http.begin(url);
    if (requestStarted) {
        http.addHeader("Content-Type", contentType);
        http.addHeader("Content-Length", String(body.length()));

        const String token = deviceRegistrationGetToken();
        if (token.length() > 0) {
            http.addHeader("x-device-token", token);
        }

        httpCode = http.sendRequest("POST", &body, body.length());
        const bool shouldReadBody = httpCode != HTTP_CODE_ACCEPTED &&
            httpCode != HTTP_CODE_NO_CONTENT &&
            httpCode != HTTP_CODE_RESET_CONTENT;
        if (shouldReadBody) {
            responseBody = http.getString();
        }
    }

    const bool success = httpCode >= 200 && httpCode < 300;
    if (success) {
        Serial.printf("[AUDIO] Upload successful: HTTP %d\n", httpCode);
        deviceStatusRecordBackendSuccess();
    } else {
        Serial.printf("[AUDIO] ERROR: Audio upload failed: HTTP %d\n", httpCode);
        if (responseBody.length() > 0) {
            Serial.printf("[AUDIO] Backend response: %s\n", responseBody.c_str());
        }
    }

    http.end();
    return success;
}

void audioTask(void*) {
    bool waitingMessagePrinted = false;
    while (true) {
        if (g_wavBuffer == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!deviceIsOnline()) {
            if (!waitingMessagePrinted) {
                Serial.println("[AUDIO] Waiting for device online state...");
                waitingMessagePrinted = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        waitingMessagePrinted = false;

        g_recording = true;
        Serial.println("[AUDIO] Starting audio window");
        struct timeval recordingStart;
        String startedAt;
        String audioWindowId;
        String endedAt;
        const bool recorded = recordWav(
            recordingStart,
            startedAt,
            audioWindowId,
            endedAt);
        g_recording = false;

        if (!recorded) {
            Serial.println("[AUDIO] ERROR: Recording failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!deviceIsOnline()) {
            Serial.println("[AUDIO] Device went offline during recording; audio discarded");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        Serial.printf("[AUDIO] Recording timestamp: %s\n", startedAt.c_str());
        Serial.printf("[AUDIO] Samples recorded: %u\n", static_cast<unsigned>(kSampleCount));
        Serial.printf("[AUDIO] PCM size: %u bytes\n", static_cast<unsigned>(kPcmBytes));
        Serial.printf("[AUDIO] WAV size: %u bytes\n", static_cast<unsigned>(kWavBytes));
        Serial.println("[AUDIO] Audio recording complete");

        String captureIds[5];
        String captureTimestamps[5];
        size_t captureCount = 0;
        size_t reportedCaptureCount = 0;
        const uint32_t confirmationStartMs = millis();
        Serial.println("[AUDIO] Waiting for frame confirmations...");
        while (captureCount < 5 &&
               millis() - confirmationStartMs < config::kAudioFrameConfirmationWaitMs) {
            frameUploaderGetCaptureIdsForWindow(
                startedAt,
                endedAt,
                captureIds,
                captureTimestamps,
                5,
                &captureCount);

            while (reportedCaptureCount < captureCount) {
                Serial.println("[AUDIO] Frame confirmation received");
                Serial.printf("[AUDIO] Capture ID: %s\n",
                    captureIds[reportedCaptureCount].c_str());
                Serial.printf("[AUDIO] Frame timestamp: %s\n",
                    captureTimestamps[reportedCaptureCount].c_str());
                Serial.println("[AUDIO] Belongs to current window: YES");
                ++reportedCaptureCount;
                Serial.printf("[AUDIO] Valid capture IDs: %u/5\n",
                    static_cast<unsigned>(reportedCaptureCount));
            }

            if (captureCount < 5) {
                vTaskDelay(pdMS_TO_TICKS(config::kAudioFrameConfirmationPollMs));
            }
        }

        if (captureCount != 5) {
            Serial.println("[AUDIO] Frame confirmation timeout");
            Serial.printf("[AUDIO] Valid capture IDs: %u/5\n", static_cast<unsigned>(captureCount));
            Serial.println("[AUDIO] Audio upload skipped");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        Serial.println("[AUDIO] Duration: 5 seconds");
        Serial.printf("[AUDIO] Valid capture IDs: %u/5\n", static_cast<unsigned>(captureCount));
        Serial.println("[AUDIO] Uploading audio...");
        Serial.printf("[AUDIO] Upload URL: %s\n", (String(config::kBackendBaseUrl) + String(config::kAudioUploadPath)).c_str());

        g_uploading = true;
        const bool uploaded = uploadWav(audioWindowId, startedAt, captureIds, captureCount);
        g_uploading = false;

        if (!uploaded) {
            Serial.println("[AUDIO] Audio upload failed; waiting for next window");
        }
    }
}

}  // namespace

namespace audioManager {

bool begin() {
    Serial.println("[AUDIO] Initializing microphone...");
    Serial.printf("[AUDIO] Microphone initialized on GPIO%d\n", MIC_ADC_PIN);
    Serial.printf("[AUDIO] Sample rate: %u Hz\n", static_cast<unsigned>(kSampleRate));
    Serial.println("[AUDIO] Channels: 1");
    Serial.println("[AUDIO] Bits: 16");

    if (!configureAdc()) {
        Serial.println("[AUDIO] ERROR: Microphone initialization failed");
        return false;
    }

    if (psramFound()) {
        g_wavBuffer = static_cast<uint8_t*>(
            heap_caps_malloc(kWavBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (g_wavBuffer == nullptr) {
        g_wavBuffer = static_cast<uint8_t*>(malloc(kWavBytes));
    }
    if (g_wavBuffer == nullptr) {
        Serial.println("[AUDIO] ERROR: Failed to allocate audio buffer");
        return false;
    }

    g_started = true;
    if (xTaskCreatePinnedToCore(
            audioTask,
            "audio-manager",
            8192,
            nullptr,
            1,
            &g_audioTask,
            1) != pdPASS) {
        free(g_wavBuffer);
        g_wavBuffer = nullptr;
        g_started = false;
        Serial.println("[AUDIO] ERROR: Failed to start audio task");
        return false;
    }

    return true;
}

void update() {
}

bool isRecording() {
    return g_recording;
}

bool isUploading() {
    return g_uploading;
}

void startRecording() {
}

}  // namespace audioManager
