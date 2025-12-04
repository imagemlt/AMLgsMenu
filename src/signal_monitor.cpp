#include "signal_monitor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace
{
const char *kJournalCmd = "journalctl -u wifibroadcast --since \"5 seconds ago\" --no-pager --output=cat";
const std::chrono::milliseconds kSleepSlice{100};
}

SignalMonitor::SignalMonitor() = default;

SignalMonitor::~SignalMonitor()
{
    Stop();
}

void SignalMonitor::Start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;
    worker_ = std::thread(&SignalMonitor::Run, this);
}

void SignalMonitor::Stop()
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

GroundSignalSnapshot SignalMonitor::Latest() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void SignalMonitor::Run()
{
    while (running_)
    {
        UpdateSnapshot();
        for (int i = 0; i < 20 && running_; ++i)
        {
            std::this_thread::sleep_for(kSleepSlice);
        }
    }
}

bool SignalMonitor::UpdateSnapshot()
{
    FILE *pipe = popen(kJournalCmd, "r");
    if (!pipe)
    {
        return false;
    }

    std::map<uint64_t, float> antenna_data;
    char buffer[1024];

    while (fgets(buffer, sizeof(buffer), pipe))
    {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        {
            line.pop_back();
        }
        if (line.empty())
            continue;

        auto fields = SignalMonitor::SplitString(line, '\t');
        if (fields.size() < 5)
            continue;
        if (fields[1] != "RX_ANT")
            continue;

        try
        {
            uint64_t antenna_id = std::stoull(fields[3], nullptr, 16);
            auto stats = SignalMonitor::SplitString(fields[4], ':');
            if (stats.size() < 7)
                continue;
            float rssi_val = static_cast<float>(std::stof(stats[2]));
            antenna_data[antenna_id] = rssi_val;
        }
        catch (const std::exception &)
        {
            continue;
        }
    }

    pclose(pipe);

    if (antenna_data.empty())
        return false;

    GroundSignalSnapshot snapshot;
    size_t idx = 0;
    for (const auto &entry : antenna_data)
    {
        if (idx == 0)
        {
            snapshot.signal_a = entry.second;
        }
        else if (idx == 1)
        {
            snapshot.signal_b = entry.second;
            break;
        }
        ++idx;
    }

    snapshot.valid = true;
    snapshot.timestamp = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = snapshot;
    return true;
}

std::vector<std::string> SignalMonitor::SplitString(const std::string &line, char delim)
{
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start <= line.size())
    {
        auto pos = line.find(delim, start);
        if (pos == std::string::npos)
        {
            tokens.emplace_back(line.substr(start));
            break;
        }
        tokens.emplace_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return tokens;
}
