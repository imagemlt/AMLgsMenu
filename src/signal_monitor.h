#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct GroundSignalSnapshot {
    float signal_a = 0.0f;
    float signal_b = 0.0f;
    bool valid = false;
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::time_point::min();
};

class SignalMonitor {
public:
    SignalMonitor();
    ~SignalMonitor();

    void Start();
    void Stop();
    GroundSignalSnapshot Latest() const;

private:
    void Run();
    bool UpdateSnapshot();
    static std::vector<std::string> SplitString(const std::string &line, char delim);

    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    GroundSignalSnapshot latest_;
};
