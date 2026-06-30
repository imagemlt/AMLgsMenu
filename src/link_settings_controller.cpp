#include "link_settings_controller.h"

#include "udp_command_client.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <unordered_map>

LinkSettingsController::LinkSettingsController(MenuState &state, CommandTemplates &templates)
    : state_(state), templates_(templates)
{
}

void LinkSettingsController::SetHooks(AcquireTransportFn acquire_transport,
                                      EnqueueRemoteFn enqueue_remote,
                                      EnqueueShellFn enqueue_shell)
{
    acquire_transport_ = std::move(acquire_transport);
    enqueue_remote_ = std::move(enqueue_remote);
    enqueue_shell_ = std::move(enqueue_shell);
}

void LinkSettingsController::ApplyChannel()
{
    const auto &chs = state_.Channels();
    if (chs.empty())
        return;
    int ch = chs[state_.ChannelIndex()];
    if (ap_mode_)
    {
        if (acquire_transport_ && enqueue_remote_)
        {
            if (auto transport = acquire_transport_())
            {
                std::string cmd = "set_ap_channel " + std::to_string(ch);
                enqueue_remote_([transport, cmd]()
                                {
                    std::vector<std::string> resp;
                    if (!transport->SendWithReply(cmd, resp, 2000))
                    {
                        std::fprintf(stderr, "[AMLgsMenu] AP set_ap_channel %s failed\n", cmd.c_str());
                    }
                    else
                    {
                        for (const auto &line : resp)
                            std::fprintf(stdout, "[AMLgsMenu] AP set_ap_channel resp: %s\n", line.c_str());
                    } });
            }
        }
        return;
    }
    if (acquire_transport_ && enqueue_remote_)
    {
        if (auto transport = acquire_transport_())
        {
            std::unordered_map<std::string, std::string> vars{{"CHANNEL", std::to_string(ch)}};
            auto cmd = templates_.Render("remote", "channel", vars);
            if (!cmd.empty())
            {
                enqueue_remote_([transport, cmd]()
                                {
                    if (!transport->Send(cmd, false))
                    {
                        std::fprintf(stderr, "[AMLgsMenu] Failed to send channel command\n");
                    } });
            }
        }
    }
    ApplyLocalMonitorChannel(ch);
}

void LinkSettingsController::ApplyBandwidth()
{
    if (ap_mode_)
        return;
    int bw = (state_.BandwidthIndex() == 0) ? 10 : (state_.BandwidthIndex() == 1 ? 20 : 40);
    if (acquire_transport_ && enqueue_remote_)
    {
        if (auto transport = acquire_transport_())
        {
            std::unordered_map<std::string, std::string> vars{{"BANDWIDTH", std::to_string(bw)}};
            auto cmd = templates_.Render("remote", "bandwidth", vars);
            if (!cmd.empty())
            {
                enqueue_remote_([transport, cmd]()
                                {
                    if (!transport->Send(cmd, false))
                    {
                        std::fprintf(stderr, "[AMLgsMenu] Failed to send bandwidth command\n");
                    } });
            }
        }
    }
}

void LinkSettingsController::ApplySkyMode()
{
    const auto &sky_modes = state_.SkyModes();
    if (sky_modes.empty())
        return;
    VideoMode mode = sky_modes[state_.SkyModeIndex()];
    if (acquire_transport_ && enqueue_remote_)
    {
        if (auto transport = acquire_transport_())
        {
            std::unordered_map<std::string, std::string> vars{
                {"WIDTH", std::to_string(mode.width)},
                {"HEIGHT", std::to_string(mode.height)},
                {"FPS", std::to_string(mode.refresh ? mode.refresh : 60)}};
            auto cmd = templates_.Render("remote", "sky_mode", vars);
            if (!cmd.empty())
            {
                enqueue_remote_([transport, cmd]()
                                {
                    if (!transport->Send(cmd, false))
                    {
                        std::fprintf(stderr, "[AMLgsMenu] Failed to send sky mode command\n");
                    } });
            }
        }
    }
}

void LinkSettingsController::ApplyGroundDisplayMode(const std::string &label)
{
    std::ofstream ofs("/sys/class/display/mode");
    if (!ofs.is_open())
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to open /sys/class/display/mode for writing\n");
        return;
    }
    ofs << label << std::endl;
    if (!ofs.good())
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to write ground display mode '%s'\n", label.c_str());
    }
}

