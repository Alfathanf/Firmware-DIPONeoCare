#include "wifi/wifi_provisioning.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_http_server.h>

#include "config.h"
#include "streaming/stream_server.h"
#include "wifi/wifi_manager.h"

namespace {

// ============================================================
// NVS
// ============================================================

constexpr char kWifiNamespace[] = "wifi";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";

// ============================================================
// Provisioning server
// ============================================================

DNSServer g_dnsServer;

httpd_handle_t g_server = nullptr;

bool g_active = false;

String g_apSsid;

String g_lastStatus =
    "Connect to the DIPONeoCare setup network "
    "and open http://192.168.4.1";

// ============================================================
// Helper state
// ============================================================

bool g_newCredentialsPending = false;

// ============================================================
// Build AP SSID
// ============================================================

String buildApSsid() {

    uint8_t mac[6];

    WiFi.macAddress(mac);

    char buffer[8];

    snprintf(
        buffer,
        sizeof(buffer),
        "%02X%02X",
        mac[4],
        mac[5]
    );

    return String(config::kProvisioningApSsid)
           + "-"
           + String(buffer);
}

// ============================================================
// URL decode
// ============================================================

String urlDecode(
    const String& input
) {

    String output;

    output.reserve(
        input.length()
    );

    for (
        size_t i = 0;
        i < input.length();
        ++i
    ) {

        if (input[i] == '+') {

            output += ' ';
        }

        else if (
            input[i] == '%' &&
            i + 2 < input.length()
        ) {

            char hi =
                input.charAt(i + 1);

            char lo =
                input.charAt(i + 2);

            int value = 0;

            if (
                hi >= '0' &&
                hi <= '9'
            ) {
                value =
                    (hi - '0') << 4;
            }

            else if (
                hi >= 'A' &&
                hi <= 'F'
            ) {
                value =
                    (hi - 'A' + 10) << 4;
            }

            else if (
                hi >= 'a' &&
                hi <= 'f'
            ) {
                value =
                    (hi - 'a' + 10) << 4;
            }

            if (
                lo >= '0' &&
                lo <= '9'
            ) {
                value |=
                    (lo - '0');
            }

            else if (
                lo >= 'A' &&
                lo <= 'F'
            ) {
                value |=
                    (lo - 'A' + 10);
            }

            else if (
                lo >= 'a' &&
                lo <= 'f'
            ) {
                value |=
                    (lo - 'a' + 10);
            }

            output +=
                static_cast<char>(value);

            i += 2;
        }

        else {

            output += input[i];
        }
    }

    return output;
}

// ============================================================
// Basic HTML escaping
// ============================================================

String htmlEscape(
    const String& input
) {

    String output;

    output.reserve(
        input.length() + 16
    );

    for (
        size_t i = 0;
        i < input.length();
        ++i
    ) {

        char c = input[i];

        switch (c) {

            case '&':
                output += "&amp;";
                break;

            case '<':
                output += "&lt;";
                break;

            case '>':
                output += "&gt;";
                break;

            case '"':
                output += "&quot;";
                break;

            case '\'':
                output += "&#39;";
                break;

            default:
                output += c;
                break;
        }
    }

    return output;
}

// ============================================================
// Scan Wi-Fi networks
// ============================================================

String buildNetworkOptions() {

    String options;

    int networkCount =
        WiFi.scanNetworks(
            false,
            true,
            1000
        );

    if (networkCount <= 0) {

        WiFi.scanDelete();

        return
            "<option value=\"\">"
            "No networks found"
            "</option>";
    }

    for (
        int i = 0;
        i < networkCount;
        ++i
    ) {

        String ssid =
            WiFi.SSID(i);

        if (ssid.length() == 0) {
            continue;
        }

        int rssi =
            WiFi.RSSI(i);

        String security =
            (
                WiFi.encryptionType(i)
                == WIFI_AUTH_OPEN
            )
            ? "Open"
            : "Secured";

        String escapedSsid =
            htmlEscape(ssid);

        options +=
            "<option value=\"" +
            escapedSsid +
            "\">" +
            escapedSsid +
            " (" +
            String(rssi) +
            " dBm, " +
            security +
            ")</option>";
    }

    WiFi.scanDelete();

    if (options.length() == 0) {

        return
            "<option value=\"\">"
            "No networks found"
            "</option>";
    }

    return options;
}

// ============================================================
// Build HTML page
// ============================================================

String buildPageHtml() {

    String html =
        "<!doctype html>"
        "<html>"
        "<head>"

        "<meta charset=\"utf-8\">"

        "<meta "
        "name=\"viewport\" "
        "content=\"width=device-width, initial-scale=1\">"

        "<title>DIPONeoCare Setup</title>"

        "<style>"

        "body{"
        "font-family:Arial,sans-serif;"
        "background:#0f172a;"
        "color:#e2e8f0;"
        "margin:0;"
        "display:flex;"
        "justify-content:center;"
        "}"

        ".card{"
        "width:min(92vw,440px);"
        "background:#111827;"
        "border-radius:12px;"
        "margin:24px 0;"
        "padding:20px;"
        "box-shadow:0 10px 24px rgba(0,0,0,.25);"
        "}"

        "h1{"
        "margin:0 0 8px;"
        "}"

        ".subtitle{"
        "color:#94a3b8;"
        "margin-bottom:20px;"
        "}"

        "label{"
        "display:block;"
        "margin-top:12px;"
        "font-weight:bold;"
        "}"

        "select,input{"
        "width:100%;"
        "box-sizing:border-box;"
        "margin-top:8px;"
        "padding:11px;"
        "border-radius:8px;"
        "border:1px solid #334155;"
        "background:#0b1220;"
        "color:#e2e8f0;"
        "}"

        "button{"
        "margin-top:18px;"
        "width:100%;"
        "padding:12px;"
        "border:none;"
        "border-radius:8px;"
        "background:#22c55e;"
        "color:#04130b;"
        "font-weight:bold;"
        "font-size:16px;"
        "}"

        ".status{"
        "margin-top:14px;"
        "padding:10px;"
        "border-radius:8px;"
        "background:#1e293b;"
        "font-size:14px;"
        "}"

        ".info{"
        "margin-top:16px;"
        "font-size:13px;"
        "color:#94a3b8;"
        "line-height:1.5;"
        "}"

        "</style>"

        "</head>"

        "<body>"

        "<div class=\"card\">"

        "<h1>DIPONeoCare</h1>"

        "<div class=\"subtitle\">"
        "Wi-Fi Configuration"
        "</div>"

        "<form "
        "method=\"POST\" "
        "action=\"/connect\">"

        "<label for=\"ssid\">"
        "Available Networks"
        "</label>"

        "<select "
        "id=\"ssid\" "
        "name=\"ssid\" "
        "required>"

        "<option value=\"\">"
        "Select Wi-Fi"
        "</option>"

        + buildNetworkOptions() +

        "</select>"

        "<label for=\"password\">"
        "Password"
        "</label>"

        "<input "
        "id=\"password\" "
        "name=\"password\" "
        "type=\"password\" "
        "placeholder=\"Enter Wi-Fi password\">"

        "<button type=\"submit\">"
        "Connect"
        "</button>"

        "</form>"

        "<div class=\"status\">"
        + htmlEscape(g_lastStatus) +
        "</div>"

        "<div class=\"info\">"
        "After saving the configuration, "
        "the device will connect automatically "
        "to the selected Wi-Fi network."
        "</div>"

        "</div>"

        "</body>"
        "</html>";

    return html;
}

// ============================================================
// HTTP handlers
// ============================================================

esp_err_t handleRoot(
    httpd_req_t* req
) {

    String html =
        buildPageHtml();

    httpd_resp_set_type(
        req,
        "text/html"
    );

    httpd_resp_send(
        req,
        html.c_str(),
        html.length()
    );

    return ESP_OK;
}

// ============================================================

esp_err_t handleConnect(
    httpd_req_t* req
) {

    char buffer[512];

    size_t received =
        httpd_req_recv(
            req,
            buffer,
            sizeof(buffer) - 1
        );

    if (received <= 0) {

        g_lastStatus =
            "No configuration data received.";

        httpd_resp_set_status(
            req,
            "400 Bad Request"
        );

        httpd_resp_send(
            req,
            "Bad request",
            HTTPD_RESP_USE_STRLEN
        );

        return ESP_OK;
    }

    buffer[received] = '\0';

    String body =
        String(buffer);

    String ssid;
    String password;

    // --------------------------------------------------------
    // Parse SSID
    // --------------------------------------------------------

    int ssidStart =
        body.indexOf("ssid=");

    int passwordStart =
        body.indexOf("&password=");

    if (
        ssidStart >= 0 &&
        passwordStart > ssidStart
    ) {

        ssid =
            urlDecode(
                body.substring(
                    ssidStart + 5,
                    passwordStart
                )
            );

        password =
            urlDecode(
                body.substring(
                    passwordStart + 10
                )
            );
    }

    // --------------------------------------------------------
    // Validate
    // --------------------------------------------------------

    if (ssid.length() == 0) {

        g_lastStatus =
            "Please select a Wi-Fi network.";

        httpd_resp_set_status(
            req,
            "400 Bad Request"
        );

        httpd_resp_send(
            req,
            "Invalid SSID",
            HTTPD_RESP_USE_STRLEN
        );

        return ESP_OK;
    }

    // --------------------------------------------------------
    // Save credentials
    // --------------------------------------------------------

    if (
        !wifiManagerApplyConfiguredCredentials(
            ssid,
            password
        )
    ) {

        g_lastStatus =
            "Failed to save Wi-Fi configuration.";

        httpd_resp_set_status(
            req,
            "500 Internal Server Error"
        );

        httpd_resp_send(
            req,
            "Failed to save configuration",
            HTTPD_RESP_USE_STRLEN
        );

        return ESP_OK;
    }

    // --------------------------------------------------------
    // Tell provisioning loop to apply it
    // --------------------------------------------------------

    g_newCredentialsPending = true;

    g_lastStatus =
        "Configuration saved. Connecting...";

    Serial.println();
    Serial.println(
        "[WIFI] New Wi-Fi credentials received"
    );

    Serial.printf(
        "[WIFI] SSID: %s\n",
        ssid.c_str()
    );

    // --------------------------------------------------------
    // Send response before shutting down portal
    // --------------------------------------------------------

    const char* response =
        "<!doctype html>"
        "<html>"
        "<head>"
        "<meta name=\"viewport\" "
        "content=\"width=device-width, initial-scale=1\">"
        "<title>DIPONeoCare</title>"
        "</head>"
        "<body>"
        "<h2>DIPONeoCare</h2>"
        "<p>Wi-Fi configuration saved.</p>"
        "<p>The device is now connecting "
        "to the selected network.</p>"
        "<p>You can close this page.</p>"
        "</body>"
        "</html>";

    httpd_resp_set_type(
        req,
        "text/html"
    );

    httpd_resp_set_status(
        req,
        "200 OK"
    );

    httpd_resp_send(
        req,
        response,
        HTTPD_RESP_USE_STRLEN
    );

    return ESP_OK;
}

// ============================================================
// Captive portal
// ============================================================

esp_err_t handleCaptive(
    httpd_req_t* req
) {

    httpd_resp_set_status(
        req,
        "302 Found"
    );

    httpd_resp_set_hdr(
        req,
        "Location",
        "http://192.168.4.1/"
    );

    httpd_resp_send(
        req,
        "",
        0
    );

    return ESP_OK;
}

// ============================================================
// Web server
// ============================================================

void configureWebServer() {

    if (g_server != nullptr) {
        return;
    }

    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.server_port = 80;

    config.max_open_sockets = 4;

    config.stack_size = 8192;

    esp_err_t ret =
        httpd_start(
            &g_server,
            &config
        );

    if (ret != ESP_OK) {

        g_server = nullptr;

        Serial.printf(
            "[WIFI] Failed to start provisioning "
            "web server: %d\n",
            ret
        );

        return;
    }

    // --------------------------------------------------------
    // Root
    // --------------------------------------------------------

    httpd_uri_t rootUri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handleRoot,
        .user_ctx = nullptr
    };

