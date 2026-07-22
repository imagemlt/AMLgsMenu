#include "signal_monitor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
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
    bool ap_mode_enabled = false;
    std::string wlan_name;
    if (ReadApModeConfig(ap_mode_enabled, wlan_name) && ap_mode_enabled)
    {
        return UpdateSnapshotFromApLink();
    }

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

bool SignalMonitor::UpdateSnapshotFromApLink()
{
    bool ap_mode_enabled = false;
    std::string wlan_name;
    if (!ReadApModeConfig(ap_mode_enabled, wlan_name) || !ap_mode_enabled)
    {
        return false;
    }

    const std::string cmd = "iw dev " + wlan_name + " link 2>/dev/null";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
    {
        return false;
    }

    std::optional<float> signal_dbm;
    std::optional<float> tx_bitrate_mbps;
    std::optional<float> rx_bitrate_mbps;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe))
    {
        std::string line = TrimCopy(buffer);
        if (line.empty() || line == "Not connected.")
            continue;

        if (!signal_dbm.has_value())
        {
            signal_dbm = ParseSignalDbm(line);
        }
        if (line.rfind("tx bitrate:", 0) == 0)
        {
            tx_bitrate_mbps = ParseBitrateMbps(line);
        }
        else if (line.rfind("rx bitrate:", 0) == 0)
        {
            rx_bitrate_mbps = ParseBitrateMbps(line);
        }
    }
    pclose(pipe);

    const auto now = std::chrono::steady_clock::now();
    if (signal_dbm.has_value())
    {
        GroundSignalSnapshot snapshot;
        snapshot.signal_a = *signal_dbm;
        snapshot.signal_b = *signal_dbm;
        snapshot.valid = true;
        snapshot.timestamp = now;

        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = snapshot;
    }

    if (tx_bitrate_mbps.has_value() || rx_bitrate_mbps.has_value())
    {
        PacketRateSnapshot rate{};
        rate.primary_mbps = tx_bitrate_mbps.value_or(rx_bitrate_mbps.value_or(0.0f));
        rate.secondary_mbps = rx_bitrate_mbps.value_or(tx_bitrate_mbps.value_or(0.0f));
        rate.valid = (rate.primary_mbps > 0.0f || rate.secondary_mbps > 0.0f);
        rate.timestamp = now;

        std::lock_guard<std::mutex> lock(mutex_);
        latest_rate_ = rate;
    }

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

std::string SignalMonitor::TrimCopy(const std::string &text)
{
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t' ||
                                   text[start] == '\r' || text[start] == '\n'))
    {
        ++start;
    }
    size_t end = text.size();
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }
    return text.substr(start, end - start);
}

bool SignalMonitor::ReadApModeConfig(bool &ap_mode_enabled, std::string &wlan_name)
{
    ap_mode_enabled = false;
    wlan_name = "wlan0";

    const char *paths[] = {"/storage/digitalfpv/wfb.conf", "/flash/wfb.conf"};
    for (const char *path : paths)
    {
        std::ifstream in(path);
        if (!in.is_open())
            continue;

        std::string line;
        while (std::getline(in, line))
        {
            line = TrimCopy(line);
            if (line.empty() || line[0] == '#')
                continue;
            auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = TrimCopy(line.substr(0, eq));
            std::string value = TrimCopy(line.substr(eq + 1));
            if (key == "ap_mode")
            {
                ap_mode_enabled = (value == "1");
            }
            else if (key == "wlan" && !value.empty())
            {
                wlan_name = value;
            }
        }
        return true;
    }
    return false;
}

std::optional<float> SignalMonitor::ParseSignalDbm(const std::string &line)
{
    if (line.rfind("signal:", 0) != 0)
        return std::nullopt;
    std::string value = TrimCopy(line.substr(std::strlen("signal:")));
    auto pos = value.find(" dBm");
    if (pos != std::string::npos)
    {
        value = value.substr(0, pos);
    }
    try
    {
        return std::stof(value);
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}

std::optional<float> SignalMonitor::ParseBitrateMbps(const std::string &line)
{
    auto colon = line.find(':');
    if (colon == std::string::npos)
        return std::nullopt;
    std::string value = TrimCopy(line.substr(colon + 1));
    auto unit_pos = value.find(" MBit/s");
    if (unit_pos == std::string::npos)
        return std::nullopt;
    value = value.substr(0, unit_pos);
    auto first_space = value.find(' ');
    if (first_space != std::string::npos)
    {
        value = value.substr(0, first_space);
    }
    try
    {
        return std::stof(value);
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}
