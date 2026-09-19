#include "wifi/wifi_manager.h"

#include "secrets.h"

namespace {
const char* g_ssid = nullptr;
const char* g_password = nullptr;
bool g_connected = false;
uint32_t g_lastConnectionAttemptMs = 0;
uint32_t g_lastStatusLogMs = 0;

void logWifiConnectionDetails() {
    Serial.printf("[WIFI] Connected\n");
    Serial.printf("[WIFI] SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());
}

void logWifiError(const wl_status_t status) {
    Serial.printf("[WIFI] Connection status: %s (%d)\n", wifiManagerGetStatusName(), status);
}

}  // namespace

void wifiManagerInit(const char* ssid, const char* password) {
    g_ssid = ssid;
    g_password = password;

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);

    Serial.println("[WIFI] Connecting to WiFi...");
    WiFi.begin(g_ssid, g_password);
    g_lastConnectionAttemptMs = millis();
    g_lastStatusLogMs = millis();
}

void wifiManagerUpdate() {
    if (g_ssid == nullptr || g_password == nullptr) {
        return;
    }

    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
        if (!g_connected) {
            g_connected = true;
            logWifiConnectionDetails();
        }
        return;
    }

    if (g_connected) {
        g_connected = false;
        Serial.println("[WIFI] Connection lost");
        Serial.println("[WIFI] Attempting reconnection...");
    }

    const uint32_t now = millis();
    if (now - g_lastConnectionAttemptMs >= 5000) {
        g_lastConnectionAttemptMs = now;
        WiFi.begin(g_ssid, g_password);
    }

    if (now - g_lastStatusLogMs >= 10000) {
        g_lastStatusLogMs = now;
        logWifiError(status);
    }
}

bool wifiManagerIsConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String wifiManagerGetSsid() {
    return WiFi.SSID();
}

String wifiManagerGetMacAddress() {
    return WiFi.macAddress();
}

IPAddress wifiManagerGetLocalIp() {
    return WiFi.localIP();
}

int8_t wifiManagerGetRssi() {
    return WiFi.RSSI();
}

const char* wifiManagerGetStatusName() {
    switch (WiFi.status()) {
        case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
        case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
        case WL_CONNECTED: return "WL_CONNECTED";
        case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
        case WL_DISCONNECTED: return "WL_DISCONNECTED";
        default: return "UNKNOWN";
    }
}
