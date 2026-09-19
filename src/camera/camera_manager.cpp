#include "camera/camera_manager.h"

// ESP32-S3 WROOM N16R8 camera pin mapping that has already been verified.
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1

#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5

#define Y9_GPIO_NUM 16
#define Y8_GPIO_NUM 17
#define Y7_GPIO_NUM 18
#define Y6_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y4_GPIO_NUM 8
#define Y3_GPIO_NUM 9
#define Y2_GPIO_NUM 11

#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 13

namespace {
constexpr uint8_t kJpegQuality = 12;
constexpr framesize_t kFrameSize = FRAMESIZE_QVGA;
constexpr uint32_t kXclkFrequencyHz = 20000000;

bool g_cameraReady = false;
bool g_psramAvailable = false;

void printCameraError(const char* context, esp_err_t err) {
    Serial.printf("[CAMERA] %s failed with error 0x%x (%s)\n", context, err, esp_err_to_name(err));
}

void configureCameraPins(camera_config_t& config) {
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;

    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
}

void configureCameraFrameBuffer(camera_config_t& config) {
    g_psramAvailable = psramFound();

    if (g_psramAvailable) {
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
        Serial.println("[CAMERA] PSRAM detected; using PSRAM frame buffers");
    } else {
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        Serial.println("[CAMERA] PSRAM not detected; using DRAM frame buffers");
    }
}

}  // namespace

bool cameraInit() {
    if (g_cameraReady) {
        return true;
    }

    camera_config_t config;
    memset(&config, 0, sizeof(config));

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.xclk_freq_hz = kXclkFrequencyHz;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = kFrameSize;
    config.jpeg_quality = kJpegQuality;

    configureCameraPins(config);
    configureCameraFrameBuffer(config);

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        printCameraError("esp_camera_init", err);
        g_cameraReady = false;
        return false;
    }

    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        printCameraError("esp_camera_sensor_get", ESP_ERR_CAMERA_NOT_SUPPORTED);
        g_cameraReady = false;
        return false;
    }

    sensor->set_framesize(sensor, kFrameSize);
    sensor->set_quality(sensor, kJpegQuality);
    sensor->set_contrast(sensor, 2);
    sensor->set_brightness(sensor, 2);

    g_cameraReady = true;
    cameraPrintDebugInfo();
    return true;
}

camera_fb_t* cameraCapture() {
    if (!g_cameraReady) {
        return nullptr;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
        Serial.println("[CAMERA] Failed to capture a frame");
        return nullptr;
    }

    return fb;
}

void cameraRelease(camera_fb_t* fb) {
    if (fb != nullptr) {
        esp_camera_fb_return(fb);
    }
}

bool cameraIsReady() {
    return g_cameraReady;
}

bool cameraIsPsramAvailable() {
    return g_psramAvailable;
}

void cameraPrintDebugInfo() {
    Serial.println("[CAMERA] Camera initialized");
    Serial.printf("[CAMERA] PSRAM: %s\n", g_psramAvailable ? "available" : "not available");
    Serial.printf("[CAMERA] Resolution: QVGA\n");
    Serial.printf("[CAMERA] JPEG quality: %d\n", kJpegQuality);
    Serial.printf("[CAMERA] Frame format: JPEG\n");
    if (g_psramAvailable) {
        Serial.printf("[CAMERA] PSRAM size: %u bytes\n", ESP.getPsramSize());
    }
}
