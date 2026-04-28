#include "remote_sync_service.h"

#include "udp_command_client.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <vector>

RemoteSyncService::RemoteSyncService(MenuState &state, CommandTemplates &templates, AppConfig &config)
    : state_(state), templates_(templates), config_(config)
{
}

void RemoteSyncService::SetHooks(AcquireTransportFn acquire_transport,
                                 EnsureRunnerFn ensure_runner,
                                 EnqueueRemoteFn enqueue_remote)
{
    acquire_transport_ = std::move(acquire_transport);
    ensure_runner_ = std::move(ensure_runner);
    enqueue_remote_ = std::move(enqueue_remote);
}

void RemoteSyncService::RequestSync()
{
    request_flag_.store(true, std::memory_order_release);
    if (!ensure_runner_ || !ensure_runner_())
    {
        return;
    }
    MaybeLaunch();
}

void RemoteSyncService::DrainPending()
{
    Snapshot snapshot{};
    bool apply = false;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_ready_)
        {
            snapshot = pending_snapshot_;
            pending_ready_ = false;
            apply = true;
        }
    }
    if (apply)
    {
        ApplySnapshot(snapshot);
    }
}

void RemoteSyncService::MaybeLaunch()
{
    if (!enqueue_remote_ || !acquire_transport_)
    {
        return;
    }
    if (!request_flag_.load(std::memory_order_acquire))
    {
        return;
    }
    bool expected = false;
    if (!inflight_.compare_exchange_strong(expected, true))
    {
        return;
    }
    auto transport = acquire_transport_();
    if (!transport)
    {
        inflight_.store(false, std::memory_order_release);
        return;
    }
    request_flag_.store(false, std::memory_order_release);
    enqueue_remote_([this, transport]()
                    {
        if (auto udp = std::dynamic_pointer_cast<UdpCommandClient>(transport))
        {
            if (CollectRemoteStateAsync(udp))
            {
                return;
            }
        }

        Snapshot snapshot{};
        if (CollectRemoteState(snapshot, transport))
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_snapshot_ = snapshot;
            pending_ready_ = true;
        }
        inflight_.store(false, std::memory_order_release);
        MaybeLaunch(); });
}

