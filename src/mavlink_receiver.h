#pragma once

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "common/mavlink.h"

struct ParsedTelemetry {
    // flags
    bool has_attitude = false;
    bool has_gps = false;
    bool has_home = false;
    bool has_battery = false;
    bool has_radio_rssi = false;
    bool has_sky_temp = false;
    bool has_flight_mode = false;
    // optionals
    bool has_video_metrics = false;
    // data
    float roll_deg = 0.0f;
    float pitch_deg = 0.0f;
    float yaw_deg = 0.0f;
    double latitude = 0.0;
    double longitude = 0.0;
    float altitude_m = 0.0f;
    double home_latitude = 0.0;
    double home_longitude = 0.0;
    float home_distance_m = 0.0f;
    std::string flight_mode{"UNKNOWN"};
    int rc_rssi = 0; // 0-255
    float batt_voltage_v = 0.0f;   // pack voltage in volts
    float cell_voltage_v = 0.0f;   // average cell voltage if available
    int cell_count = 0;            // number of valid cells seen
    int batt_remaining_pct = -1; // -1 if unknown
    float sky_temp_c = 0.0f;
    float video_bitrate_mbps = 0.0f;
    std::string video_resolution;
    int video_refresh_hz = 0;
};

class MavlinkReceiver {
public:
    explicit MavlinkReceiver(uint16_t udp_port = 14450);
    ~MavlinkReceiver();

    void Start();
    void Stop();
    ParsedTelemetry Latest() const;

private:
    void ThreadFunc();
    void HandleMessage(const mavlink_message_t &msg);
    void UpdateHomeDistanceLocked();
    static float HaversineMeters(double lat1, double lon1, double lat2, double lon2);
    static std::string ModeToString(uint8_t base_mode, uint32_t custom_mode, uint8_t autopilot);

    uint16_t port_;
    int sock_ = -1;
    std::thread worker_;
    std::atomic<bool> running_{false};
    bool first_msg_logged_ = false;
    uint8_t autopilot_type_ = MAV_AUTOPILOT_GENERIC;

    mutable std::mutex mtx_;
    ParsedTelemetry telem_;
};
