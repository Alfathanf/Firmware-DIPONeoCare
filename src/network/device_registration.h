#pragma once

#include <Arduino.h>

void deviceRegistrationInit();
void deviceRegistrationUpdate();
void deviceRegistrationHandleAuthFailure(int httpCode);
bool deviceRegistrationIsRegistered();
bool deviceRegistrationHasStoredToken();
String deviceRegistrationGetToken();
String deviceRegistrationGetDeviceId();
