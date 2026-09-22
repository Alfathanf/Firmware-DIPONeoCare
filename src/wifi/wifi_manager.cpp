#include "wifi/wifi_manager.h"

#include <Preferences.h>

#include "config.h"
#include "streaming/stream_server.h"
#include "wifi/wifi_provisioning.h"

namespace {

// ============================================================
// NVS
// ============================================================

constexpr char kWifiNamespace[] = "wifi";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";

// ============================================================
// Wi-Fi connection configuration
// ============================================================

constexpr uint8_t kMaxConnectionFailuresBeforeProvisioning = 3;

// ============================================================
// State
// ============================================================

const char* g_defaultSsid = nullptr;
const char* g_defaultPassword = nullptr;

String g_targetSsid;
String g_targetPassword;

bool g_connected = false;
bool g_provisioning = false;

uint8_t g_connectionFailures = 0;

uint32_t g_lastConnectionAttemptMs = 0;
uint32_t g_lastStatusLogMs = 0;

// ============================================================
// NVS helpers
// ============================================================

String readStoredSsid() {
    Preferences prefs;

    if (!prefs.begin(kWifiNamespace, true)) {
        return "";
    }

    String value = prefs.getString(kSsidKey, "");

    prefs.end();

    return value;
}

String readStoredPassword() {
    Preferences prefs;

    if (!prefs.begin(kWifiNamespace, true)) {
        return "";
    }

    String value = prefs.getString(kPasswordKey, "");

    prefs.end();

    return value;
}

bool hasStoredCredentials() {
    String ssid = readStoredSsid();
    String password = readStoredPassword();

    return ssid.length() > 0 && password.length() > 0;
}

bool saveStoredCredentials(
    const String& ssid,
    const String& password
) {
    Preferences prefs;

    if (!prefs.begin(kWifiNamespace, false)) {
        Serial.println("[WIFI] Failed to open NVS for writing");
        return false;
    }

    size_t ssidWritten =
        prefs.putString(kSsidKey, ssid);

    size_t passwordWritten =
        prefs.putString(kPasswordKey, password);

    prefs.end();

    if (ssidWritten == 0 || passwordWritten == 0) {
        Serial.println("[WIFI] Failed to save Wi-Fi credentials");
        return false;
    }

    Serial.println("[WIFI] Wi-Fi credentials saved");

    return true;
}

void clearStoredCredentials() {
    Preferences prefs;

    if (!prefs.begin(kWifiNamespace, false)) {
        Serial.println("[WIFI] Failed to open NVS");
        return;
    }

    prefs.remove(kSsidKey);
    prefs.remove(kPasswordKey);

    prefs.end();

    Serial.println("[WIFI] Stored Wi-Fi credentials cleared");
}

// ============================================================
// Logging
// ============================================================

void logWifiConnectionDetails() {

    Serial.println();
    Serial.println("========================================");
    Serial.println("[WIFI] Connected");
    Serial.println("========================================");

    Serial.printf(
        "[WIFI] SSID: %s\n",
        WiFi.SSID().c_str()
    );

    Serial.printf(
        "[WIFI] IP: %s\n",
        WiFi.localIP().toString().c_str()
    );

    Serial.printf(
        "[WIFI] MAC: %s\n",
        WiFi.macAddress().c_str()
    );

    Serial.printf(
        "[WIFI] RSSI: %d dBm\n",
        WiFi.RSSI()
    );

    Serial.println("========================================");
}

void logWifiError(const wl_status_t status) {

    Serial.printf(
        "[WIFI] Connection status: %s (%d)\n",
        wifiManagerGetStatusName(),
        status
    );
}

// ============================================================
// Connection
// ============================================================

void beginStaConnection(
    const String& ssid,
    const String& password
) {

    g_targetSsid = ssid;
    g_targetPassword = password;

    g_connected = false;
    g_connectionFailures = 0;

    WiFi.mode(WIFI_STA);

    WiFi.setAutoReconnect(true);

    // We manage credentials ourselves using Preferences.
    WiFi.persistent(false);

    WiFi.begin(
        g_targetSsid.c_str(),
        g_targetPassword.c_str()
    );

    g_lastConnectionAttemptMs = millis();
    g_lastStatusLogMs = millis();

    Serial.println();
    Serial.println("[WIFI] Connecting to configured network...");

    Serial.printf(
        "[WIFI] Target SSID: %s\n",
        g_targetSsid.c_str()
    );
}

// ============================================================
// Provisioning transition
// ============================================================

void switchToProvisioningMode() {

    if (g_provisioning) {
        return;
    }

    Serial.println();
    Serial.println("========================================");
    Serial.println("[WIFI] Wi-Fi connection failed");
    Serial.println("[WIFI] Switching to provisioning mode");
    Serial.println("========================================");

    g_connected = false;
    g_provisioning = true;
    g_connectionFailures = 0;

    WiFi.disconnect(false, false);

    wifiProvisioningStart();
}

} // namespace

// ============================================================
// Public API
// ============================================================

