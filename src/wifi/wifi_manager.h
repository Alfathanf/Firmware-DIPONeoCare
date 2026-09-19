#pragma once

#include <Arduino.h>
#include <WiFi.h>

void wifiManagerInit(const char* ssid, const char* password);
void wifiManagerUpdate();
bool wifiManagerIsConnected();
String wifiManagerGetSsid();
String wifiManagerGetMacAddress();
IPAddress wifiManagerGetLocalIp();
int8_t wifiManagerGetRssi();
const char* wifiManagerGetStatusName();
