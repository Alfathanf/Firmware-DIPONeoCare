#include "streaming/stream_server.h"

#include <WiFi.h>
#include <esp_http_server.h>

#include "camera/camera_manager.h"
#include "config.h"

namespace {

// ============================================================
// HTTP SERVER
// ============================================================

httpd_handle_t g_server = nullptr;
bool g_streamRunning = false;

// ============================================================
// STREAM STATISTICS
// ============================================================

uint32_t g_statsWindowStartMs = 0;
uint32_t g_statsFrameCount = 0;

uint64_t g_statsTotalBytes = 0;
uint64_t g_statsTotalCaptureTimeMs = 0;
uint64_t g_statsTotalSendTimeMs = 0;


// ============================================================
// STREAM HANDLER
// ============================================================

esp_err_t streamHandler(httpd_req_t* req) {

    g_streamRunning = true;

    // --------------------------------------------------------
    // Reset statistics for this client session
    // --------------------------------------------------------

    g_statsWindowStartMs = millis();
    g_statsFrameCount = 0;
    g_statsTotalBytes = 0;
    g_statsTotalCaptureTimeMs = 0;
    g_statsTotalSendTimeMs = 0;


    // --------------------------------------------------------
    // Configure MJPEG response
    // --------------------------------------------------------

    esp_err_t ret = httpd_resp_set_type(
        req,
        "multipart/x-mixed-replace; boundary=frame"
    );

    if (ret != ESP_OK) {
        Serial.printf(
            "[STREAM] Failed to set response type: %s\n",
            esp_err_to_name(ret)
        );

        g_streamRunning = false;
        return ret;
    }

    // Allow browser/frontend from another origin.
    httpd_resp_set_hdr(
        req,
        "Access-Control-Allow-Origin",
        "*"
    );

    Serial.println("[STREAM] Client connected");


    // ========================================================
    // MAIN STREAM LOOP
    // ========================================================

    while (true) {

        // ----------------------------------------------------
        // 1. CAPTURE FRAME
        // ----------------------------------------------------

        const uint32_t captureStart = millis();

        camera_fb_t* fb = cameraCapture();

        const uint32_t captureTime =
            millis() - captureStart;


        // ----------------------------------------------------
        // Camera capture failed
        // ----------------------------------------------------

        if (fb == nullptr) {

            Serial.println(
                "[STREAM] Camera capture failed"
            );

            break;
        }


        const uint32_t frameLen = fb->len;


        // ----------------------------------------------------
        // 2. PREPARE MJPEG FRAME HEADER
        // ----------------------------------------------------

        char header[96];

        const int headerLength = snprintf(
            header,
            sizeof(header),

            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %lu\r\n"
            "\r\n",

            static_cast<unsigned long>(frameLen)
        );


        if (headerLength <= 0 ||
            headerLength >= static_cast<int>(sizeof(header))) {

            Serial.println(
                "[STREAM] Failed to create frame header"
            );

            cameraRelease(fb);

            break;
        }


        // ----------------------------------------------------
        // 3. SEND HEADER + JPEG
        // ----------------------------------------------------

        const uint32_t sendStart = millis();


        // Send multipart header.
        ret = httpd_resp_send_chunk(
            req,
            header,
            headerLength
        );


        if (ret != ESP_OK) {

            cameraRelease(fb);

            Serial.printf(
                "[STREAM] Client disconnected while sending header: %s\n",
                esp_err_to_name(ret)
            );

            break;
        }


        // Send JPEG framebuffer directly.
        //
        // IMPORTANT:
        // Do not copy fb->buf into another buffer.
        // The framebuffer is already in PSRAM.
        //
        ret = httpd_resp_send_chunk(
            req,
            reinterpret_cast<const char*>(fb->buf),
            frameLen
        );


        const uint32_t sendTime =
            millis() - sendStart;


        // ----------------------------------------------------
        // 4. RELEASE CAMERA FRAMEBUFFER
        // ----------------------------------------------------

        cameraRelease(fb);


        // ----------------------------------------------------
        // Client disconnected during JPEG transmission
        // ----------------------------------------------------

        if (ret != ESP_OK) {

            Serial.printf(
                "[STREAM] Client disconnected while sending frame: %s\n",
                esp_err_to_name(ret)
            );

            break;
        }


        // ----------------------------------------------------
        // 5. SEND FRAME TERMINATOR
        // ----------------------------------------------------

        ret = httpd_resp_send_chunk(
            req,
            "\r\n",
            2
        );


        if (ret != ESP_OK) {

            Serial.printf(
                "[STREAM] Client disconnected after frame: %s\n",
                esp_err_to_name(ret)
            );

            break;
        }


        // ====================================================
        // 6. UPDATE STATISTICS
        // ====================================================

        g_statsFrameCount++;

        g_statsTotalBytes += frameLen;

        g_statsTotalCaptureTimeMs += captureTime;

        g_statsTotalSendTimeMs += sendTime;


        // ====================================================
        // 7. PERIODIC STATISTICS
        // ====================================================

        const uint32_t now = millis();

        const uint32_t elapsedMs =
            now - g_statsWindowStartMs;


        if (elapsedMs >=
            config::kStreamStatsIntervalMs) {


            const float elapsedSeconds =
                elapsedMs / 1000.0f;


            // ------------------------------------------------
            // FPS
            // ------------------------------------------------

            const float fps =
                g_statsFrameCount > 0
                    ? static_cast<float>(
                        g_statsFrameCount
                    ) / elapsedSeconds
                    : 0.0f;


            // ------------------------------------------------
            // Average JPEG size
            // ------------------------------------------------

            const float avgFrameSizeKb =
                g_statsFrameCount > 0
                    ? (
                        static_cast<float>(
                            g_statsTotalBytes
                        ) /
                        static_cast<float>(
                            g_statsFrameCount
                        ) /
                        1024.0f
                    )
                    : 0.0f;


            // ------------------------------------------------
            // Average camera capture time
            // ------------------------------------------------

            const float avgCaptureTimeMs =
                g_statsFrameCount > 0
                    ? static_cast<float>(
                        g_statsTotalCaptureTimeMs
                    ) /
                    static_cast<float>(
                        g_statsFrameCount
                    )
                    : 0.0f;


            // ------------------------------------------------
            // Average network send time
            // ------------------------------------------------

            const float avgSendTimeMs =
                g_statsFrameCount > 0
                    ? static_cast<float>(
                        g_statsTotalSendTimeMs
                    ) /
                    static_cast<float>(
                        g_statsFrameCount
                    )
                    : 0.0f;


            // =================================================
            // PRINT DIAGNOSTICS
            // =================================================

            Serial.println();
            Serial.println(
                "========================================"
            );

            Serial.println(
                "[STREAM] Stream Statistics"
            );

            Serial.println(
                "========================================"
            );


            Serial.printf(
                "[STREAM] FPS: %.1f\n",
                fps
            );


            Serial.printf(
                "[STREAM] Frames captured: %lu\n",
                static_cast<unsigned long>(
                    g_statsFrameCount
                )
            );


            Serial.printf(
                "[STREAM] Avg JPEG size: %.1f KB\n",
                avgFrameSizeKb
            );


            Serial.printf(
                "[STREAM] Avg capture time: %.1f ms\n",
                avgCaptureTimeMs
            );


            Serial.printf(
                "[STREAM] Avg send time: %.1f ms\n",
                avgSendTimeMs
            );


            // ------------------------------------------------
            // Camera / PSRAM
            // ------------------------------------------------

            Serial.printf(
                "[CAMERA] PSRAM: %s\n",
                cameraIsPsramAvailable()
                    ? "available"
                    : "not available"
            );


            Serial.printf(
                "[MEM] Free heap: %u bytes\n",
                ESP.getFreeHeap()
            );


            Serial.printf(
                "[MEM] Free PSRAM: %u bytes\n",
                ESP.getFreePsram()
            );


            // ------------------------------------------------
            // Wi-Fi
            // ------------------------------------------------

            if (WiFi.status() == WL_CONNECTED) {

                Serial.printf(
                    "[WIFI] RSSI: %d dBm\n",
                    WiFi.RSSI()
                );

            } else {

                Serial.println(
                    "[WIFI] Status: disconnected"
                );
            }


            Serial.println(
                "========================================"
            );


            // ------------------------------------------------
            // Reset statistics window
            // ------------------------------------------------

            g_statsWindowStartMs = now;

            g_statsFrameCount = 0;

            g_statsTotalBytes = 0;

            g_statsTotalCaptureTimeMs = 0;

            g_statsTotalSendTimeMs = 0;
        }
    }


    // ========================================================
    // CLIENT DISCONNECTED / STREAM STOPPED
    // ========================================================

    g_streamRunning = false;


    // Signal end of chunked response.
    //
    // If client already disconnected this can return an error.
    // We intentionally ignore it because the connection is already
    // considered terminated.
    //
    httpd_resp_send_chunk(
        req,
        nullptr,
        0
    );


    Serial.println(
        "[STREAM] Client disconnected"
    );


    return ESP_OK;
}


// ============================================================
// HTTP SERVER CONFIGURATION
// ============================================================

httpd_config_t createServerConfig() {

    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();


    // --------------------------------------------------------
    // Port
    // --------------------------------------------------------

    config.server_port = 80;


    // --------------------------------------------------------
    // URI handlers
    // --------------------------------------------------------

    // Keep enough URI handlers available for future endpoints:
    //
    // /stream
    // /status
    // /config
    // /device
    //
    config.max_uri_handlers = 8;


    // --------------------------------------------------------
    // HTTP server worker task
    // --------------------------------------------------------

    // Streaming is a long-running handler.
    //
    // A reasonable stack size gives the HTTP task enough room
    // without unnecessarily consuming large amounts of RAM.
    //
    config.stack_size = 8192;


    // --------------------------------------------------------
    // Server task priority
    // --------------------------------------------------------

    config.task_priority = 5;


    // --------------------------------------------------------
    // Receive buffer
    // --------------------------------------------------------

    config.recv_wait_timeout = 5;


    // --------------------------------------------------------
    // Send buffer timeout
    // --------------------------------------------------------

    config.send_wait_timeout = 5;


    // --------------------------------------------------------
    // LRU purge
    // --------------------------------------------------------

    // Automatically remove the least recently used client
    // when the server runs out of client slots.
    //
    config.lru_purge_enable = true;


    // --------------------------------------------------------
    // Maximum simultaneous sockets
    // --------------------------------------------------------

    // We currently only need a small number of connections.
    //
    // One is enough for the stream during Phase 1–3,
    // while additional sockets leave room for future endpoints.
    //
    config.max_open_sockets = 5;


    return config;
}

}  // namespace


