#pragma once

#include <Arduino.h>

namespace sdSyncManager {
void init();
void update();
bool isActive();
uint32_t pendingCount();
}  // namespace sdSyncManager