void LinkSettingsController::ApplyBitrate()
{
    const auto &bitrates = state_.Bitrates();
    if (bitrates.empty())
        return;
    int br_mbps = bitrates[state_.BitrateIndex()];
    int br_kbps = br_mbps * 1024;
    {
        const auto &mcs_levels = state_.McsLevels();
        if (!mcs_levels.empty())
        {
            float required_rate = static_cast<float>(br_mbps) * 2.0f;
            if (br_mbps > 4)
            {
                required_rate += 1.0f;
            }
            const float rates_10[] = {3.25f, 6.5f, 9.75f, 13.0f, 19.5f, 26.0f, 29.25f, 32.5f, 39.0f, 43.3f};
            const float rates_20[] = {6.5f, 13.0f, 19.5f, 26.0f, 39.0f, 52.0f, 58.5f, 65.0f, 78.0f, 86.7f};
            const float rates_40[] = {13.5f, 27.0f, 40.5f, 54.0f, 81.0f, 108.0f, 121.5f, 135.0f, 162.0f, 180.0f};
            const float *rates = rates_20;
            switch (state_.BandwidthIndex())
            {
            case 0:
                rates = rates_10;
                break;
            case 2:
                rates = rates_40;
                break;
            default:
                rates = rates_20;
                break;
            }

            int required_mcs = 9;
            for (int i = 0; i < 10; ++i)
            {
                if (rates[i] >= required_rate)
                {
                    required_mcs = i;
                    break;
                }
            }
            int current_index = state_.SkyMcsIndex();
            if (current_index < 0 || current_index >= static_cast<int>(mcs_levels.size()))
            {
                current_index = 0;
            }
            int current_mcs = mcs_levels[current_index];
            if (current_mcs != required_mcs)
            {
                int idx = FindMcsIndex(state_, required_mcs);
                if (idx < 0)
                {
                    idx = static_cast<int>(mcs_levels.size()) - 1;
                }
                if (idx >= 0 && idx < static_cast<int>(mcs_levels.size()))
                {
                    state_.SetSkyMcsIndex(idx);
                }
            }
        }
    }
    if (acquire_transport_ && enqueue_remote_)
    {
        if (auto transport = acquire_transport_())
        {
            std::unordered_map<std::string, std::string> vars{{"BITRATE_KBPS", std::to_string(br_kbps)}};
            auto cmd = templates_.Render("remote", "bitrate", vars);
            if (!cmd.empty())
            {
                enqueue_remote_([transport, cmd]()
                                {
                    if (!transport->Send(cmd, false))
                    {
                        std::fprintf(stderr, "[AMLgsMenu] Failed to send bitrate command\n");
                    } });
            }
        }
    }
}

void LinkSettingsController::ApplySkyMcs()
{
    const auto &mcs_levels = state_.McsLevels();
    if (mcs_levels.empty())
        return;
    int idx = state_.SkyMcsIndex();
    if (idx < 0 || idx >= static_cast<int>(mcs_levels.size()))
        return;
    int mcs = mcs_levels[idx];
    if (acquire_transport_ && enqueue_remote_)
    {
        if (auto transport = acquire_transport_())
        {
            std::unordered_map<std::string, std::string> vars{{"MCS", std::to_string(mcs)}};
            auto cmd = templates_.Render("remote", "sky_mcs", vars);
            if (!cmd.empty())
            {
                enqueue_remote_([transport, cmd]()
                                {
                    if (!transport->Send(cmd, false))
                    {
                        std::fprintf(stderr, "[AMLgsMenu] Failed to send sky MCS command\n");
                    } });
            }
        }
    }
}

void LinkSettingsController::RequestIdrFrame()
{
    if (idr_requested_)
        return;
    auto cmd = templates_.Render("remote", "request_idr", {});
    if (cmd.empty() || !acquire_transport_ || !enqueue_remote_)
        return;
    if (auto transport = acquire_transport_())
    {
        idr_requested_ = true;
        enqueue_remote_([transport, cmd]()
                        {
            if (!transport->Send(cmd, false))
            {
                std::fprintf(stderr, "[AMLgsMenu] Failed to request IDR frame\n");
            } });
    }
}

void LinkSettingsController::ApplyRecording(bool enable)
{
    const char *payload = enable ? "record=1" : "record=0";
    if (!SendUdpControlCommand(5612, payload, "record"))
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to send recording command (%s)\n",
                     enable ? "record=1" : "record=0");
    }
}

void LinkSettingsController::ApplySkyPower()
{
    const auto &powers = state_.PowerLevels();
    if (powers.empty())
        return;
    int p = powers[state_.SkyPowerIndex()];
    if (acquire_transport_ && enqueue_remote_)
    {
        if (auto transport = acquire_transport_())
        {
            int tx_pwr = p * 50;
            std::unordered_map<std::string, std::string> vars{
                {"POWER", std::to_string(p)},
                {"TXPOWER", std::to_string(tx_pwr)}};
            auto cmd = templates_.Render("remote", "sky_power", vars);
            if (!cmd.empty())
            {
                enqueue_remote_([transport, cmd]()
                                {
                    if (!transport->Send(cmd, false))
                    {
                        std::fprintf(stderr, "[AMLgsMenu] Failed to send tx power command\n");
                    } });
            }
        }
    }
}

