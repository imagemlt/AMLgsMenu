#include "mavlink_receiver.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <climits>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/resource.h>

namespace {
constexpr double kDegToRad = M_PI / 180.0;
constexpr double kEarthRadiusM = 6371000.0;
}

MavlinkReceiver::MavlinkReceiver(uint16_t udp_port) : port_(udp_port) {}

MavlinkReceiver::~MavlinkReceiver() { Stop(); }

void MavlinkReceiver::Start() {
    if (running_) return;
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) {
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
    addr.sin_port = htons(port_);
    if (bind(sock_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(sock_);
        sock_ = -1;
        return;
    }
    running_ = true;
    worker_ = std::thread(&MavlinkReceiver::ThreadFunc, this);
}

void MavlinkReceiver::Stop() {
    running_ = false;
    if (sock_ >= 0) {
        shutdown(sock_, SHUT_RDWR);
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}

ParsedTelemetry MavlinkReceiver::Latest() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return telem_;
}

void MavlinkReceiver::ThreadFunc() {
    uint8_t buf[1500];
    sockaddr_in src{};
    socklen_t srclen = sizeof(src);
    mavlink_status_t status{};
    mavlink_message_t msg{};

    // Lower thread priority to avoid impacting video pipeline
#ifdef __linux__
    sched_param sp{};
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    setpriority(PRIO_PROCESS, 0, 5);
#endif

    while (running_) {
        ssize_t n = recvfrom(sock_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&src), &srclen);
        if (n <= 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!running_) break;
            continue;
        }
        for (ssize_t i = 0; i < n; ++i) {
            if (mavlink_parse_char(MAVLINK_COMM_0, buf[i], &msg, &status)) {
                HandleMessage(msg);
            }
        }
    }
}

void MavlinkReceiver::HandleMessage(const mavlink_message_t &msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!first_msg_logged_) {
        std::fprintf(stdout, "[AMLgsMenu] First MAVLink message received (id=%u)\n", msg.msgid);
        std::fflush(stdout);
        first_msg_logged_ = true;
    }
    switch (msg.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
        autopilot_type_ = mavlink_msg_heartbeat_get_autopilot(&msg);
        uint8_t base = mavlink_msg_heartbeat_get_base_mode(&msg);
        uint32_t custom = mavlink_msg_heartbeat_get_custom_mode(&msg);
        telem_.flight_mode = ModeToString(base, custom, autopilot_type_);
        telem_.has_flight_mode = (telem_.flight_mode != "UNKNOWN");
        break;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
        telem_.roll_deg = mavlink_msg_attitude_get_roll(&msg) * 180.0f / static_cast<float>(M_PI);
        telem_.pitch_deg = mavlink_msg_attitude_get_pitch(&msg) * 180.0f / static_cast<float>(M_PI);
        telem_.yaw_deg = mavlink_msg_attitude_get_yaw(&msg) * 180.0f / static_cast<float>(M_PI);
        telem_.has_attitude = true;
        break;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        telem_.latitude = mavlink_msg_gps_raw_int_get_lat(&msg) / 1e7;
        telem_.longitude = mavlink_msg_gps_raw_int_get_lon(&msg) / 1e7;
        telem_.altitude_m = mavlink_msg_gps_raw_int_get_alt(&msg) / 1000.0f;
        telem_.has_gps = true;
        UpdateHomeDistanceLocked();
        break;
    }
    case MAVLINK_MSG_ID_HOME_POSITION: {
        telem_.home_latitude = mavlink_msg_home_position_get_latitude(&msg) / 1e7;
        telem_.home_longitude = mavlink_msg_home_position_get_longitude(&msg) / 1e7;
        telem_.has_home = true;
        UpdateHomeDistanceLocked();
        break;
    }
    case MAVLINK_MSG_ID_RC_CHANNELS_RAW: {
        telem_.rc_rssi = mavlink_msg_rc_channels_raw_get_rssi(&msg);
        telem_.has_radio_rssi = true;
        break;
    }
    case MAVLINK_MSG_ID_RAW_IMU: {
        if (msg.compid == MAV_COMP_ID_SYSTEM_CONTROL) {
            // Non-standard: temperature packed at offset 27 (as in Digi app)
            int16_t temp = _MAV_RETURN_int16_t(&msg, 24 + 2 + 1);
            telem_.sky_temp_c = static_cast<float>(temp) / 100.0f;
            telem_.has_sky_temp = true;
        }
        break;
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
        telem_.batt_voltage_v = mavlink_msg_sys_status_get_voltage_battery(&msg) / 1000.0f; // mV -> V
        telem_.batt_remaining_pct = mavlink_msg_sys_status_get_battery_remaining(&msg);     // 0-100, -1 unknown
        telem_.cell_count = 0;      // unknown from this message
        telem_.cell_voltage_v = 0.0f;
        telem_.has_battery = true;
        break;
    }
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
        // Use per-cell voltages if present
        float sum_v = 0.0f;
        int valid_cells = 0;
        uint16_t mv_cells[10] = {0};
        mavlink_msg_battery_status_get_voltages(&msg, mv_cells);
        for (int i = 0; i < 10; ++i) {
            uint16_t mv = mv_cells[i];
            if (mv != UINT16_MAX && mv != 0) {
                sum_v += static_cast<float>(mv) / 1000.0f;
                ++valid_cells;
            }
        }
        if (valid_cells > 0) {
            telem_.cell_count = valid_cells;
            telem_.cell_voltage_v = sum_v / static_cast<float>(valid_cells);
            telem_.batt_voltage_v = sum_v; // pack voltage = sum of cells
            telem_.has_battery = true;
        }
        int8_t rem = mavlink_msg_battery_status_get_battery_remaining(&msg);
        if (rem >= 0) {
            telem_.batt_remaining_pct = rem;
        }
        break;
    }
    default:
        break;
    }
}

