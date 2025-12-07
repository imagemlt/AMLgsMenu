#include "telemetry_worker.h"

#include "video_mode.h"

#include <thread>

namespace {
constexpr std::chrono::seconds kLoopSleep{1};
constexpr std::chrono::seconds kSignalInterval{2};
constexpr std::chrono::seconds kTempInterval{1};
constexpr std::chrono::seconds kFpsInterval{1};
}

TelemetryWorker::TelemetryWorker(SignalMonitor *signal_monitor)
    : signal_monitor_(signal_monitor) {}

TelemetryWorker::~TelemetryWorker() {
    Stop();
}

void TelemetryWorker::Start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&TelemetryWorker::ThreadMain, this);
}

void TelemetryWorker::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

TelemetryWorker::Snapshot TelemetryWorker::Latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void TelemetryWorker::ThreadMain() {
    auto last_signal = std::chrono::steady_clock::time_point{};
    auto last_temp = std::chrono::steady_clock::time_point{};
    auto last_fps = std::chrono::steady_clock::time_point{};

    while (running_) {
        const auto now = std::chrono::steady_clock::now();

        if (signal_monitor_) {
            if (last_signal.time_since_epoch().count() == 0 ||
                (now - last_signal) >= kSignalInterval) {
                signal_monitor_->Poll();
                last_signal = now;
            }
        }

        Snapshot snap;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap = latest_;
        }

        if (signal_monitor_) {
            snap.ground_signal = signal_monitor_->Latest();
            snap.packet_rate = signal_monitor_->LatestRate();
        }
        bool updated = false;
        if (last_temp.time_since_epoch().count() == 0 ||
            (now - last_temp) >= kTempInterval) {
            snap.ground_temp_c = ReadTemperatureC();
            snap.has_ground_temp = true;
            last_temp = now;
            updated = true;
        }

        if (last_fps.time_since_epoch().count() == 0 ||
            (now - last_fps) >= kFpsInterval) {
            snap.output_fps = GetOutputFps();
            last_fps = now;
            updated = true;
        }

        if (signal_monitor_) {
            updated = true;
        }

        if (updated) {
            snap.timestamp = now;
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = snap;
        }

        if (!running_) {
            break;
        }
        std::this_thread::sleep_for(kLoopSleep);
    }
}
