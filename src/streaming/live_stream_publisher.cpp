#include "streaming/live_stream_publisher.h"

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "camera/camera_manager.h"
#include "config.h"
#include "network/device_registration.h"
#include "network/device_status.h"
#include "wifi/wifi_manager.h"

namespace liveStreamPublisher {
namespace {

constexpr uint32_t kLiveCaptureIntervalMs = 200;
constexpr uint32_t kLiveReconnectBaseMs = 1000;
constexpr uint32_t kLiveReconnectMaxMs = 30000;
constexpr uint32_t kLiveStatsIntervalMs = 5000;
constexpr uint32_t kWebSocketLoopIntervalMs = 5;
constexpr uint32_t kFrameMutexTimeoutMs = 20;

struct LiveFrame {
    uint8_t* data;
    size_t length;
};

WebSocketsClient g_webSocket;

bool g_initialized = false;
bool g_connecting = false;
bool g_connected = false;
bool g_waitingLogged = false;

uint32_t g_lastConnectAttemptMs = 0;
uint32_t g_reconnectDelayMs = kLiveReconnectBaseMs;

uint32_t g_liveFramesSent = 0;
uint32_t g_liveFramesDropped = 0;
uint64_t g_liveBytesSent = 0;

uint32_t g_liveStatsStartMs = 0;
uint32_t g_liveLastStatsMs = 0;

SemaphoreHandle_t g_latestFrameMutex = nullptr;

LiveFrame g_latestFrame = {
    nullptr,
    0
};

TaskHandle_t g_captureTask = nullptr;
TaskHandle_t g_sendTask = nullptr;

String g_liveHeaders;

bool canPublishLive() {
    if (!config::kLiveStreamPublisherEnabled) {
        return false;
    }

    if (!wifiManagerIsConnected()) {
        return false;
    }

    if (!deviceRegistrationIsRegistered()) {
        return false;
    }

    if (deviceRegistrationGetToken().length() == 0) {
        return false;
    }

    if (deviceRegistrationGetDeviceId().length() == 0) {
        return false;
    }

    return true;
}

String buildLiveIngestUrl() {
    const String deviceId =
        deviceRegistrationGetDeviceId();

    if (deviceId.length() == 0) {
        return String();
    }

    String base =
        String(config::kBackendBaseUrl);

    while (base.endsWith("/")) {
        base.remove(base.length() - 1);
    }

    if (base.startsWith("https://")) {
        base.replace("https://", "wss://");
    } else if (base.startsWith("http://")) {
        base.replace("http://", "ws://");
    } else if (
        base.startsWith("wss://") ||
        base.startsWith("ws://")
    ) {
    } else {
        Serial.printf(
            "[LIVE] Unsupported backend URL scheme: %s\n",
            base.c_str()
        );

        return String();
    }

    String path =
        String(config::kLiveStreamIngestPath);

    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    String suffix =
        String(config::kLiveStreamIngestSuffix);

    if (
        suffix.length() > 0 &&
        !suffix.startsWith("/")
    ) {
        suffix = "/" + suffix;
    }

    return base + path + deviceId + suffix;
}

bool parseWebSocketUrl(
    const String& url,
    String& host,
    uint16_t& port,
    String& path,
    bool& secure
) {
    host = "";
    port = 0;
    path = "";
    secure = false;

    String scheme;

    if (url.startsWith("wss://")) {
        scheme = "wss://";
        secure = true;
    } else if (url.startsWith("ws://")) {
        scheme = "ws://";
        secure = false;
    } else {
        return false;
    }

    String urlNoScheme =
        url.substring(scheme.length());

    if (urlNoScheme.length() == 0) {
        return false;
    }

    const int slashIndex =
        urlNoScheme.indexOf('/');

    String hostPort;

    if (slashIndex >= 0) {
        hostPort =
            urlNoScheme.substring(0, slashIndex);

        path =
            urlNoScheme.substring(slashIndex);

        if (path.length() == 0) {
            path = "/";
        }
    } else {
        hostPort = urlNoScheme;
        path = "/";
    }

    if (hostPort.length() == 0) {
        return false;
    }

    const int portSeparator =
        hostPort.indexOf(':');

    if (portSeparator >= 0) {
        host =
            hostPort.substring(0, portSeparator);

        const String portString =
            hostPort.substring(portSeparator + 1);

        const int parsedPort =
            portString.toInt();

        if (
            parsedPort <= 0 ||
            parsedPort > 65535
        ) {
            return false;
        }

        port =
            static_cast<uint16_t>(parsedPort);
    } else {
        host = hostPort;
        port = secure ? 443 : 80;
    }

    if (
        host.length() == 0 ||
        path.length() == 0
    ) {
        return false;
    }

    return true;
}

void freeFrameBuffer(
    LiveFrame& frame
) {
    if (frame.data == nullptr) {
        frame.length = 0;
        return;
    }

    if (cameraIsPsramAvailable()) {
        heap_caps_free(frame.data);
    } else {
        free(frame.data);
    }

    frame.data = nullptr;
    frame.length = 0;
}

void clearLatestFrame() {
    if (g_latestFrameMutex == nullptr) {
        return;
    }

    if (
        xSemaphoreTake(
            g_latestFrameMutex,
            pdMS_TO_TICKS(kFrameMutexTimeoutMs)
        ) != pdTRUE
    ) {
        return;
    }

    if (g_latestFrame.data != nullptr) {
        freeFrameBuffer(g_latestFrame);
    }

    xSemaphoreGive(g_latestFrameMutex);
}

void resetLiveStatistics() {
    g_liveFramesSent = 0;
    g_liveFramesDropped = 0;
    g_liveBytesSent = 0;

    g_liveStatsStartMs = millis();
    g_liveLastStatsMs = millis();
}

void logLiveStats() {
    const uint32_t now = millis();

    if (g_liveLastStatsMs == 0) {
        resetLiveStatistics();
        return;
    }

    if (
        now - g_liveLastStatsMs <
        kLiveStatsIntervalMs
    ) {
        return;
    }

    const uint32_t elapsedMs =
        now - g_liveStatsStartMs;

    const float elapsedSeconds =
        elapsedMs > 0
            ? static_cast<float>(elapsedMs) / 1000.0f
            : 0.0f;

    const float fps =
        elapsedSeconds > 0.0f
            ? static_cast<float>(g_liveFramesSent) /
              elapsedSeconds
            : 0.0f;

    const float avgFrameSizeKb =
        g_liveFramesSent > 0
            ? static_cast<float>(g_liveBytesSent) /
              static_cast<float>(g_liveFramesSent) /
              1024.0f
            : 0.0f;

    Serial.println("========================================");
    Serial.println("[LIVE] Stream statistics");
    Serial.println("========================================");

    Serial.printf(
        "[LIVE] Connected: %s\n",
        g_connected ? "yes" : "no"
    );

    Serial.printf(
        "[LIVE] Connecting: %s\n",
        g_connecting ? "yes" : "no"
    );

    Serial.printf(
        "[LIVE] Frames sent: %u\n",
        static_cast<unsigned>(g_liveFramesSent)
    );

    Serial.printf(
        "[LIVE] Frames dropped: %u\n",
        static_cast<unsigned>(g_liveFramesDropped)
    );

    Serial.printf(
        "[LIVE] Live FPS: %.1f\n",
        fps
    );

    Serial.printf(
        "[LIVE] Avg frame size: %.1f KB\n",
        avgFrameSizeKb
    );

    Serial.printf(
        "[LIVE] Free heap: %u bytes\n",
        ESP.getFreeHeap()
    );

    Serial.printf(
        "[LIVE] Free PSRAM: %u bytes\n",
        ESP.getFreePsram()
    );

    if (wifiManagerIsConnected()) {
        Serial.printf(
            "[LIVE] Wi-Fi RSSI: %d dBm\n",
            WiFi.RSSI()
        );
    }

    Serial.printf(
        "[LIVE] Reconnect delay: %lu ms\n",
        static_cast<unsigned long>(
            g_reconnectDelayMs
        )
    );

    Serial.println("========================================");

    resetLiveStatistics();
}

void onWebSocketEvent(
    WStype_t type,
    uint8_t* payload,
    size_t length
) {
    switch (type) {
        case WStype_CONNECTED: {
            g_connected = true;
            g_connecting = false;
            g_waitingLogged = false;
            g_reconnectDelayMs =
                kLiveReconnectBaseMs;

            Serial.println(
                "[LIVE] Connected to backend live ingestion"
            );

            Serial.println(
                "[LIVE] WebSocket handshake successful"
            );

            break;
        }

        case WStype_DISCONNECTED: {
            g_connected = false;
            g_connecting = false;

            Serial.println(
                "[LIVE] Disconnected from backend live ingestion"
            );

            if (
                g_reconnectDelayMs <
                kLiveReconnectMaxMs
            ) {
                g_reconnectDelayMs *= 2;

                if (
                    g_reconnectDelayMs >
                    kLiveReconnectMaxMs
                ) {
                    g_reconnectDelayMs =
                        kLiveReconnectMaxMs;
                }
            }

            break;
        }

        case WStype_ERROR: {
            g_connected = false;
            g_connecting = false;

            Serial.println(
                "[LIVE] WebSocket error"
            );

            if (
                payload != nullptr &&
                length > 0
            ) {
                Serial.printf(
                    "[LIVE] Error payload: %.*s\n",
                    static_cast<int>(length),
                    reinterpret_cast<char*>(payload)
                );
            }

            break;
        }

        case WStype_TEXT: {
            if (
                payload != nullptr &&
                length > 0
            ) {
                Serial.printf(
                    "[LIVE] Backend message: %.*s\n",
                    static_cast<int>(length),
                    reinterpret_cast<char*>(payload)
                );
            }

            break;
        }

        case WStype_BIN:
            break;

        case WStype_PING:
            break;

        case WStype_PONG:
            break;

        default:
            break;
    }
}

void connectToLiveIngest() {
    if (
        g_connecting ||
        g_connected
    ) {
        return;
    }

    if (!canPublishLive()) {
        return;
    }

    const String deviceId =
        deviceRegistrationGetDeviceId();

    const String deviceToken =
        deviceRegistrationGetToken();

    if (
        deviceId.length() == 0 ||
        deviceToken.length() == 0
    ) {
        return;
    }

    const String liveUrl =
        buildLiveIngestUrl();

    if (liveUrl.length() == 0) {
        Serial.println(
            "[LIVE] Failed to build live ingest URL"
        );

        return;
    }

    String host;
    uint16_t port = 443;
    String path;
    bool secure = false;

    if (
        !parseWebSocketUrl(
            liveUrl,
            host,
            port,
            path,
            secure
        )
    ) {
        Serial.printf(
            "[LIVE] Invalid WebSocket URL: %s\n",
            liveUrl.c_str()
        );

        return;
    }

    String macAddress =
        WiFi.macAddress();

    macAddress.trim();

    if (macAddress.length() == 0) {
        Serial.println(
            "[LIVE] ERROR: Wi-Fi MAC address is empty"
        );

        return;
    }

    g_liveHeaders =
        "x-device-mac: " +
        macAddress +
        "\r\n"
        "x-device-token: " +
        deviceToken;

    g_webSocket.setExtraHeaders(
        g_liveHeaders.c_str()
    );

    Serial.println(
        "[LIVE] Connecting to backend live ingestion..."
    );

    Serial.printf(
        "[LIVE] URL: %s\n",
        liveUrl.c_str()
    );

    Serial.printf(
        "[LIVE] Host: %s\n",
        host.c_str()
    );

    Serial.printf(
        "[LIVE] Port: %u\n",
        static_cast<unsigned>(port)
    );

    Serial.printf(
        "[LIVE] Path: %s\n",
        path.c_str()
    );

    Serial.printf(
        "[LIVE] Protocol: %s\n",
        secure ? "WSS/TLS" : "WS"
    );

    Serial.printf(
        "[LIVE] Device MAC: %s\n",
        macAddress.c_str()
    );

    Serial.println(
        "[LIVE] Device token: configured"
    );

    g_connecting = true;
    g_lastConnectAttemptMs = millis();

    if (secure) {
        g_webSocket.beginSSL(
            host.c_str(),
            port,
            path.c_str(),
            ""
        );
    } else {
        g_webSocket.begin(
            host.c_str(),
            port,
            path.c_str(),
            ""
        );
    }
}

void captureLiveFrameTask(void*) {
    while (true) {
        if (
            !g_initialized ||
            !config::kLiveStreamPublisherEnabled ||
            !g_connected
        ) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        const uint32_t captureStartMs =
            millis();

        camera_fb_t* fb =
            cameraCapture();

        if (fb == nullptr) {
            ++g_liveFramesDropped;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const size_t frameLength =
            fb->len;

        uint8_t* frameCopy = nullptr;

        if (cameraIsPsramAvailable()) {
            frameCopy =
                static_cast<uint8_t*>(
                    heap_caps_malloc(
                        frameLength,
                        MALLOC_CAP_SPIRAM |
                        MALLOC_CAP_8BIT
                    )
                );
        }

        if (frameCopy == nullptr) {
            frameCopy =
                static_cast<uint8_t*>(
                    malloc(frameLength)
                );
        }

        if (frameCopy != nullptr) {
            memcpy(
                frameCopy,
                fb->buf,
                frameLength
            );
        }

        cameraRelease(fb);

        if (frameCopy == nullptr) {
            Serial.println(
                "[LIVE] Failed to allocate live-frame buffer"
            );

            ++g_liveFramesDropped;

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (
            g_latestFrameMutex == nullptr ||
            xSemaphoreTake(
                g_latestFrameMutex,
                pdMS_TO_TICKS(kFrameMutexTimeoutMs)
            ) != pdTRUE
        ) {
            LiveFrame droppedFrame = {
                frameCopy,
                frameLength
            };

            freeFrameBuffer(droppedFrame);

            ++g_liveFramesDropped;

            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (g_latestFrame.data != nullptr) {
            freeFrameBuffer(g_latestFrame);
            ++g_liveFramesDropped;
        }

        g_latestFrame.data =
            frameCopy;

        g_latestFrame.length =
            frameLength;

        xSemaphoreGive(
            g_latestFrameMutex
        );

        const uint32_t captureDurationMs =
            millis() - captureStartMs;

        if (captureDurationMs > 100) {
            Serial.printf(
                "[LIVE] Camera capture took %lu ms\n",
                static_cast<unsigned long>(
                    captureDurationMs
                )
            );
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                kLiveCaptureIntervalMs
            )
        );
    }
}

void sendLiveFrameTask(void*) {
    while (true) {
        if (
            !g_initialized ||
            !config::kLiveStreamPublisherEnabled
        ) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!wifiManagerIsConnected()) {
            if (
                g_connected ||
                g_connecting
            ) {
                g_connected = false;
                g_connecting = false;

                if (g_webSocket.isConnected()) {
                    g_webSocket.disconnect();
                }
            }

            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        g_webSocket.loop();

        if (!g_connected) {
            if (
                !g_connecting &&
                canPublishLive() &&
                millis() -
                    g_lastConnectAttemptMs >=
                    g_reconnectDelayMs
            ) {
                connectToLiveIngest();
            }

            vTaskDelay(
                pdMS_TO_TICKS(
                    kWebSocketLoopIntervalMs
                )
            );

            continue;
        }

        if (
            g_latestFrameMutex == nullptr ||
            xSemaphoreTake(
                g_latestFrameMutex,
                pdMS_TO_TICKS(kFrameMutexTimeoutMs)
            ) != pdTRUE
        ) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (g_latestFrame.data == nullptr) {
            xSemaphoreGive(
                g_latestFrameMutex
            );

            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        LiveFrame frame =
            g_latestFrame;

        g_latestFrame.data = nullptr;
        g_latestFrame.length = 0;

        xSemaphoreGive(
            g_latestFrameMutex
        );

        const uint32_t sendStartMs =
            millis();

        const bool sent =
            g_webSocket.sendBIN(
                frame.data,
                frame.length
            );

        const uint32_t sendDurationMs =
            millis() - sendStartMs;

        if (sent) {
            ++g_liveFramesSent;

            g_liveBytesSent +=
                frame.length;

            Serial.printf(
                "[LIVE] Frame sent: %u bytes in %lu ms\n",
                static_cast<unsigned>(
                    frame.length
                ),
                static_cast<unsigned long>(
                    sendDurationMs
                )
            );
        } else {
            ++g_liveFramesDropped;

            Serial.printf(
                "[LIVE] Failed to send live frame: %u bytes\n",
                static_cast<unsigned>(
                    frame.length
                )
            );

            if (!g_webSocket.isConnected()) {
                g_connected = false;
                g_connecting = false;

                Serial.println(
                    "[LIVE] WebSocket connection lost while sending frame"
                );
            }
        }

        freeFrameBuffer(frame);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void manageWebSocketConnection() {
    if (!config::kLiveStreamPublisherEnabled) {
        return;
    }

    if (!wifiManagerIsConnected()) {
        if (g_webSocket.isConnected()) {
            g_webSocket.disconnect();
        }

        g_connected = false;
        g_connecting = false;

        return;
    }

    if (!canPublishLive()) {
        if (
            g_connected ||
            g_connecting
        ) {
            if (g_webSocket.isConnected()) {
                g_webSocket.disconnect();
            }

            g_connected = false;
            g_connecting = false;
        }

        return;
    }

    if (
        !g_connected &&
        !g_connecting &&
        millis() -
            g_lastConnectAttemptMs >=
            g_reconnectDelayMs
    ) {
        connectToLiveIngest();
    }
}

}

void begin() {
    if (g_initialized) {
        return;
    }

    g_initialized = true;

    Serial.println(
        "[LIVE] Initializing live stream publisher..."
    );

    if (!config::kLiveStreamPublisherEnabled) {
        Serial.println(
            "[LIVE] Publisher disabled in config"
        );

        return;
    }

    if (g_latestFrameMutex == nullptr) {
        g_latestFrameMutex =
            xSemaphoreCreateMutex();
    }

    if (g_latestFrameMutex == nullptr) {
        Serial.println(
            "[LIVE] Failed to create live frame mutex"
        );

        g_initialized = false;
        return;
    }

    g_webSocket.onEvent(
        onWebSocketEvent
    );

    if (g_captureTask == nullptr) {
        const BaseType_t result =
            xTaskCreatePinnedToCore(
                captureLiveFrameTask,
                "live-capture",
                8192,
                nullptr,
                2,
                &g_captureTask,
                1
            );

        if (result != pdPASS) {
            Serial.println(
                "[LIVE] Failed to start live capture task"
            );

            g_captureTask = nullptr;
        }
    }

    if (g_sendTask == nullptr) {
        const BaseType_t result =
            xTaskCreatePinnedToCore(
                sendLiveFrameTask,
                "live-send",
                8192,
                nullptr,
                3,
                &g_sendTask,
                1
            );

        if (result != pdPASS) {
            Serial.println(
                "[LIVE] Failed to start live send task"
            );

            g_sendTask = nullptr;
        }
    }

    g_lastConnectAttemptMs =
        millis() -
        kLiveReconnectBaseMs;

    g_liveStatsStartMs =
        millis();

    g_liveLastStatsMs =
        millis();

    if (!deviceRegistrationIsRegistered()) {
        Serial.println(
            "[LIVE] Waiting for device registration before live connection"
        );

        return;
    }

    Serial.println(
        "[LIVE] Publisher ready; waiting for Wi-Fi and backend connection"
    );
}

void update() {
    if (
        !g_initialized ||
        !config::kLiveStreamPublisherEnabled
    ) {
        return;
    }

    if (!wifiManagerIsConnected()) {
        g_waitingLogged = false;

        if (
            g_connected ||
            g_connecting
        ) {
            g_connected = false;
            g_connecting = false;

            if (g_webSocket.isConnected()) {
                g_webSocket.disconnect();
            }
        }

        return;
    }

    manageWebSocketConnection();

    if (
        !g_waitingLogged &&
        deviceIsOnline()
    ) {
        Serial.println(
            "[LIVE] Device online; waiting for backend live-ingestion connection"
        );

        g_waitingLogged = true;
    }

    logLiveStats();
}

bool isReady() {
    return
        g_initialized &&
        config::kLiveStreamPublisherEnabled &&
        g_connected;
}

}