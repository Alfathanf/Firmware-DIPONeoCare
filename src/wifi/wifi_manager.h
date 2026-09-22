#pragma once

#include <Arduino.h>
#include <WiFi.h>

/**
 * Initialize Wi-Fi manager.
 *
 * If stored credentials exist:
 *   - Connect to stored Wi-Fi.
 *
 * If no stored credentials exist:
 *   - Start provisioning mode.
 *
 * @param ssid     Development fallback SSID.
 * @param password Development fallback password.
 */
void wifiManagerInit(const char* ssid, const char* password);

/**
 * Must be called continuously from loop().
 *
 * Handles:
 * - Wi-Fi connection
 * - reconnection
 * - connection failure detection
 * - provisioning transition
 */
void wifiManagerUpdate();

/**
 * Returns true when ESP32 is connected to Wi-Fi.
 */
bool wifiManagerIsConnected();

/**
 * Returns true when provisioning mode is active.
 */
bool wifiManagerIsProvisioning();

/**
 * Returns true when Wi-Fi credentials exist in NVS.
 */
bool wifiManagerHasStoredCredentials();

/**
 * Clears stored Wi-Fi credentials from NVS.
 */
void wifiManagerClearStoredCredentials();

/**
 * Save and apply new Wi-Fi credentials.
 *
 * @return true if credentials are valid and saved.
 */
bool wifiManagerApplyConfiguredCredentials(
    const String& ssid,
    const String& password
);

/**
 * Get currently connected SSID.
 */
String wifiManagerGetSsid();

/**
 * Get ESP32 MAC address.
 */
String wifiManagerGetMacAddress();

/**
 * Get current local IP address.
 */
IPAddress wifiManagerGetLocalIp();

/**
 * Get current Wi-Fi RSSI.
 */
int8_t wifiManagerGetRssi();

/**
 * Get human-readable Wi-Fi status.
 */
const char* wifiManagerGetStatusName();