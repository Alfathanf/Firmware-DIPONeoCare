#pragma once

#include <Arduino.h>

void streamServerStart(uint16_t port = 80);
void streamServerStop();
bool streamServerIsRunning();
