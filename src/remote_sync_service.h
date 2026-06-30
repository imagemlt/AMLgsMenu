#pragma once

#include "app_config.h"
#include "command_templates.h"
#include "command_transport.h"
#include "menu_state.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class UdpCommandClient;

class RemoteSyncService
{
public:
    using AcquireTransportFn = std::function<std::shared_ptr<CommandTransport>()>;
    using EnsureRunnerFn = std::function<bool()>;
    using EnqueueRemoteFn = std::function<void(std::function<void()>)>;

    RemoteSyncService(MenuState &state, CommandTemplates &templates, AppConfig &config);

    void SetHooks(AcquireTransportFn acquire_transport,
                  EnsureRunnerFn ensure_runner,
                  EnqueueRemoteFn enqueue_remote);
    void SetApMode(bool enabled) { ap_mode_.store(enabled, std::memory_order_release); }
    void RequestSync();
    void DrainPending();

private:
    struct Snapshot
    {
        bool has_channel = false;
        int channel = 0;
        bool has_bandwidth = false;
        int bandwidth = 0;
        bool has_power = false;
        int power = 0;
        bool has_mcs = false;
        int mcs = 0;
        bool has_bitrate = false;
        int bitrate_kbps = 0;
        bool has_sky_mode = false;
        int width = 0;
        int height = 0;
        int fps = 0;
    };

    void MaybeLaunch();
    bool CollectRemoteStateAp(Snapshot &snapshot, const std::shared_ptr<CommandTransport> &transport);
    bool FillSnapshotFromMap(const std::unordered_map<std::string, std::string> &values, Snapshot &snapshot);
    bool CollectRemoteState(Snapshot &snapshot, const std::shared_ptr<CommandTransport> &transport);
    bool CollectRemoteStateAsync(const std::shared_ptr<UdpCommandClient> &transport);
    void ApplySnapshot(const Snapshot &snapshot);

    static int FindChannelIndex(const MenuState &state, int channel_val);
    static int FindPowerIndex(const MenuState &state, int power_val);
    static int FindMcsIndex(const MenuState &state, int mcs_val);
    static int FindSkyModeIndex(const MenuState &state, int width, int height, int refresh);
    static int FindBitrateIndex(const MenuState &state, int bitrate_mbps);
    static std::string TrimCopy(const std::string &text);

    MenuState &state_;
    CommandTemplates &templates_;
    AppConfig &config_;
    AcquireTransportFn acquire_transport_;
    EnsureRunnerFn ensure_runner_;
    EnqueueRemoteFn enqueue_remote_;

    std::atomic<bool> request_flag_{false};
    std::atomic<bool> inflight_{false};
    std::mutex pending_mutex_;
    Snapshot pending_snapshot_{};
    bool pending_ready_ = false;
    std::atomic<bool> ap_mode_{false};
};