bool RemoteSyncService::FillSnapshotFromMap(const std::unordered_map<std::string, std::string> &values,
                                            Snapshot &snapshot)
{
    auto try_parse_int = [](const std::string &text, int &value) -> bool
    {
        try
        {
            value = std::stoi(text);
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    bool any = false;
    auto batch_values = values;

    std::string value;
    if (!batch_values["channel"].empty())
    {
        value = batch_values["channel"];
        int ch = 0;
        if (try_parse_int(value, ch))
        {
            snapshot.has_channel = true;
            snapshot.channel = ch;
            any = true;
        }
    }
    if (!batch_values["bandwidth"].empty())
    {
        value = batch_values["bandwidth"];
        int bw = 0;
        if (try_parse_int(value, bw))
        {
            snapshot.has_bandwidth = true;
            snapshot.bandwidth = bw;
            any = true;
        }
    }
    if (!batch_values["sky_power"].empty())
    {
        value = batch_values["sky_power"];
        int p = 0;
        if (try_parse_int(value, p))
        {
            snapshot.has_power = true;
            snapshot.power = p;
            any = true;
        }
    }
    if (!batch_values["sky_mcs"].empty())
    {
        value = batch_values["sky_mcs"];
        int mcs = 0;
        if (try_parse_int(value, mcs))
        {
            snapshot.has_mcs = true;
            snapshot.mcs = mcs;
            any = true;
        }
    }
    if (!batch_values["bitrate"].empty())
    {
        value = batch_values["bitrate"];
        int kbps = 0;
        if (try_parse_int(value, kbps))
        {
            snapshot.has_bitrate = true;
            snapshot.bitrate_kbps = kbps;
            any = true;
        }
    }
    std::string size_value;
    std::string fps_value;
    bool have_size = false;
    bool have_fps = false;
    auto it_size = batch_values.find("sky_size");
    if (it_size != batch_values.end())
    {
        size_value = it_size->second;
        have_size = !size_value.empty();
    }
    auto it_fps = batch_values.find("sky_fps");
    if (it_fps != batch_values.end())
    {
        fps_value = it_fps->second;
        have_fps = !fps_value.empty();
    }
    if (have_size && have_fps)
    {
        int width = 0;
        int height = 0;
        int fps = 0;
        if (std::sscanf(size_value.c_str(), "%dx%d", &width, &height) == 2 && try_parse_int(fps_value, fps))
        {
            snapshot.has_sky_mode = true;
            snapshot.width = width;
            snapshot.height = height;
            snapshot.fps = fps;
            any = true;
        }
    }
    return any;
}

bool RemoteSyncService::CollectRemoteState(Snapshot &snapshot,
                                           const std::shared_ptr<CommandTransport> &transport)
{
    if (!transport)
    {
        return false;
    }
    std::unordered_map<std::string, std::string> batch_values;
    std::string batch_cmd;
    const std::vector<std::string> keys = {"channel", "bandwidth", "sky_power", "sky_mcs",
                                           "bitrate", "sky_size", "sky_fps"};
    for (const auto &key : keys)
    {
        auto cmd = templates_.Render("remote_query", key, {});
        if (cmd.empty())
            continue;
        if (!batch_cmd.empty())
            batch_cmd.append(" ; ");
        batch_cmd.append("echo __key__");
        batch_cmd.append(key);
        batch_cmd.append(" ; ");
        batch_cmd.append(cmd);
    }
    if (batch_cmd.empty())
        return false;
    std::vector<std::string> response;
    if (!transport->SendWithReply(batch_cmd, response, 1500))
        return false;
    std::string current_key;
    for (const auto &line : response)
    {
        auto trimmed = TrimCopy(line);
        if (trimmed.empty() || trimmed == "timeout")
            continue;
        if (trimmed.rfind("__key__", 0) == 0)
        {
            current_key = trimmed.substr(7);
            continue;
        }
        if (!current_key.empty() && batch_values.find(current_key) == batch_values.end())
        {
            batch_values[current_key] = trimmed;
        }
    }
    if (batch_values.empty())
        return false;
    return FillSnapshotFromMap(batch_values, snapshot);
}

bool RemoteSyncService::CollectRemoteStateAsync(const std::shared_ptr<UdpCommandClient> &transport)
{
    if (!transport)
        return false;
    std::string batch_cmd;
    const std::vector<std::string> keys = {"channel", "bandwidth", "sky_power", "sky_mcs",
                                           "bitrate", "sky_size", "sky_fps"};
    for (const auto &key : keys)
    {
        auto cmd = templates_.Render("remote_query", key, {});
        if (cmd.empty())
            continue;
        if (!batch_cmd.empty())
            batch_cmd.append(" ; ");
        batch_cmd.append("echo __key__");
        batch_cmd.append(key);
        batch_cmd.append(" ; ");
        batch_cmd.append(cmd);
    }
    if (batch_cmd.empty())
        return false;

    return transport->SendWithReplyAsync(
        batch_cmd, keys,
        [this](const std::unordered_map<std::string, std::string> &values, bool ok)
        {
            if (ok)
            {
                Snapshot snapshot{};
                if (FillSnapshotFromMap(values, snapshot))
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    pending_snapshot_ = snapshot;
                    pending_ready_ = true;
                }
            }
            inflight_.store(false, std::memory_order_release);
            MaybeLaunch();
        },
        1500);
}

void RemoteSyncService::ApplySnapshot(const Snapshot &snapshot)
{
    const bool notify_prev = state_.NotifyEnabled();
    state_.SetNotifyEnabled(false);
    if (snapshot.has_channel)
    {
        int idx = FindChannelIndex(state_, snapshot.channel);
        if (idx >= 0)
        {
            state_.SetChannelIndex(idx);
            config_.MutableValues()["channel"] = std::to_string(snapshot.channel);
            std::fprintf(stdout, "[AMLgsMenu] Remote channel synced: %d\n", snapshot.channel);
        }
    }
    if (snapshot.has_bandwidth)
    {
        int bw = snapshot.bandwidth;
        int idx = -1;
        if (bw == 10)
            idx = 0;
        else if (bw == 20)
            idx = 1;
        else if (bw == 40)
            idx = 2;
        if (idx >= 0)
        {
            state_.SetBandwidthIndex(idx);
            config_.MutableValues()["bandwidth"] = std::to_string(bw);
            std::fprintf(stdout, "[AMLgsMenu] Remote bandwidth synced: %d MHz\n", bw);
        }
    }
    if (snapshot.has_power)
    {
        int idx = FindPowerIndex(state_, snapshot.power);
        if (idx >= 0)
        {
            state_.SetSkyPowerIndex(idx);
            std::fprintf(stdout, "[AMLgsMenu] Remote TX power synced: %d\n", snapshot.power);
        }
    }
    if (snapshot.has_mcs)
    {
        int idx = FindMcsIndex(state_, snapshot.mcs);
        if (idx >= 0)
        {
            state_.SetSkyMcsIndex(idx);
            std::fprintf(stdout, "[AMLgsMenu] Remote MCS synced: %d\n", snapshot.mcs);
        }
    }
    if (snapshot.has_bitrate)
    {
        int kbps = snapshot.bitrate_kbps;
        int mbps = static_cast<int>(std::round(static_cast<double>(kbps) / 1024.0));
        if (mbps < 1)
            mbps = 1;
        int idx = FindBitrateIndex(state_, mbps);
        if (idx >= 0)
        {
            state_.SetBitrateIndex(idx);
            std::fprintf(stdout, "[AMLgsMenu] Remote bitrate synced: %d kbps\n", kbps);
        }
    }
    if (snapshot.has_sky_mode)
    {
        int idx = FindSkyModeIndex(state_, snapshot.width, snapshot.height, snapshot.fps);
        if (idx >= 0)
        {
            state_.SetSkyModeIndex(idx);
            std::fprintf(stdout, "[AMLgsMenu] Remote sky mode synced: %dx%d @ %dHz\n",
                         snapshot.width, snapshot.height, snapshot.fps);
        }
    }
    state_.SetNotifyEnabled(notify_prev);
}

int RemoteSyncService::FindChannelIndex(const MenuState &state, int channel_val)
{
    const auto &chs = state.Channels();
    for (size_t i = 0; i < chs.size(); ++i)
    {
        if (chs[i] == channel_val)
            return static_cast<int>(i);
    }
    return -1;
}

int RemoteSyncService::FindPowerIndex(const MenuState &state, int power_val)
{
    const auto &pwr = state.PowerLevels();
    for (size_t i = 0; i < pwr.size(); ++i)
    {
        if (pwr[i] == power_val)
            return static_cast<int>(i);
    }
    return -1;
}

int RemoteSyncService::FindMcsIndex(const MenuState &state, int mcs_val)
{
    const auto &mcs = state.McsLevels();
    for (size_t i = 0; i < mcs.size(); ++i)
    {
        if (mcs[i] == mcs_val)
            return static_cast<int>(i);
    }
    return -1;
}

int RemoteSyncService::FindSkyModeIndex(const MenuState &state, int width, int height, int refresh)
{
    const auto &modes = state.SkyModes();
    for (size_t i = 0; i < modes.size(); ++i)
    {
        if (modes[i].width == width && modes[i].height == height && modes[i].refresh == refresh)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int RemoteSyncService::FindBitrateIndex(const MenuState &state, int bitrate_mbps)
{
    const auto &bitrates = state.Bitrates();
    for (size_t i = 0; i < bitrates.size(); ++i)
    {
        if (bitrates[i] == bitrate_mbps)
            return static_cast<int>(i);
    }
    return -1;
}

std::string RemoteSyncService::TrimCopy(const std::string &text)
{
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
    {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}
