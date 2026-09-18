#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

// =====================================================
// WIFI
// =====================================================
const char* ssid = "FALITA";
const char* password = "02031964##";

// =====================================================
// CAMERA PIN - ESP32-S3 WROOM N16R8 CAM
// =====================================================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1

#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11

#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13


// =====================================================
// STREAM HANDLER
// =====================================================
static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;

    // Response type MJPEG
    httpd_resp_set_type(
        req,
        "multipart/x-mixed-replace; boundary=frame"
    );

    httpd_resp_set_hdr(
        req,
        "Access-Control-Allow-Origin",
        "*"
    );

    while (true)
    {
        fb = esp_camera_fb_get();

        if (!fb)
        {
            Serial.println("Camera capture failed");
            continue;
        }

        char header[128];

        int header_len = snprintf(
            header,
            sizeof(header),
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n\r\n",
            fb->len
        );

        esp_err_t ret;

        // Kirim header
        ret = httpd_resp_send_chunk(
            req,
            header,
            header_len
        );

        if (ret != ESP_OK)
        {
            esp_camera_fb_return(fb);
            break;
        }

        // Kirim JPEG
        ret = httpd_resp_send_chunk(
            req,
            (const char*)fb->buf,
            fb->len
        );

        esp_camera_fb_return(fb);

        if (ret != ESP_OK)
        {
            break;
        }

        // Frame separator
        ret = httpd_resp_send_chunk(
            req,
            "\r\n",
            2
        );

        if (ret != ESP_OK)
        {
            break;
        }

        // ~20 FPS maksimum
        delay(50);
    }

    // Tutup response
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}


// =====================================================
// START HTTP SERVER
// =====================================================
void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.server_port = 80;

    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t stream_uri = {
            .uri       = "/stream",
            .method    = HTTP_GET,
            .handler   = stream_handler,
            .user_ctx  = NULL
        };

        httpd_register_uri_handler(
            server,
            &stream_uri
        );

        Serial.println("HTTP server started");
    }
    else
    {
        Serial.println("HTTP server failed to start");
    }
}


// =====================================================
// CAMERA SETUP
// =====================================================
bool setupCamera()
{
    camera_config_t config;

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    // -------------------------------------------------
    // Camera Data Pins
    // -------------------------------------------------
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    // -------------------------------------------------
    // Camera Control Pins
    // -------------------------------------------------
    config.pin_xclk  = XCLK_GPIO_NUM;
    config.pin_pclk  = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href  = HREF_GPIO_NUM;

    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    config.pin_pwdn  = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;

    // -------------------------------------------------
    // Clock
    // -------------------------------------------------
    config.xclk_freq_hz = 20000000;

    // -------------------------------------------------
    // JPEG
    // -------------------------------------------------
    config.pixel_format = PIXFORMAT_JPEG;

    // -------------------------------------------------
    // Resolution
    // -------------------------------------------------
    config.frame_size = FRAMESIZE_QVGA;

    // JPEG quality
    // Semakin kecil = kualitas semakin tinggi
    config.jpeg_quality = 12;

    // -------------------------------------------------
    // Frame Buffer
    // -------------------------------------------------
    if (psramFound())
    {
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;

        Serial.println("PSRAM detected");
    }
    else
    {
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        Serial.println("WARNING: PSRAM not detected");
    }

    // -------------------------------------------------
    // Initialize camera
    // -------------------------------------------------
    esp_err_t err = esp_camera_init(&config);

    if (err != ESP_OK)
    {
        Serial.printf(
            "Camera init failed with error 0x%x\n",
            err
        );

        return false;
    }

    // -------------------------------------------------
    // Sensor settings
    // -------------------------------------------------
    sensor_t *s = esp_camera_sensor_get();

    if (s != NULL)
    {
        s->set_framesize(
            s,
            FRAMESIZE_QVGA
        );
    }

    Serial.println("Camera initialized successfully");

    return true;
}


// =====================================================
// WIFI SETUP
// =====================================================
void setupWiFi()
{
    WiFi.mode(WIFI_STA);

    WiFi.begin(
        ssid,
        password
    );

    Serial.print("Connecting to WiFi");

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi Connected");

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}


// =====================================================
// SETUP
// =====================================================
void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("==============================");
    Serial.println("ESP32-S3 CAMERA");
    Serial.println("==============================");

    // -------------------------------------------------
    // PSRAM information
    // -------------------------------------------------
    if (psramFound())
    {
        Serial.println("PSRAM: OK");
        Serial.printf(
            "PSRAM Size: %u bytes\n",
            ESP.getPsramSize()
        );
    }
    else
    {
        Serial.println("PSRAM: NOT FOUND");
    }

    // -------------------------------------------------
    // WiFi
    // -------------------------------------------------
    setupWiFi();

    // -------------------------------------------------
    // Camera
    // -------------------------------------------------
    if (!setupCamera())
    {
        Serial.println("Camera initialization failed!");
        return;
    }

    // -------------------------------------------------
    // HTTP Server
    // -------------------------------------------------
    startCameraServer();

    // -------------------------------------------------
    // Stream URL
    // -------------------------------------------------
    Serial.println();
    Serial.println("==============================");
    Serial.print("STREAM: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/stream");
    Serial.println("==============================");
}


// =====================================================
// LOOP
// =====================================================
void loop()
{
    delay(1000);
}