#pragma once

#include "command_templates.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct CustomOsdSnapshot {
    float x = 0.0f;
    float y = 0.0f;
    std::string text;
    bool valid = false;
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::time_point::min();
};

class CustomOsdMonitor {
public:
    explicit CustomOsdMonitor(std::vector<CommandTemplates::CustomOsdCommand> entries);
    ~CustomOsdMonitor();

    void Start();
    void Stop();
    std::vector<CustomOsdSnapshot> Latest() const;

private:
    void Run();
    std::string Execute(const std::string &cmd) const;

    const std::vector<CommandTemplates::CustomOsdCommand> entries_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::vector<CustomOsdSnapshot> snapshots_;
};
