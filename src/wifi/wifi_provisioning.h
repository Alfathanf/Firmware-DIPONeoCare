#pragma once

#include <Arduino.h>

/**
 * Initialize provisioning if no stored credentials exist.
 */
void wifiProvisioningInit();

/**
 * Force provisioning mode.
 *
 * Used when stored Wi-Fi credentials exist
 * but the device cannot connect to that network.
 */
void wifiProvisioningStart();

/**
 * Process provisioning services.
 *
 * Must be called continuously from loop().
 */
void wifiProvisioningLoop();

/**
 * Stop provisioning mode and return to normal STA mode.
 */
void wifiProvisioningStop();

/**
 * Returns true while provisioning mode is active.
 */
bool wifiProvisioningIsActive();

/**
 * Get provisioning AP SSID.
 */
String wifiProvisioningGetApSsid();