    // --------------------------------------------------------
    // Connect
    // --------------------------------------------------------

    httpd_uri_t connectUri = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = handleConnect,
        .user_ctx = nullptr
    };

    // --------------------------------------------------------
    // Captive portal
    // --------------------------------------------------------

    httpd_uri_t captiveUri = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = handleCaptive,
        .user_ctx = nullptr
    };

    httpd_uri_t hotspotUri = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = handleCaptive,
        .user_ctx = nullptr
    };

    httpd_uri_t faviconUri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = handleCaptive,
        .user_ctx = nullptr
    };

    // --------------------------------------------------------
    // Register handlers
    // --------------------------------------------------------

    httpd_register_uri_handler(
        g_server,
        &rootUri
    );

    httpd_register_uri_handler(
        g_server,
        &connectUri
    );

    httpd_register_uri_handler(
        g_server,
        &captiveUri
    );

    httpd_register_uri_handler(
        g_server,
        &hotspotUri
    );

    httpd_register_uri_handler(
        g_server,
        &faviconUri
    );

    Serial.println(
        "[WIFI] Provisioning portal started"
    );
}

// ============================================================

void stopWebServer() {

    if (g_server != nullptr) {

        httpd_stop(g_server);

        g_server = nullptr;

        Serial.println(
            "[WIFI] Provisioning web server stopped"
        );
    }
}

