#pragma once

namespace config {
constexpr uint16_t kStreamPort = 80;
constexpr uint32_t kWiFiRetryIntervalMs = 5000;
constexpr uint32_t kStreamStatsIntervalMs = 5000;
constexpr bool kWifiProvisioningEnabled = true;
constexpr char kFirmwareVersion[] = "1.0.0";
constexpr char kBackendBaseUrl[] = "http://10.227.237.179:3000/";
constexpr char kDeviceRegisterPath[] = "api/devices/register";
constexpr char kDeviceHeartbeatPath[] = "api/devices/heartbeat";
constexpr uint32_t kHeartbeatIntervalMs = 15000;
constexpr uint32_t kDeviceRegistrationRetryMs = 20000;
constexpr char kProvisioningApSsid[] = "DIPONeoCare-Setup";
constexpr char kProvisioningApPassword[] = "DIPONeoCare";
constexpr uint32_t kProvisioningApGateway = 19216841U;
constexpr uint32_t kProvisioningApSubnet = 2552552550U;
constexpr int kWifiResetPin = -1;
constexpr bool kWifiResetButtonEnabled = false;
}  // namespace config