void MavlinkReceiver::UpdateHomeDistanceLocked() {
    if (telem_.has_home && telem_.has_gps) {
        telem_.home_distance_m = HaversineMeters(telem_.latitude, telem_.longitude,
                                                 telem_.home_latitude, telem_.home_longitude);
    }
}

float MavlinkReceiver::HaversineMeters(double lat1, double lon1, double lat2, double lon2) {
    double dlat = (lat2 - lat1) * kDegToRad;
    double dlon = (lon2 - lon1) * kDegToRad;
    double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
               std::cos(lat1 * kDegToRad) * std::cos(lat2 * kDegToRad) *
               std::sin(dlon / 2) * std::sin(dlon / 2);
    double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    return static_cast<float>(kEarthRadiusM * c);
}

std::string MavlinkReceiver::ModeToString(uint8_t base_mode, uint32_t custom_mode, uint8_t autopilot) {
    // ArduPilot (Copter) custom_mode map
    if (autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA) {
        switch (custom_mode) {
        case 0: return "STABILIZE";
        case 1: return "ACRO";
        case 2: return "ALT_HOLD";
        case 3: return "AUTO";
        case 4: return "GUIDED";
        case 5: return "LOITER";
        case 6: return "RTL";
        case 7: return "CIRCLE";
        case 8: return "LAND";
        case 9: return "DRIFT";
        case 10: return "SPORT";
        case 11: return "FLIP";
        case 12: return "AUTOTUNE";
        case 13: return "POSHOLD";
        case 14: return "BRAKE";
        case 15: return "THROW";
        case 16: return "AVOID_ADSB";
        case 17: return "GUIDED_NOGPS";
        case 18: return "SMARTRTL";
        case 19: return "FLOWHOLD";
        case 20: return "FOLLOW";
        case 21: return "ZIGZAG";
        case 22: return "SYSTEMID";
        case 23: return "AUTOROTATE";
        case 24: return "AUTO_RTL";
        default: break;
        }
    }

    // PX4 custom mode: main_mode (bits 16-23), sub_mode (bits 24-31)
    if (autopilot == MAV_AUTOPILOT_PX4) {
        uint8_t main_mode = (custom_mode >> 16) & 0xFF;
        uint8_t sub_mode = (custom_mode >> 24) & 0xFF;
        switch (main_mode) {
        case 1: return "MANUAL";
        case 2: return "ALTCTL";
        case 3: return "POSCTL";
        case 4: {
            switch (sub_mode) {
            case 1: return "AUTO_READY";
            case 2: return "AUTO_TAKEOFF";
            case 3: return "AUTO_LOITER";
            case 4: return "AUTO_MISSION";
            case 5: return "AUTO_RTL";
            case 6: return "AUTO_LAND";
            case 7: return "AUTO_RTGS";
            case 8: return "AUTO_FOLLOW";
            case 9: return "AUTO_PRECLAND";
            default: return "AUTO";
            }
        }
        case 5: return "ACRO";
        case 6: return "OFFBOARD";
        case 7: return "STABILIZED";
        case 8: return "RATTITUDE";
        default: break;
        }
    }

    // Generic autopilot: try INAV-style mapping; Betaflight often keeps custom_mode=0
    if (autopilot == MAV_AUTOPILOT_GENERIC) {
        switch (custom_mode) {
        case 0: return "ACRO";
        case 1: return "ANGLE";
        case 2: return "HORIZON";
        case 3: return "ALTHOLD";
        case 4: return "CRUISE";
        case 5: return "POSHOLD";
        case 6: return "RTH";
        case 7: return "NAV_WP";
        case 8: return "LAND";
        case 9: return "FAILSAFE";
        case 10: return "GPS_RESCUE";
        case 11: return "LAUNCH";
        default: break; // Betaflight likely leaves custom_mode=0, fall through to base flags
        }
    }

    // Base mode flags as a fallback
    if (base_mode & MAV_MODE_FLAG_AUTO_ENABLED) return "AUTO";
    if (base_mode & MAV_MODE_FLAG_GUIDED_ENABLED) return "GUIDED";
    if (base_mode & MAV_MODE_FLAG_STABILIZE_ENABLED) return "STABILIZE";
    if (base_mode & MAV_MODE_FLAG_MANUAL_INPUT_ENABLED) return "MANUAL";

    return "UNKNOWN";
}