// ============================================================
// Access Point
// ============================================================

void startProvisioningAp() {

    g_apSsid =
        buildApSsid();

    // IMPORTANT:
    // AP + STA allows the ESP32 to scan nearby Wi-Fi networks
    // while the setup AP remains active.
    WiFi.mode(WIFI_AP_STA);

    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );

    bool started =
        WiFi.softAP(
            g_apSsid.c_str(),
            config::kProvisioningApPassword
        );

    if (!started) {

        Serial.println(
            "[WIFI] Failed to start provisioning AP"
        );

        return;
    }

    Serial.printf(
        "[WIFI] AP SSID: %s\n",
        g_apSsid.c_str()
    );

    Serial.println(
        "[WIFI] AP IP: 192.168.4.1"
    );
}

// ============================================================

void stopProvisioningAp() {

    WiFi.softAPdisconnect(true);

    WiFi.mode(WIFI_OFF);
}

// ============================================================
// Provisioning lifecycle
// ============================================================

void startProvisioningMode() {

    if (g_active) {
        return;
    }

    g_active = true;

    g_newCredentialsPending = false;

    g_lastStatus =
        "Connect to DIPONeoCare-Setup "
        "and open http://192.168.4.1";

    Serial.println();
    Serial.println(
        "========================================"
    );

    Serial.println(
        "[WIFI] ENTERING PROVISIONING MODE"
    );

    Serial.println(
        "========================================"
    );

    // --------------------------------------------------------
    // Stop normal stream server first.
    // Both stream and provisioning use port 80.
    // --------------------------------------------------------

    streamServerStop();

    // --------------------------------------------------------
    // Start AP
    // --------------------------------------------------------

    startProvisioningAp();

    // --------------------------------------------------------
    // Start web portal
    // --------------------------------------------------------

    configureWebServer();

    // --------------------------------------------------------
    // Start DNS captive portal
    // --------------------------------------------------------

    g_dnsServer.start(
        53,
        "*",
        IPAddress(192, 168, 4, 1)
    );

    Serial.println(
        "[WIFI] Provisioning portal ready"
    );

    Serial.printf(
        "[WIFI] Connect to Wi-Fi: %s\n",
        g_apSsid.c_str()
    );

    Serial.println(
        "[WIFI] Open: http://192.168.4.1"
    );

    Serial.println(
        "========================================"
    );
}

