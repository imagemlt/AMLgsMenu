#include "signal_monitor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    uint64_t video_bytes = 0;
    uint64_t first_ts = 0;
    uint64_t last_ts = 0;
    std::chrono::steady_clock::time_point log_ts = std::chrono::steady_clock::now();
    char buffer[1024];
    bool saw_pkt = false;

    pid_t current_pid = -1;
    std::string current_msg;
    auto flush_entry = [&]() {
        if (!current_msg.empty())
        {
            ProcessEntry(current_pid, current_msg, antenna_data, saw_pkt, video_bytes, first_ts, last_ts);
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

    if (!saw_pkt)
        return true;

    auto now = std::chrono::steady_clock::now();
    PacketRateSnapshot rate{};
    rate.timestamp = now;
    auto &state = rate_states_["video"];

    double dt = 0.0;
    if (last_ts > first_ts)
    {
        dt = static_cast<double>(last_ts - first_ts) / 1000.0;
    }
    if (dt <= 0.0)
    {
        if (state.valid)
            dt = std::chrono::duration<double>(now - state.ts).count();
    }
    if (dt <= 0.0)
    {
        dt = 1.0;
    }

    rate.primary_mbps = static_cast<float>((static_cast<double>(video_bytes) * 8.0) /
                                           (1024.0 * 1024.0) / dt);
    state.bytes = video_bytes;
    state.ts = now;
    state.valid = true;
    rate.valid = (video_bytes > 0);

    std::lock_guard<std::mutex> lock(mutex_);
    latest_rate_ = rate;

    return true;
}

void SignalMonitor::ProcessEntry(pid_t pid, const std::string &message,
                                 std::map<uint64_t, float> &antenna_data,
                                 bool &saw_pkt,
                                 uint64_t &video_bytes,
                                 uint64_t &first_ts,
                                 uint64_t &last_ts)
{
    (void)pid;
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
        uint64_t log_ts = 0;
        try
        {
            log_ts = static_cast<uint64_t>(std::stoull(fields[0]));
        }
        catch (const std::exception &)
        {
            log_ts = 0;
        }

        auto stats = SignalMonitor::SplitString(fields[2], ':');
        if (stats.size() < 11)
            return;
        uint64_t bytes_out = 0;
        try
        {
            bytes_out = static_cast<uint64_t>(std::stoull(stats.back()));
        }
        catch (const std::exception &)
        {
            return;
        }

        video_bytes += bytes_out;
        if (log_ts > 0)
        {
            if (first_ts == 0 || log_ts < first_ts)
                first_ts = log_ts;
            if (log_ts > last_ts)
                last_ts = log_ts;
        }
        saw_pkt = true;
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
