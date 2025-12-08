#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

struct GroundSignalSnapshot {
    float signal_a = 0.0f;
    float signal_b = 0.0f;
    bool valid = false;
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::time_point::min();
};

struct PacketRateSnapshot {
    float primary_mbps = 0.0f;
    float secondary_mbps = 0.0f;
    bool valid = false;
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::time_point::min();
};

class SignalMonitor {
public:
    SignalMonitor();
    ~SignalMonitor() = default;

    bool Poll();
    GroundSignalSnapshot Latest() const;
    PacketRateSnapshot LatestRate() const;

private:
    struct RateState {
        uint64_t bytes = 0;
        std::chrono::steady_clock::time_point ts{};
        bool valid = false;
    };

    bool UpdateSnapshot();
    void ProcessEntry(pid_t pid, const std::string &message,
                      std::map<uint64_t, float> &antenna_data,
                      bool &saw_pkt,
                      uint64_t &video_bytes,
                      uint64_t &first_ts,
                      uint64_t &last_ts);
    static std::vector<std::string> SplitString(const std::string &line, char delim);

    mutable std::mutex mutex_;
    GroundSignalSnapshot latest_;
    PacketRateSnapshot latest_rate_;
    std::unordered_map<std::string, RateState> rate_states_;
};
