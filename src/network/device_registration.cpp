#include "network/device_registration.h"

#include <Preferences.h>
#include <ArduinoJson.h>

#include "config.h"
#include "network/http_client.h"
#include "wifi/wifi_manager.h"

namespace {
constexpr char kPreferencesNamespace[] = "diponeocare";
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

void saveTokenToNvs(const String& token) {
    if (!g_initialized || token.length() == 0) {
        return;
    }

    g_preferences.begin(kPreferencesNamespace, false);
    g_preferences.putString(kDeviceTokenKey, token);
    g_preferences.end();
}

void saveDeviceIdToNvs(const String& deviceId) {
    if (!g_initialized || deviceId.length() == 0) {
        return;
    }

    g_preferences.begin(kPreferencesNamespace, false);
    g_preferences.putString(kDeviceIdKey, deviceId);
    g_preferences.end();
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

    if (!doc["data"]["deviceId"].is<const char*>() || !doc["data"]["deviceToken"].is<const char*>()) {
        Serial.println("[DEVICE] Registration response is missing deviceId or deviceToken");
        return false;
    }

    outDeviceId = String(doc["data"]["deviceId"].as<const char*>());
    outDeviceToken = String(doc["data"]["deviceToken"].as<const char*>());
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
    if (!requestOk || httpCode != 200) {
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

    saveTokenToNvs(deviceToken);
    saveDeviceIdToNvs(deviceId);
    g_deviceToken = deviceToken;
    g_deviceId = deviceId;
    g_registered = true;

    Serial.println("[DEVICE] Registration successful");
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
        Serial.println("[DEVICE] Device token loaded from flash");
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