void LinkSettingsController::ApplyGroundPower()
{
    const auto &powers = state_.PowerLevels();
    if (powers.empty())
        return;
    int p = powers[state_.GroundPowerIndex()];
    ApplyLocalMonitorPower(p);
}

void LinkSettingsController::ApplySoundEnabled()
{
    bool enabled = state_.SoundEnabled();
    if (!SendSoundCommand(enabled))
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to send sound command (%s)\n",
                     enabled ? "sound=1" : "sound=0");
    }
}

void LinkSettingsController::ApplySoundVolume()
{
    const auto &volumes = state_.VolumeLevels();
    if (volumes.empty())
        return;
    int idx = state_.SoundVolumeIndex();
    if (idx < 0 || idx >= static_cast<int>(volumes.size()))
        idx = static_cast<int>(volumes.size()) - 1;
    int volume = volumes[idx];
    std::string cmd = "pactl set-sink-volume @DEFAULT_SINK@ " + std::to_string(volume) + "% >/dev/null 2>&1";
    std::system(cmd.c_str());
}

void LinkSettingsController::ApplyBadFramePolicy()
{
    const char *key = (state_.BadFrameIndex() == 0) ? "bad_frame_drop" : "bad_frame_ignore";
    const std::string cmd = templates_.Render("local", key, {});
    if (!cmd.empty())
    {
        if (enqueue_shell_)
        {
            enqueue_shell_(cmd);
        }
        else
        {
            std::system(cmd.c_str());
        }
        return;
    }

    int value = (state_.BadFrameIndex() == 0) ? 0 : 432;
    std::ofstream ofs("/sys/module/amvdec_h265/parameters/error_handle_policy");
    if (!ofs.is_open())
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to open error_handle_policy\n");
        return;
    }
    ofs << value << std::endl;
    if (!ofs.good())
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to set error_handle_policy=%d\n", value);
    }
}

void LinkSettingsController::ApplyAdaptiveLink()
{
    if (state_.AdaptiveLinkEnabled())
    {
        std::system("systemctl enable alink && systemctl start alink");
    }
    else
    {
        std::system("systemctl disable alink");
    }
}

void LinkSettingsController::ApplyNetworkMode()
{
    const bool connect_ap = state_.NetworkModeIndex() == static_cast<int>(MenuState::NetworkMode::ConnectAp);
    // 无论是切换到热点还是切回 WFB，都需要重新加载 connman
    const std::string cmd = "systemctl restart connman && systemctl restart wifibroadcast";
    if (enqueue_shell_)
    {
        enqueue_shell_(cmd);
    }
    else
    {
        std::system(cmd.c_str());
    }
}

void LinkSettingsController::ApplyLocalMonitorChannel(int channel)
{
    if (channel <= 0)
        return;
    int bw_mhz = (state_.BandwidthIndex() == 0) ? 10 : (state_.BandwidthIndex() == 1 ? 20 : 40);
    std::string bw_suffix;
    if (bw_mhz == 20)
        bw_suffix = " HT20";
    else if (bw_mhz == 40)
        bw_suffix = " HT40+";
    std::unordered_map<std::string, std::string> vars{
        {"CHANNEL", std::to_string(channel)},
        {"BW_SUFFIX", bw_suffix}};
    auto cmd = templates_.Render("local", "monitor_channel", vars);
    if (!cmd.empty() && enqueue_shell_)
    {
        enqueue_shell_(cmd);
    }
}

void LinkSettingsController::ApplyLocalMonitorPower(int power_level)
{
    if (power_level <= 0)
        return;
    int tx_pwr = power_level * 50;
    std::unordered_map<std::string, std::string> vars{
        {"POWER", std::to_string(power_level)},
        {"TXPOWER", std::to_string(tx_pwr)}};
    auto cmd = templates_.Render("local", "monitor_power", vars);
    if (!cmd.empty() && enqueue_shell_)
    {
        enqueue_shell_(cmd);
    }
}

bool LinkSettingsController::SendSoundCommand(bool enable)
{
    const char *payload = enable ? "sound=1" : "sound=0";
    return SendUdpControlCommand(5612, payload, "sound");
}

bool LinkSettingsController::SendUdpControlCommand(uint16_t port, const char *payload, const char *tag)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        std::fprintf(stderr, "[AMLgsMenu] socket(%s) failed: %s\n", tag, std::strerror(errno));
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ssize_t sent = sendto(sock, payload, std::strlen(payload), 0,
                          reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    if (sent < 0)
    {
        std::fprintf(stderr, "[AMLgsMenu] sendto(%s) failed: %s\n", tag, std::strerror(errno));
        close(sock);
        return false;
    }
    close(sock);
    return true;
}

int LinkSettingsController::FindMcsIndex(const MenuState &state, int mcs_val)
{
    const auto &mcs = state.McsLevels();
    for (size_t i = 0; i < mcs.size(); ++i)
    {
        if (mcs[i] == mcs_val)
            return static_cast<int>(i);
    }
    return -1;
}
