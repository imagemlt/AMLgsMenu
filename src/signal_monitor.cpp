#include "signal_monitor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <vector>

namespace
{
const char *kJournalCmd = "journalctl -u wifibroadcast --since \"5 seconds ago\" --no-pager --output=export";
}

SignalMonitor::SignalMonitor() = default;
bool SignalMonitor::Poll()
{
    return UpdateSnapshot();
}

GroundSignalSnapshot SignalMonitor::Latest() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

PacketRateSnapshot SignalMonitor::LatestRate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_rate_;
}

bool SignalMonitor::UpdateSnapshot()
{
    FILE *pipe = popen(kJournalCmd, "r");
    if (!pipe)
    {
        return false;
    }

    std::map<uint64_t, float> antenna_data;
    std::chrono::steady_clock::time_point log_ts = std::chrono::steady_clock::now();
    char buffer[1024];
    bool saw_pkt = false;

    pid_t current_pid = -1;
    std::string current_msg;
    auto flush_entry = [&]() {
        if (!current_msg.empty())
        {
            ProcessEntry(current_pid, current_msg, antenna_data, saw_pkt);
        }
        current_pid = -1;
        current_msg.clear();
    };

    while (fgets(buffer, sizeof(buffer), pipe))
    {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        {
            line.pop_back();
        }
        if (line.empty())
        {
            flush_entry();
            continue;
        }

        if (line.rfind("MESSAGE=", 0) == 0)
        {
            current_msg = line.substr(8);
        }
        else if (line.rfind("_PID=", 0) == 0)
        {
            try
            {
                current_pid = static_cast<pid_t>(std::stoi(line.substr(5)));
            }
            catch (const std::exception &)
            {
                current_pid = -1;
            }
        }
    }
    flush_entry();

    pclose(pipe);

    if (!antenna_data.empty())
    {
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
        snapshot.timestamp = log_ts;

        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = snapshot;
    }

    if (saw_pkt)
    {
        std::unordered_map<std::string, uint64_t> byte_data;
        for (const auto &entry : pid_bytes_)
        {
            auto info_it = pid_cache_.find(entry.first);
            if (info_it == pid_cache_.end())
                continue;
            const auto &info = info_it->second;
            if (!info.resolved || info.cls == StreamClass::Unknown)
                continue;
            byte_data[ClassKey(info.cls)] += entry.second;
        }

        if (byte_data.empty())
            return true;

        auto now = std::chrono::steady_clock::now();
        PacketRateSnapshot rate{};
        rate.timestamp = now;

        auto calc_rate = [&](const std::string &name) -> float {
            auto it = byte_data.find(name);
            if (it == byte_data.end())
                return 0.0f;
            auto &state = rate_states_[name];
            float mbps = 0.0f;
            if (state.valid)
            {
                double dt = std::chrono::duration<double>(now - state.ts).count();
                if (dt > 0.0 && it->second >= state.bytes)
                {
                    uint64_t delta = it->second - state.bytes;
                    mbps = static_cast<float>((static_cast<double>(delta) * 8.0) / (1024.0 * 1024.0) / dt);
                }
            }
            state.bytes = it->second;
            state.ts = now;
            state.valid = true;
            return mbps;
        };

        rate.primary_mbps = calc_rate("video");
        rate.secondary_mbps = calc_rate("aux");

        rate.valid = (rate.primary_mbps > 0.0f || rate.secondary_mbps > 0.0f);

        std::lock_guard<std::mutex> lock(mutex_);
        latest_rate_ = rate;
    }

    return true;
}

void SignalMonitor::ProcessEntry(pid_t pid, const std::string &message,
                                 std::map<uint64_t, float> &antenna_data,
                                 bool &saw_pkt)
{
    if (message.empty())
        return;

    auto fields = SignalMonitor::SplitString(message, '\t');
    if (fields.size() < 2)
        return;

    if (fields[1] == "RX_ANT")
    {
        if (fields.size() < 5)
            return;
        try
        {
            uint64_t antenna_id = std::stoull(fields[3], nullptr, 16);
            auto stats = SignalMonitor::SplitString(fields[4], ':');
            if (stats.size() < 7)
                return;
            float rssi_val = static_cast<float>(std::stof(stats[2]));
            antenna_data[antenna_id] = rssi_val;
        }
        catch (const std::exception &)
        {
            return;
        }
    }
    else if (fields[1] == "PKT")
    {
        if (fields.size() < 3 || pid <= 0)
            return;
        auto stats = SignalMonitor::SplitString(fields[2], ':');
        if (stats.size() < 9)
            return;
        try
        {
            auto info = ResolvePidInfo(pid);
            if (!info.resolved || info.cls == StreamClass::Unknown)
                return;
            uint64_t bytes_out = static_cast<uint64_t>(std::stoull(stats[8]));
            pid_bytes_[pid] = bytes_out;
            saw_pkt = true;
        }
        catch (const std::exception &)
        {
            return;
        }
    }
}

SignalMonitor::ProcessInfo SignalMonitor::ResolvePidInfo(pid_t pid)
{
    auto it = pid_cache_.find(pid);
    if (it != pid_cache_.end() && it->second.resolved)
    {
        return it->second;
    }

    ProcessInfo info;
    info.resolved = true;

    std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        pid_cache_[pid] = info;
        return info;
    }

    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::vector<std::string> args;
    size_t start = 0;
    for (size_t i = 0; i < data.size(); ++i)
    {
        if (data[i] == '\0')
        {
            if (i > start)
            {
                args.emplace_back(data.data() + start, i - start);
            }
            start = i + 1;
        }
    }
    if (start < data.size())
    {
        args.emplace_back(data.data() + start, data.size() - start);
    }

    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "-p" && (i + 1) < args.size())
        {
            try
            {
                info.stream_id = std::stoi(args[i + 1], nullptr, 0);
            }
            catch (const std::exception &)
            {
                info.stream_id = -1;
            }
            break;
        }
    }

    if (info.stream_id >= 0)
    {
        info.cls = ClassFromStream(info.stream_id);
    }

    pid_cache_[pid] = info;
    return info;
}

SignalMonitor::StreamClass SignalMonitor::ClassFromStream(int stream_id)
{
    if (stream_id >= 0x00 && stream_id <= 0x0f)
        return StreamClass::Video;
    if ((stream_id >= 0x10 && stream_id <= 0x1f) || (stream_id >= 0x20 && stream_id <= 0x2f))
        return StreamClass::Aux;
    return StreamClass::Unknown;
}

std::string SignalMonitor::ClassKey(StreamClass cls)
{
    switch (cls)
    {
    case StreamClass::Video:
        return "video";
    case StreamClass::Aux:
        return "aux";
    default:
        return "unknown";
    }
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
