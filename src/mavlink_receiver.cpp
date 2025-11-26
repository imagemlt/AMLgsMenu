#include "mavlink_receiver.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
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
        uint8_t base = mavlink_msg_heartbeat_get_base_mode(&msg);
        uint32_t custom = mavlink_msg_heartbeat_get_custom_mode(&msg);
        telem_.flight_mode = ModeToString(base, custom);
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
        telem_.has_battery = true;
        break;
    }
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
        int16_t volt_mv = mavlink_msg_battery_status_get_voltages(&msg, 0); // first cell or pack voltage
        if (volt_mv != UINT16_MAX) {
            telem_.batt_voltage_v = volt_mv / 1000.0f;
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

std::string MavlinkReceiver::ModeToString(uint8_t base_mode, uint32_t custom_mode) {
    if (base_mode & MAV_MODE_FLAG_AUTO_ENABLED) return "AUTO";
    if (base_mode & MAV_MODE_FLAG_GUIDED_ENABLED) return "GUIDED";
    if (base_mode & MAV_MODE_FLAG_STABILIZE_ENABLED) return "STABILIZE";
    if (base_mode & MAV_MODE_FLAG_MANUAL_INPUT_ENABLED) return "MANUAL";
    (void)custom_mode;
    return "UNKNOWN";
}