// ============================================================

void stopProvisioningMode() {

    if (!g_active) {
        return;
    }

    Serial.println(
        "[WIFI] Leaving provisioning mode"
    );

    g_active = false;

    g_newCredentialsPending = false;

    stopWebServer();

    g_dnsServer.stop();

    stopProvisioningAp();

    Serial.println(
        "[WIFI] Provisioning mode stopped"
    );
}

// ============================================================
// Stored credentials check
// ============================================================

bool hasStoredCredentials() {

    Preferences prefs;

    if (
        !prefs.begin(
            kWifiNamespace,
            true
        )
    ) {
        return false;
    }

    String ssid =
        prefs.getString(
            kSsidKey,
            ""
        );

    String password =
        prefs.getString(
            kPasswordKey,
            ""
        );

    prefs.end();

    return (
        ssid.length() > 0 &&
        password.length() > 0
    );
}

} // namespace

// ============================================================
// Public API
// ============================================================

void wifiProvisioningInit() {

    if (
        !config::kWifiProvisioningEnabled
    ) {
        return;
    }

    if (hasStoredCredentials()) {
        return;
    }

    startProvisioningMode();
}

// ============================================================

void wifiProvisioningStart() {

    if (
        !config::kWifiProvisioningEnabled
    ) {
        Serial.println(
            "[WIFI] Provisioning is disabled"
        );

        return;
    }

    if (g_active) {
        return;
    }

    Serial.println(
        "[WIFI] Forced provisioning requested"
    );

    startProvisioningMode();
}

