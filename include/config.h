#pragma once

#include <stdint.h>

namespace config {
constexpr uint16_t kStreamPort = 80;
constexpr uint32_t kWiFiRetryIntervalMs = 5000;
constexpr uint32_t kStreamStatsIntervalMs = 5000;
constexpr bool kWifiProvisioningEnabled = true;
constexpr char kFirmwareVersion[] = "1.0.0";
constexpr char kBackendBaseUrl[] = "https://capstone-diponeocare-production.up.railway.app/";
constexpr char kDeviceRegisterPath[] = "api/devices/register";
constexpr char kDeviceHeartbeatPath[] = "api/devices/heartbeat";
constexpr char kFrameUploadPath[] = "api/devices/upload/frame";
constexpr char kAudioUploadPath[] = "api/devices/upload/audio";
constexpr uint32_t kHeartbeatIntervalMs = 15000;
constexpr uint32_t kBackendOnlineTimeoutMs = 45000;
constexpr uint32_t kDeviceRegistrationRetryMs = 20000;
constexpr uint32_t kFrameUploadIntervalMs = 1000;
constexpr uint32_t kFrameTimingStatsIntervalMs = 10000;
constexpr uint8_t kFrameUploadQueueLength = 8;
constexpr uint32_t kFrameUploaderStatsIntervalMs = 30000;
constexpr uint32_t kFrameMaxQueuedAgeMs = 20000;
constexpr uint32_t kAudioFrameConfirmationWaitMs = 20000;
constexpr uint32_t kAudioFrameConfirmationPollMs = 250;
constexpr bool kLiveStreamPublisherEnabled = false;
constexpr uint32_t kLiveStreamTargetFps = 10;
constexpr char kLiveStreamIngestUrl[] = "";
constexpr char kProvisioningApSsid[] = "DIPONeoCare-Setup";
constexpr char kProvisioningApPassword[] = "DIPONeoCare";
constexpr uint32_t kProvisioningApGateway = 19216841U;
constexpr uint32_t kProvisioningApSubnet = 2552552550U;
constexpr int kWifiResetPin = -1;
constexpr bool kWifiResetButtonEnabled = false;
}  // namespace config
