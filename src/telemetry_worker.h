#pragma once

#include "signal_monitor.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

class TelemetryWorker {
public:
    struct Snapshot {
        GroundSignalSnapshot ground_signal{};
        PacketRateSnapshot packet_rate{};
        float ground_temp_c = 0.0f;
        bool has_ground_temp = false;
        int output_fps = 0;
        std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::time_point::min();
    };

    TelemetryWorker(SignalMonitor *signal_monitor);
    ~TelemetryWorker();

    void Start();
    void Stop();
    Snapshot Latest() const;

private:
    void ThreadMain();

    SignalMonitor *signal_monitor_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    Snapshot latest_;
};