// ============================================================
// START SERVER
// ============================================================

void streamServerStart(uint16_t port) {

    // --------------------------------------------------------
    // Prevent duplicate server
    // --------------------------------------------------------

    if (g_server != nullptr) {

        Serial.println(
            "[STREAM] Server already running"
        );

        return;
    }


    // --------------------------------------------------------
    // Create optimized configuration
    // --------------------------------------------------------

    httpd_config_t config =
        createServerConfig();


    // Respect port supplied by caller.
    config.server_port = port;


    // --------------------------------------------------------
    // Start HTTP server
    // --------------------------------------------------------

    esp_err_t ret =
        httpd_start(
            &g_server,
            &config
        );


    if (ret != ESP_OK) {

        Serial.printf(
            "[STREAM] HTTP server failed to start: %s\n",
            esp_err_to_name(ret)
        );

        g_server = nullptr;

        return;
    }


    // ========================================================
    // REGISTER /stream
    // ========================================================

    httpd_uri_t streamUri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = streamHandler,
        .user_ctx = nullptr
    };


    ret = httpd_register_uri_handler(
        g_server,
        &streamUri
    );


    if (ret != ESP_OK) {

        Serial.printf(
            "[STREAM] Failed to register /stream: %s\n",
            esp_err_to_name(ret)
        );


        httpd_stop(g_server);

        g_server = nullptr;

        return;
    }


    // ========================================================
    // SERVER STARTED
    // ========================================================

    Serial.println();

    Serial.println(
        "========================================"
    );

    Serial.printf(
        "[STREAM] HTTP server started on port %u\n",
        port
    );


    // --------------------------------------------------------
    // Only print IP if Wi-Fi is already connected.
    // --------------------------------------------------------

    if (WiFi.status() == WL_CONNECTED) {

        Serial.printf(
            "[STREAM] Stream endpoint: http://%s:%u/stream\n",
            WiFi.localIP().toString().c_str(),
            port
        );

    } else {

        Serial.println(
            "[STREAM] Wi-Fi not connected yet"
        );

        Serial.printf(
            "[STREAM] Endpoint will be available on port %u\n",
            port
        );
    }


    Serial.println(
        "========================================"
    );
}


// ============================================================
// STOP SERVER
// ============================================================

void streamServerStop() {

    if (g_server != nullptr) {

        httpd_stop(g_server);

        g_server = nullptr;

        Serial.println(
            "[STREAM] HTTP server stopped"
        );
    }


    g_streamRunning = false;
}


// ============================================================
// SERVER STATUS
// ============================================================

bool streamServerIsRunning() {

    return g_server != nullptr;
}