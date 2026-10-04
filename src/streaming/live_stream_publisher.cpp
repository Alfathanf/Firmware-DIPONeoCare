#include "streaming/live_stream_publisher.h"

#include <Arduino.h>

#include "config.h"
#include "network/device_status.h"

namespace liveStreamPublisher {
namespace {
bool g_initialized = false;
bool g_waitingLogged = false;
}

void begin() {
    g_initialized = true;
    Serial.println("[LIVE] Initializing live stream publisher...");

    if (!config::kLiveStreamPublisherEnabled || config::kLiveStreamIngestUrl[0] == '\0') {
        Serial.println("[LIVE] Waiting for backend ingestion endpoint/protocol; publisher disabled");
        return;
    }

    Serial.println("[LIVE] Endpoint is configured, but no backend stream protocol adapter is available");
}

void update() {
    if (!g_initialized || !config::kLiveStreamPublisherEnabled ||
        config::kLiveStreamIngestUrl[0] == '\0') {
        return;
    }

    if (!deviceIsOnline()) {
        g_waitingLogged = false;
        return;
    }

    if (!g_waitingLogged) {
        Serial.println("[LIVE] Device online; publisher awaits the backend ingestion protocol");
        g_waitingLogged = true;
    }
}

bool isReady() {
    return false;
}
}  // namespace liveStreamPublisher