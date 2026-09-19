#pragma once

#include <Arduino.h>

void deviceRegistrationInit();
void deviceRegistrationUpdate();
bool deviceRegistrationIsRegistered();
bool deviceRegistrationHasStoredToken();
String deviceRegistrationGetToken();
String deviceRegistrationGetDeviceId();
