#include "network/heartbeat.h"

#include <ArduinoJson.h>

#include "config.h"
#include "network/http_client.h"
#include "network/device_registration.h"
#include "wifi/wifi_manager.h"

namespace {
uint32_t g_lastHeartbeatAttemptMs = 0;
bool g_heartbeatEnabled = false;

bool sendHeartbeatRequest() {
    if (!wifiManagerIsConnected()) {
        return false;
    }

    String deviceToken = deviceRegistrationGetToken();
    if (deviceToken.length() == 0) {
        Serial.println("[HEARTBEAT] No device token available; skipping");
        return false;
    }

    String macAddress = wifiManagerGetMacAddress();
    String localIp = wifiManagerGetLocalIp().toString();
    String payload = "{\"macAddress\":\"" + macAddress + "\",\"localIp\":\"" + localIp + "\"}";
    String url = String(config::kBackendBaseUrl) + String(config::kDeviceHeartbeatPath);

    String responseBody;
    int httpCode = 0;
    bool requestResult = httpPostJson(url, payload, deviceToken, responseBody, httpCode);

    if (requestResult) {
        Serial.println("[HEARTBEAT] Sent successfully");
        return true;
    }

    Serial.printf("[HEARTBEAT] Failed: HTTP %d\n", httpCode);
    return false;
}

}  // namespace

void heartbeatInit() {
    g_lastHeartbeatAttemptMs = millis();
    g_heartbeatEnabled = true;
    Serial.println("[HEARTBEAT] Initialized");
}

void heartbeatUpdate() {
    if (!wifiManagerIsConnected()) {
        return;
    }

    if (!deviceRegistrationIsRegistered()) {
        return;
    }

    const uint32_t now = millis();
    if (now - g_lastHeartbeatAttemptMs >= config::kHeartbeatIntervalMs) {
        g_lastHeartbeatAttemptMs = now;
        sendHeartbeatRequest();
    }
}

bool heartbeatIsEnabled() {
    return g_heartbeatEnabled && wifiManagerIsConnected();
}
