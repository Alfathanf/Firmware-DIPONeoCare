#include "network/http_client.h"

#include <WiFiClient.h>
#include <HTTPClient.h>

#include "config.h"

bool httpPostJson(
    const String& url,
    const String& payload,
    const String& token,
    String& responseBody,
    int& httpCode) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[HTTP] Wi-Fi not connected; skipping request");
        return false;
    }

    HTTPClient http;
    http.setTimeout(5000);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    if (token.length() > 0) {
        http.addHeader("x-device-token", token);
    }

    httpCode = http.POST(payload);
    responseBody = http.getString();

    bool success = (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED);
    if (!success) {
        Serial.printf("[HTTP] Request failed: HTTP %d\n", httpCode);
    }

    http.end();
    return success;
}
