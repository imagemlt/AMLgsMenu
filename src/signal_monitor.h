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
    void SetPrimaryStreams(const std::vector<std::string> &names) { primary_streams_ = names; }
    void SetSecondaryStreams(const std::vector<std::string> &names) { secondary_streams_ = names; }

private:
    enum class StreamClass { Unknown, Video, Aux };
    struct ProcessInfo {
        bool resolved = false;
        int stream_id = -1;
        StreamClass cls = StreamClass::Unknown;
    };
    struct RateState {
        uint64_t bytes = 0;
        std::chrono::steady_clock::time_point ts{};
        bool valid = false;
    };

    bool UpdateSnapshot();
    void ProcessEntry(pid_t pid, const std::string &message,
                      std::map<uint64_t, float> &antenna_data,
                      bool &saw_pkt);
    ProcessInfo ResolvePidInfo(pid_t pid);
    static StreamClass ClassFromStream(int stream_id);
    static std::string ClassKey(StreamClass cls);
    static std::vector<std::string> SplitString(const std::string &line, char delim);

    mutable std::mutex mutex_;
    GroundSignalSnapshot latest_;
    PacketRateSnapshot latest_rate_;
    std::unordered_map<std::string, RateState> rate_states_;
    std::unordered_map<pid_t, ProcessInfo> pid_cache_;
    std::unordered_map<pid_t, uint64_t> pid_bytes_;
    std::vector<std::string> primary_streams_;
    std::vector<std::string> secondary_streams_;
};
