#include "network/device_status.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "config.h"
#include "network/device_registration.h"
#include "wifi/wifi_manager.h"

namespace {
portMUX_TYPE g_statusLock = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_lastBackendSuccessMs = 0;
bool g_hasBackendSuccess = false;
}  // namespace

void deviceStatusRecordBackendSuccess() {
    portENTER_CRITICAL(&g_statusLock);
    g_lastBackendSuccessMs = millis();
    g_hasBackendSuccess = true;
    portEXIT_CRITICAL(&g_statusLock);
}

bool deviceIsOnline() {
    if (!wifiManagerIsConnected() || !deviceRegistrationIsRegistered()) {
        return false;
    }

    uint32_t lastSuccessMs;
    bool hasBackendSuccess;
    portENTER_CRITICAL(&g_statusLock);
    lastSuccessMs = g_lastBackendSuccessMs;
    hasBackendSuccess = g_hasBackendSuccess;
    portEXIT_CRITICAL(&g_statusLock);

    return hasBackendSuccess &&
        (millis() - lastSuccessMs) <= config::kBackendOnlineTimeoutMs;
}