// ============================================================

void wifiProvisioningLoop() {

    if (
        !config::kWifiProvisioningEnabled ||
        !g_active
    ) {
        return;
    }

    // --------------------------------------------------------
    // Process DNS captive portal
    // --------------------------------------------------------

    g_dnsServer.processNextRequest();

    // --------------------------------------------------------
    // Apply newly submitted credentials
    // --------------------------------------------------------

    if (g_newCredentialsPending) {

        g_newCredentialsPending = false;

        Serial.println(
            "[WIFI] Applying new Wi-Fi credentials"
        );

        // Stop provisioning first.
        // This changes Wi-Fi from AP+STA to STA.
        stopProvisioningMode();

        // Start connection using the newly stored
        // credentials.
        //
        // wifiManagerApplyConfiguredCredentials()
        // has already populated the target credentials,
        // but beginStaConnection() is private to
        // wifi_manager.cpp.
        //
        // Trigger a fresh Wi-Fi connection by allowing
        // wifiManagerUpdate() to reconnect.
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.persistent(false);

        String ssid = WiFi.SSID();

        // WiFi.SSID() may not yet represent the newly
        // saved credentials, therefore reload them
        // from Preferences here.

        Preferences prefs;

        if (
            prefs.begin(
                kWifiNamespace,
                true
            )
        ) {

            String savedSsid =
                prefs.getString(
                    kSsidKey,
                    ""
                );

            String savedPassword =
                prefs.getString(
                    kPasswordKey,
                    ""
                );

            prefs.end();

            if (savedSsid.length() > 0) {

                Serial.printf(
                    "[WIFI] Connecting to: %s\n",
                    savedSsid.c_str()
                );

                WiFi.begin(
                    savedSsid.c_str(),
                    savedPassword.c_str()
                );
            }
        }
    }
}

// ============================================================

void wifiProvisioningStop() {

    stopProvisioningMode();
}

// ============================================================

bool wifiProvisioningIsActive() {

    return g_active;
}

// ============================================================

String wifiProvisioningGetApSsid() {

    return g_apSsid;
}