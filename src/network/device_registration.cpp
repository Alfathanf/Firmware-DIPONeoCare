#include "network/device_registration.h"

#include <Preferences.h>
#include <ArduinoJson.h>

#include "config.h"
#include "network/http_client.h"
#include "wifi/wifi_manager.h"

namespace {
constexpr char kPreferencesNamespace[] = "device";
constexpr char kDeviceTokenKey[] = "deviceToken";
constexpr char kDeviceIdKey[] = "deviceId";
constexpr char kTokenMissing[] = "";

Preferences g_preferences;
bool g_initialized = false;
bool g_registered = false;
String g_deviceToken;
String g_deviceId;
uint32_t g_lastRegistrationAttemptMs = 0;
uint32_t g_registrationRetryIntervalMs = 20000;

String readTokenFromNvs() {
    if (!g_initialized) {
        return String();
    }

    g_preferences.begin(kPreferencesNamespace, true);
    String token = g_preferences.getString(kDeviceTokenKey, kTokenMissing);
    g_preferences.end();
    return token;
}

bool saveRegistrationToNvs(const String& deviceId, const String& token) {
    if (!g_initialized || deviceId.length() == 0 || token.length() == 0) {
        return false;
    }

    if (!g_preferences.begin(kPreferencesNamespace, false)) {
        Serial.println("[DEVICE] Failed to open device storage");
        return false;
    }

    size_t deviceIdBytes = g_preferences.putString(kDeviceIdKey, deviceId);
    size_t tokenBytes = g_preferences.putString(kDeviceTokenKey, token);
    g_preferences.end();

    if (deviceIdBytes == 0 || tokenBytes == 0) {
        Serial.println("[DEVICE] Failed to save device registration");
        return false;
    }

    return true;
}

String loadDeviceIdFromNvs() {
    if (!g_initialized) {
        return String();
    }

    g_preferences.begin(kPreferencesNamespace, true);
    String deviceId = g_preferences.getString(kDeviceIdKey, "");
    g_preferences.end();
    return deviceId;
}

bool isTokenValid(const String& token) {
    return token.length() > 0;
}

bool parseRegistrationResponse(const String& body, String& outDeviceId, String& outDeviceToken, bool& outPaired) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);
    if (error) {
        Serial.printf("[DEVICE] JSON parse failed: %s\n", error.c_str());
        return false;
    }

    if (!doc["success"].is<bool>() || !doc["success"].as<bool>()) {
        Serial.println("[DEVICE] Registration response success flag is false");
        return false;
    }

    const char* deviceId = doc["data"]["deviceId"] | "";
    const char* deviceToken = doc["data"]["deviceToken"] | "";
    if (deviceId == nullptr || deviceToken == nullptr) {
        Serial.println("[DEVICE] Registration response is missing deviceId or deviceToken");
        return false;
    }

    outDeviceId = String(deviceId);
    outDeviceToken = String(deviceToken);
    outPaired = doc["data"]["paired"].as<bool>();

    return outDeviceId.length() > 0 && outDeviceToken.length() > 0;
}

void attemptRegistration() {
    if (!wifiManagerIsConnected()) {
        return;
    }

    const uint32_t now = millis();
    if (now - g_lastRegistrationAttemptMs < g_registrationRetryIntervalMs) {
        return;
    }
    g_lastRegistrationAttemptMs = now;

    String macAddress = wifiManagerGetMacAddress();
    String localIp = wifiManagerGetLocalIp().toString();
    String firmwareVersion = String(config::kFirmwareVersion);

    String payload = "{\"macAddress\":\"" + macAddress + "\",\"localIp\":\"" + localIp + "\",\"firmwareVersion\":\"" + firmwareVersion + "\"}";
    String url = String(config::kBackendBaseUrl) + String(config::kDeviceRegisterPath);

    Serial.println("[DEVICE] Registering device...");
    Serial.printf("[DEVICE] MAC: %s\n", macAddress.c_str());
    Serial.printf("[DEVICE] IP: %s\n", localIp.c_str());
    Serial.printf("[DEVICE] Firmware: %s\n", firmwareVersion.c_str());

    String responseBody;
    int httpCode = 0;
    bool requestOk = httpPostJson(url, payload, "", responseBody, httpCode);
    if (!requestOk) {
        Serial.printf("[DEVICE] Registration failed: HTTP %d\n", httpCode);
        return;
    }

    String deviceId;
    String deviceToken;
    bool paired = false;
    if (!parseRegistrationResponse(responseBody, deviceId, deviceToken, paired)) {
        Serial.println("[DEVICE] Registration response invalid; retry later");
        return;
    }

    if (!saveRegistrationToNvs(deviceId, deviceToken)) {
        Serial.println("[DEVICE] Registration succeeded, but device data was not saved");
        return;
    }

    g_deviceToken = deviceToken;
    g_deviceId = deviceId;
    g_registered = true;

    Serial.printf("[DEVICE] Registration successful: HTTP %d\n", httpCode);
    Serial.printf("[DEVICE] Device ID: %s\n", deviceId.c_str());
    Serial.println("[DEVICE] Device token saved");
    Serial.printf("[DEVICE] Paired: %s\n", paired ? "true" : "false");
}

}  // namespace

void deviceRegistrationInit() {
    g_initialized = true;
    g_deviceToken = readTokenFromNvs();
    g_deviceId = loadDeviceIdFromNvs();
    g_registered = isTokenValid(g_deviceToken);

    if (g_registered) {
        Serial.println("[DEVICE] Stored device token found");
        Serial.println("[DEVICE] Device already registered");
        Serial.printf("[DEVICE] Device ID: %s\n", g_deviceId.length() > 0 ? g_deviceId.c_str() : "unknown");
    } else {
        Serial.println("[DEVICE] No stored device token");
    }
}

void deviceRegistrationUpdate() {
    if (!wifiManagerIsConnected()) {
        return;
    }

    if (g_registered) {
        return;
    }

    attemptRegistration();
}

bool deviceRegistrationIsRegistered() {
    return g_registered;
}

bool deviceRegistrationHasStoredToken() {
    return isTokenValid(readTokenFromNvs());
}

String deviceRegistrationGetToken() {
    if (g_deviceToken.length() == 0) {
        g_deviceToken = readTokenFromNvs();
    }
    return g_deviceToken;
}

String deviceRegistrationGetDeviceId() {
    if (g_deviceId.length() == 0) {
        g_deviceId = loadDeviceIdFromNvs();
    }
    return g_deviceId;
}

void deviceRegistrationHandleAuthFailure(int httpCode) {
    if (httpCode != 401 && httpCode != 403) {
        return;
    }

    if (!g_initialized) {
        return;
    }

    if (g_preferences.begin(kPreferencesNamespace, false)) {
        g_preferences.remove(kDeviceTokenKey);
        g_preferences.remove(kDeviceIdKey);
        g_preferences.end();
    }

    g_deviceToken = String();
    g_deviceId = String();
    g_registered = false;
    g_lastRegistrationAttemptMs = 0;
    Serial.printf("[DEVICE] Stored device token rejected: HTTP %d; registration required\n", httpCode);
}
