#include "custom_osd_monitor.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace
{
constexpr std::chrono::milliseconds kInterval{2000};
}

CustomOsdMonitor::CustomOsdMonitor(std::vector<CommandTemplates::CustomOsdCommand> entries)
    : entries_(std::move(entries))
{
    snapshots_.resize(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i)
    {
        snapshots_[i].x = entries_[i].x;
        snapshots_[i].y = entries_[i].y;
    }
}

CustomOsdMonitor::~CustomOsdMonitor()
{
    Stop();
}

void CustomOsdMonitor::Start()
{
    if (entries_.empty())
        return;
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;
    worker_ = std::thread(&CustomOsdMonitor::Run, this);
}

void CustomOsdMonitor::Stop()
{
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false))
    {
        if (worker_.joinable())
        {
            worker_.join();
        }
    }
}

std::vector<CustomOsdSnapshot> CustomOsdMonitor::Latest() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshots_;
}

void CustomOsdMonitor::Run()
{
    while (running_)
    {
        for (size_t i = 0; i < entries_.size() && running_; ++i)
        {
            CustomOsdSnapshot snap;
            snap.x = entries_[i].x;
            snap.y = entries_[i].y;
            snap.text = Execute(entries_[i].command);
            snap.valid = !snap.text.empty();
            snap.timestamp = std::chrono::steady_clock::now();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshots_[i] = snap;
            }
        }

        std::this_thread::sleep_for(kInterval);
    }
}

std::string CustomOsdMonitor::Execute(const std::string &cmd) const
{
    std::array<char, 512> buffer{};
    std::string output;

    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
    {
        return {};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
    {
        output += buffer.data();
    }
    pclose(pipe);

    // keep only first line
    auto pos = output.find('\n');
    if (pos != std::string::npos)
    {
        output = output.substr(0, pos);
    }
    // trim
    while (!output.empty() && (output.back() == '\r' || output.back() == '\n' || output.back() == ' ' || output.back() == '\t'))
        output.pop_back();
    size_t start = 0;
    while (start < output.size() && (output[start] == ' ' || output[start] == '\t'))
        ++start;
    if (start > 0)
        output.erase(0, start);
    return output;
}