void wifiManagerInit(
    const char* ssid,
    const char* password
) {

    g_defaultSsid = ssid;
    g_defaultPassword = password;

    g_connected = false;
    g_provisioning = false;
    g_connectionFailures = 0;

    Serial.println();
    Serial.println("========================================");
    Serial.println("[WIFI] Initializing Wi-Fi manager");
    Serial.println("========================================");

    // ========================================================
    // Stored credentials
    // ========================================================

    if (hasStoredCredentials()) {

        String storedSsid = readStoredSsid();
        String storedPassword = readStoredPassword();

        Serial.println("[WIFI] Stored credentials found");

        beginStaConnection(
            storedSsid,
            storedPassword
        );

        return;
    }

    // ========================================================
    // No stored credentials
    // ========================================================

    Serial.println("[WIFI] No stored Wi-Fi credentials");

    if (config::kWifiProvisioningEnabled) {

        wifiProvisioningStart();

        return;
    }

    // ========================================================
    // Development fallback
    // ========================================================

    if (
        g_defaultSsid != nullptr &&
        g_defaultPassword != nullptr &&
        strlen(g_defaultSsid) > 0
    ) {

        Serial.println(
            "[WIFI] Using development fallback credentials"
        );

        beginStaConnection(
            String(g_defaultSsid),
            String(g_defaultPassword)
        );

        return;
    }

    // ========================================================
    // Nothing available
    // ========================================================

    wifiProvisioningStart();
}

// ============================================================

void wifiManagerUpdate() {

    // ========================================================
    // Provisioning mode
    // ========================================================

    if (wifiProvisioningIsActive()) {

        wifiProvisioningLoop();

        return;
    }

    // ========================================================
    // No target Wi-Fi
    // ========================================================

    if (
        g_targetSsid.length() == 0 &&
        g_defaultSsid == nullptr
    ) {
        return;
    }

    const wl_status_t status = WiFi.status();

    // ========================================================
    // Connected
    // ========================================================

    if (status == WL_CONNECTED) {

        if (!g_connected) {

            g_connected = true;
            g_connectionFailures = 0;

            logWifiConnectionDetails();
        }

        return;
    }

    // ========================================================
    // Connection lost
    // ========================================================

    if (g_connected) {

        g_connected = false;

        Serial.println("[WIFI] Connection lost");
        Serial.println("[WIFI] Attempting reconnection...");
    }

    const uint32_t now = millis();

    // ========================================================
    // Retry connection
    // ========================================================

    if (
        now - g_lastConnectionAttemptMs >=
        config::kWiFiRetryIntervalMs
    ) {

        g_lastConnectionAttemptMs = now;

        g_connectionFailures++;

        Serial.printf(
            "[WIFI] Connection attempt %u/%u\n",
            g_connectionFailures,
            kMaxConnectionFailuresBeforeProvisioning
        );

        // ----------------------------------------------------
        // Stored/configured Wi-Fi
        // ----------------------------------------------------

        if (g_targetSsid.length() > 0) {

            WiFi.begin(
                g_targetSsid.c_str(),
                g_targetPassword.c_str()
            );
        }

        // ----------------------------------------------------
        // Development fallback
        // ----------------------------------------------------

        else if (
            g_defaultSsid != nullptr &&
            g_defaultPassword != nullptr
        ) {

            WiFi.begin(
                g_defaultSsid,
                g_defaultPassword
            );
        }

        // ----------------------------------------------------
        // Too many failures
        // ----------------------------------------------------

        if (
            g_connectionFailures >=
            kMaxConnectionFailuresBeforeProvisioning
        ) {

            Serial.println(
                "[WIFI] Maximum connection attempts reached"
            );

            switchToProvisioningMode();

            return;
        }
    }

    // ========================================================
    // Periodic status logging
    // ========================================================

    if (
        now - g_lastStatusLogMs >= 10000
    ) {

        g_lastStatusLogMs = now;

        logWifiError(status);
    }
}

// ============================================================

bool wifiManagerIsConnected() {

    return WiFi.status() == WL_CONNECTED;
}

// ============================================================

bool wifiManagerIsProvisioning() {

    return wifiProvisioningIsActive();
}

// ============================================================

bool wifiManagerHasStoredCredentials() {

    return hasStoredCredentials();
}

// ============================================================

void wifiManagerClearStoredCredentials() {

    clearStoredCredentials();

    g_targetSsid = "";
    g_targetPassword = "";

    g_connectionFailures = 0;
}

// ============================================================

bool wifiManagerApplyConfiguredCredentials(
    const String& ssid,
    const String& password
) {

    if (ssid.length() == 0) {
        return false;
    }

    Serial.printf(
        "[WIFI] Applying new Wi-Fi configuration: %s\n",
        ssid.c_str()
    );

    if (
        !saveStoredCredentials(
            ssid,
            password
        )
    ) {
        return false;
    }

    g_targetSsid = ssid;
    g_targetPassword = password;

    g_connectionFailures = 0;
    g_connected = false;

    return true;
}

// ============================================================

String wifiManagerGetSsid() {

    return WiFi.SSID();
}

// ============================================================

String wifiManagerGetMacAddress() {

    return WiFi.macAddress();
}

// ============================================================

IPAddress wifiManagerGetLocalIp() {

    return WiFi.localIP();
}

// ============================================================

int8_t wifiManagerGetRssi() {

    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }

    return WiFi.RSSI();
}

// ============================================================

const char* wifiManagerGetStatusName() {

    switch (WiFi.status()) {

        case WL_IDLE_STATUS:
            return "WL_IDLE_STATUS";

        case WL_NO_SSID_AVAIL:
            return "WL_NO_SSID_AVAIL";

        case WL_SCAN_COMPLETED:
            return "WL_SCAN_COMPLETED";

        case WL_CONNECTED:
            return "WL_CONNECTED";

        case WL_CONNECT_FAILED:
            return "WL_CONNECT_FAILED";

        case WL_CONNECTION_LOST:
            return "WL_CONNECTION_LOST";

        case WL_DISCONNECTED:
            return "WL_DISCONNECTED";

        default:
            return "UNKNOWN";
    }
}