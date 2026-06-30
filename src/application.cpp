#include "application.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "display_platform.h"
#include "input_controller.h"
#include "video_mode.h"
#include "mavlink_receiver.h"
#include "udp_command_client.h"
#include "ssh_command_client.h"
#include "ap_tcp_client.h"
#include "command_templates.h"
#include "terminal.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <filesystem>

namespace
{
std::string ShellEscape(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char c : value)
    {
        if (c == '\'')
        {
            out.append("'\"'\"'");
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}
}

Application::Application() = default;
Application::~Application() { Shutdown(); }

std::string Application::DetectApGateway()
{
    std::ifstream route("/proc/net/route");
    std::string line;
    while (std::getline(route, line))
    {
        std::istringstream iss(line);
        std::string iface, dest, gw;
        if (!(iss >> iface >> dest >> gw))
            continue;
        if (dest != "00000000")
            continue;
        if (gw == "00000000")
            continue;
        unsigned int gw_int = 0;
        if (std::sscanf(gw.c_str(), "%x", &gw_int) != 1)
            continue;
        struct in_addr addr;
        addr.s_addr = gw_int;
        char buf[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &addr, buf, sizeof(buf)))
            return std::string(buf);
    }
    return {};
}

namespace
{
    constexpr float kOsdRefreshHz = 30.0f;
    std::string TrimCopy(const std::string &text)
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

    float BearingDegrees(double lat1, double lon1, double lat2, double lon2)
    {
        const double rad = M_PI / 180.0;
        double phi1 = lat1 * rad;
        double phi2 = lat2 * rad;
        double dlon = (lon2 - lon1) * rad;
        double y = std::sin(dlon) * std::cos(phi2);
        double x = std::cos(phi1) * std::sin(phi2) - std::sin(phi1) * std::cos(phi2) * std::cos(dlon);
        double brng = std::atan2(y, x) * 180.0 / M_PI;
        if (brng < 0.0)
            brng += 360.0;
        return static_cast<float>(brng);
    }

    MenuRenderer::TelemetryData ConvertTelemetry(const ParsedTelemetry &src, const MenuState &state)
    {
        MenuRenderer::TelemetryData out{};
        // Signals: map RC RSSI (0-255) to a rough dBm-ish scale
        if (src.has_radio_rssi)
        {
            float rc_dbm = -100.0f + static_cast<float>(src.rc_rssi) * 0.4f;
            out.rc_signal = rc_dbm;
            out.ground_signal_a = rc_dbm;
            out.ground_signal_b = rc_dbm;
            out.has_rc_signal = true;
        }
        else
        {
            out.has_rc_signal = false;
        }

        if (src.has_flight_mode && !src.flight_mode.empty())
        {
            out.flight_mode = src.flight_mode;
            out.has_flight_mode = true;
        }
        else
        {
            out.has_flight_mode = false;
        }
        out.has_attitude = src.has_attitude;
        out.roll_deg = src.roll_deg;
        out.pitch_deg = src.pitch_deg;

        out.has_gps = src.has_gps;
        if (src.has_gps)
        {
            out.latitude = src.latitude;
            out.longitude = src.longitude;
            out.altitude_m = src.altitude_m;
            out.home_distance_m = src.home_distance_m;
            out.gps_satellites = src.gps_satellites;
        }
        out.has_speed = src.has_speed;
        if (src.has_speed)
        {
            out.ground_speed_mps = src.ground_speed_mps;
            out.air_speed_mps = src.air_speed_mps;
        }

        out.has_home_bearing = false;
        if (src.has_gps && src.has_home && src.has_attitude)
        {
            float bearing = BearingDegrees(src.latitude, src.longitude, src.home_latitude, src.home_longitude);
            float rel = bearing - src.yaw_deg;
            while (rel <= -180.0f)
                rel += 360.0f;
            while (rel > 180.0f)
                rel -= 360.0f;
            out.has_home_bearing = true;
            out.home_bearing_rel_deg = rel;
        }
        out.has_rc = src.has_rc;
        if (src.has_rc)
        {
            out.rc_left_x = src.rc_left_x;
            out.rc_left_y = src.rc_left_y;
            out.rc_right_x = src.rc_right_x;
            out.rc_right_y = src.rc_right_y;
        }

        out.has_battery = src.has_battery;
        if (src.has_battery)
        {
            out.pack_voltage = src.batt_voltage_v;
            out.cell_voltage_estimated = false;
            if (src.batt_voltage_v > 0.1f)
            {
                int est_cells = static_cast<int>(std::floor(src.batt_voltage_v / 4.3f)) + 1;
                if (est_cells < 1)
                    est_cells = 1;
                if (est_cells > 12)
                    est_cells = 12;
                out.cell_voltage = src.batt_voltage_v / static_cast<float>(est_cells);
                out.cell_voltage_estimated = true;
            }
            else if (src.cell_count > 0 && src.cell_voltage_v > 0.01f)
            {
                out.cell_voltage = src.cell_voltage_v;
            }
            else if (src.cell_count > 0 && src.batt_voltage_v > 0.1f)
            {
                out.cell_voltage = src.batt_voltage_v / static_cast<float>(src.cell_count);
            }
            else
            {
                // Fallback: assume 4S only as last resort
                out.cell_voltage = src.batt_voltage_v > 0.1f ? src.batt_voltage_v / 4.0f : 0.0f;
            }
        }

        out.has_sky_temp = src.has_sky_temp;
        out.sky_temp_c = src.sky_temp_c;
        out.ground_temp_c = 0.0f;

        if (src.has_video_metrics)
        {
            out.bitrate_mbps = src.video_bitrate_mbps;
            out.video_resolution = src.video_resolution;
            out.video_refresh_hz = src.video_refresh_hz;
        }
        else
        {
            const auto &ground_modes = state.GroundModes();
            VideoMode mode = ground_modes.empty() ? VideoMode{"1920x1080 @ 60Hz", 1920, 1080, 60}
                                                  : ground_modes[state.GroundModeIndex() % ground_modes.size()];
            std::ostringstream res;
            res << mode.width << "x" << mode.height;
            out.video_resolution = res.str();
            out.video_refresh_hz = mode.refresh ? mode.refresh : 60;
            out.bitrate_mbps = 0.0f;
        }

        return out;
    }
} // namespace

bool Application::Initialize(const std::string &font_path, bool use_mock,
                             const std::string &terminal_font_path)
{
    use_mock_ = use_mock;
    if (!display_platform_.Initialize())
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to init display platform\n");
        return false;
    }
    input_controller_ = std::make_unique<InputController>();
    if (!input_controller_->Initialize())
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to init libinput/udev\n");
        return false;
    }
    input_controller_->ScanJoysticks();
    command_templates_.LoadFromFile(command_cfg_path_);
    cmd_runner_ = std::make_unique<CommandExecutor>();
    cmd_runner_->Start();
    command_runner_active_ = true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    io.MouseDrawCursor = false;

    const float base_size = 26.0f; // larger base size for readability
    const bool is_zc0005 = (!font_path.empty() &&
                            font_path.find("ZC0005-Regular-2.ttf") != std::string::npos);
    const float ui_size = is_zc0005 ? base_size : (base_size * 1.2f);
    if (!font_path.empty())
    {
        ui_font_ = io.Fonts->AddFontFromFileTTF(font_path.c_str(), ui_size);
    }
    else
    {
        ui_font_ = io.Fonts->AddFontDefault();
    }
    if (!ui_font_)
    {
        ui_font_ = io.Fonts->Fonts.empty() ? io.Fonts->AddFontDefault() : io.Fonts->Fonts[0];
    }
    if (!terminal_font_path.empty())
    {
        terminal_font_ = io.Fonts->AddFontFromFileTTF(terminal_font_path.c_str(), base_size);
    }
    else if (!font_path.empty())
    {
        terminal_font_ = io.Fonts->AddFontFromFileTTF(font_path.c_str(), base_size);
    }
    if (!terminal_font_)
    {
        terminal_font_ = ui_font_;
    }

    ImGui_ImplOpenGL3_Init("#version 100");

    terminal_ = std::make_unique<Terminal>();
    terminal_->setEmbedded(true);
    signal_monitor_ = std::make_unique<SignalMonitor>();
    telemetry_worker_ = std::make_unique<TelemetryWorker>(signal_monitor_.get());
    telemetry_worker_->Start();
    terminal_->setFont(terminal_font_);

    auto sky_modes = DefaultSkyModes();
    auto ground_modes = LoadHdmiModes("/sys/class/amhdmitx/amhdmitx0/disp_cap");
    if (ground_modes.empty())
    {
        std::cout << "[AMLgsMenu] failed to load ground HDMI modes, using 1080p60hz as fallback" << std::endl;
        ground_modes = {VideoMode{"1080p60hz", 1920, 1080, 60}};
    }
    if (!std::any_of(ground_modes.begin(), ground_modes.end(),
                     [](const VideoMode &m)
                     { return m.refresh == 120; }))
    {
        std::cout << "[AMLgsMenu] add 120hz display mode fallback" << std::endl;
        ground_modes.push_back(VideoMode{"1080p120hz", 1920, 1080, 120});
        ground_modes.push_back(VideoMode{"720p120hz", 1280, 720, 120});
    }
    menu_state_ = std::make_unique<MenuState>(sky_modes, ground_modes);
    if (!EnsureWritableConfig())
    {
        std::fprintf(stderr, "[AMLgsMenu] Warning: failed to prepare config file '%s'\n", config_.ConfigPath().c_str());
    }
    LoadConfig();
    ap_mode_ = (menu_state_->NetworkModeIndex() == static_cast<int>(MenuState::NetworkMode::ConnectAp));
    RebuildTransport(menu_state_->GetFirmwareType(), ap_mode_);
    settings_controller_ = std::make_unique<LinkSettingsController>(*menu_state_, command_templates_);
    settings_controller_->SetHooks(
        [this]()
        { return AcquireTransport(); },
        [this](std::function<void()> job)
        {
            if (cmd_runner_)
            {
                cmd_runner_->EnqueueRemote(std::move(job));
            }
        },
        [this](const std::string &cmd)
        {
            if (cmd_runner_)
            {
                cmd_runner_->EnqueueShell(cmd);
            }
            else
            {
                std::system(cmd.c_str());
            }
        });
    settings_controller_->SetApMode(ap_mode_);
    remote_sync_ = std::make_unique<RemoteSyncService>(*menu_state_, command_templates_, config_);
    remote_sync_->SetHooks(
        [this]()
        { return AcquireTransport(); },
        [this]()
        { return EnsureCommandRunnerForRemoteSync(); },
        [this](std::function<void()> job)
        {
            if (cmd_runner_)
            {
                cmd_runner_->EnqueueRemote(std::move(job));
            }
        });
    remote_sync_->SetApMode(ap_mode_);
    StartRemoteSync();
    ApplyLanguageToImGui(menu_state_->GetLanguage());
    if (!use_mock_)
    {
        mav_receiver_ = std::make_unique<MavlinkReceiver>(mavlink_port_);
        mav_receiver_->Start();
    }

    std::function<MenuRenderer::TelemetryData(MenuRenderer::TelemetryData)> provider;
    if (!use_mock_ && mav_receiver_)
    {
        provider = [this](MenuRenderer::TelemetryData last_data)
        {
            auto data = ConvertTelemetry(mav_receiver_->Latest(), *menu_state_);
            if (telemetry_worker_)
            {
                auto snap = telemetry_worker_->Latest();
                if (snap.ground_signal.valid)
                {
                    if (last_data.ground_signal_a == 0.0f)
                    {
                        std::fprintf(stdout, "[AMLgsMenu] First ground signal received,refresh sky signal values\n");
                        RestartRemoteSync();
                    }
                    data.ground_signal_a = snap.ground_signal.signal_a;
                    data.ground_signal_b = snap.ground_signal.signal_b;
                }
                if (snap.packet_rate.valid && snap.packet_rate.primary_mbps > 0.0f)
                {
                    data.bitrate_mbps = snap.packet_rate.primary_mbps;
                    if (settings_controller_)
                    {
                        settings_controller_->RequestIdrFrame();
                    }
                }
                if (snap.has_ground_temp)
                {
                    data.ground_temp_c = snap.ground_temp_c;
                }
                if (snap.output_fps > 0)
                {
                    data.video_refresh_hz = snap.output_fps;
                }
                if (snap.has_hid_batt)
                {
                    data.has_ground_batt = true;
                    data.ground_batt_percent = snap.hid_batt_percent;
                }
                if (snap.has_wifi_monitor)
                {
                    data.has_wifi_monitor = true;
                    data.wifi_monitor_count = snap.wifi_monitor_count;
                }
            }
            else
            {
                data.ground_temp_c = ReadTemperatureC();
            }
            return data;
        };
    }
    else if (telemetry_worker_)
    {
        provider = nullptr;
    }

    renderer_ = std::make_unique<MenuRenderer>(
        *menu_state_, use_mock_, provider,
        [this]()
        {
            if (terminal_)
                terminal_->toggleVisibility();
        },
        [this]()
        { return terminal_ && terminal_->isTerminalVisible(); },
        [this](const std::string &path)
        { StartFirmwareUpdate(path); },
        [this]()
        { return UpdateStatus(); },
        [this]()
        { RequestReboot(); },
        [this](const std::string &ssid, const std::string &password)
        { return UpdateWifiClientConfig(ssid, password); },
        config_.WifiClientSsid(), config_.WifiClientPassword());
    splash_player_.Init();
    input_controller_->SetCallbacks({
        [this]()
        {
            return menu_state_ && menu_state_->MenuVisible();
        },
        [this]()
        {
            if (menu_state_)
                menu_state_->ToggleMenuVisibility();
        },
        [this](bool visible)
        {
            if (menu_state_)
                menu_state_->SetMenuVisible(visible);
        },
        [this](bool visible)
        {
            UpdateCommandRunner(visible);
        },
        [this]()
        {
            return terminal_ && terminal_->isTerminalVisible();
        },
        [this](char c)
        {
            if (terminal_)
                terminal_->SendControlChar(c);
        },
        [this](int sig)
        {
            if (terminal_)
                terminal_->SendSignal(sig);
        },
    });

    menu_state_->SetOnChangeCallback([this](MenuState::SettingType type)
                                     {
        switch (type) {
        case MenuState::SettingType::Channel:
            SaveConfigValue("channel", std::to_string(menu_state_->Channels()[menu_state_->ChannelIndex()]));
            if (settings_controller_) settings_controller_->ApplyChannel();
            break;
        case MenuState::SettingType::Bandwidth:
            SaveConfigValue("bandwidth", std::to_string((menu_state_->BandwidthIndex() == 0) ? 10 :
                                                        (menu_state_->BandwidthIndex() == 1) ? 20 : 40));
            if (settings_controller_) settings_controller_->ApplyBandwidth();
            break;
        case MenuState::SettingType::NetworkMode:
            {
                ap_mode_ = (menu_state_->NetworkModeIndex() == static_cast<int>(MenuState::NetworkMode::ConnectAp));
                SaveConfigValue("ap_mode", ap_mode_ ? "1" : "0");
                RebuildTransport(menu_state_->GetFirmwareType(), ap_mode_);
                if (settings_controller_)
                {
                    settings_controller_->SetApMode(ap_mode_);
                    settings_controller_->ApplyNetworkMode();
                }
                if (remote_sync_)
                {
                    remote_sync_->SetApMode(ap_mode_);
                    RestartRemoteSync();
                }
            }
            break;
        case MenuState::SettingType::GroundMode: {
            // update ground_res and clean legacy typo
            config_.MutableValues().erase("groud_res");
            const auto &modes = menu_state_->GroundModes();
            if (modes.empty())
                break;
            const auto &mode = modes[menu_state_->GroundModeIndex()];
            bool skip_once = menu_state_->ConsumeGroundModeSkipSaveOnce();
            bool force_save_once = menu_state_->ConsumeGroundModeForceSaveOnce();
            const bool should_save = !skip_once || force_save_once;
            if (should_save)
            {
                SaveConfigValue("ground_res", mode.label);
                if (mode.refresh > 60)
                {
                    menu_state_->SetGroundModePersisted(mode.label, true);
                }
            }
            if (settings_controller_) settings_controller_->ApplyGroundDisplayMode(mode.label);
            break;
        }
        case MenuState::SettingType::SkyMode:
            if (settings_controller_) settings_controller_->ApplySkyMode();
            break;
        case MenuState::SettingType::GroundPower:
            SaveConfigValue("driver_txpower_override", std::to_string(menu_state_->PowerLevels()[menu_state_->GroundPowerIndex()]));
            if (settings_controller_) settings_controller_->ApplyGroundPower();
            break;
        case MenuState::SettingType::Bitrate:
            if (settings_controller_) settings_controller_->ApplyBitrate();
            break;
        case MenuState::SettingType::SkyMcs:
            if (settings_controller_) settings_controller_->ApplySkyMcs();
            break;
        case MenuState::SettingType::SkyPower:
            if (settings_controller_) settings_controller_->ApplySkyPower();
            break;
        case MenuState::SettingType::SoundEnable:
            SaveConfigValue("sound", menu_state_->SoundEnabled() ? "1" : "0");
            if (settings_controller_) settings_controller_->ApplySoundEnabled();
            break;
        case MenuState::SettingType::SoundVolume: {
            const auto &volumes = menu_state_->VolumeLevels();
            if (!volumes.empty())
            {
                int idx = menu_state_->SoundVolumeIndex();
                if (idx < 0 || idx >= static_cast<int>(volumes.size()))
                    idx = std::max(0, std::min(static_cast<int>(volumes.size()) - 1, idx));
                SaveConfigValue("sound_volume", std::to_string(volumes[idx]));
                if (settings_controller_) settings_controller_->ApplySoundVolume();
            }
            break;
        }
        case MenuState::SettingType::BufferLevel: {
            const auto &levels = menu_state_->BufferLevels();
            if (!levels.empty())
            {
                int idx = menu_state_->BufferLevelIndex();
                if (idx < 0 || idx >= static_cast<int>(levels.size()))
                    idx = 0;
                SaveConfigValue("buf_level", std::to_string(levels[idx]));
                if (cmd_runner_)
                {
                    cmd_runner_->EnqueueShell("systemctl restart amldigitalfpv");
                }
                else
                {
                    std::system("systemctl restart amldigitalfpv");
                }
            }
            break;
        }
        case MenuState::SettingType::HdmiAttr: {
            const auto &attrs = menu_state_->HdmiAttrs();
            if (!attrs.empty())
            {
                int idx = menu_state_->HdmiAttrIndex();
                if (idx < 0 || idx >= static_cast<int>(attrs.size()))
                    idx = 0;
                const std::string value = attrs[idx];
                bool skip_once = menu_state_->ConsumeHdmiAttrSkipSaveOnce();
                bool force_save_once = menu_state_->ConsumeHdmiAttrForceSaveOnce();
                const bool should_save = !skip_once || force_save_once;
                if (should_save)
                {
                    SaveConfigValue("hdmi_attr", value);
                }
                const std::string cmd = "echo " + value + " > /sys/class/amhdmitx/amhdmitx0/attr";
                if (cmd_runner_)
                {
                    cmd_runner_->EnqueueShell(cmd);
                }
                else
                {
                    std::system(cmd.c_str());
                }
            }
            break;
        }
        case MenuState::SettingType::BadFramePolicy: {
            int value = (menu_state_->BadFrameIndex() == 0) ? 0 : 432;
            SaveConfigValue("bad_frame", std::to_string(value));
            if (settings_controller_) settings_controller_->ApplyBadFramePolicy();
            break;
        }
        case MenuState::SettingType::AdaptiveLink: {
            SaveConfigValue("alink", menu_state_->AdaptiveLinkEnabled() ? "1" : "0");
            if (settings_controller_) settings_controller_->ApplyAdaptiveLink();
            break;
        }
        case MenuState::SettingType::Language: {
            auto lang = menu_state_->GetLanguage();
            SaveConfigValue("lang", lang == MenuState::Language::CN ? "cn" : "en");
            ApplyLanguageToImGui(lang);
            break;
        }
        case MenuState::SettingType::Recording: {
            bool recording = menu_state_->Recording();
            if (settings_controller_) settings_controller_->ApplyRecording(recording);
            break;
        }
        case MenuState::SettingType::Firmware: {
            auto mode = menu_state_->GetFirmwareType();
            SaveConfigValue("firmware", mode == MenuState::FirmwareType::CCEdition ? "cc" : "official");
            RebuildTransport(mode, ap_mode_);
            RestartRemoteSync();
            break;
        }
        default:
            break;
        } });
    // menu_state_->SetVisibilityChangeCallback([this](bool visible)
    //                                          { if(!visible)this->SaveConfig(); });
    running_ = true;
    initialized_ = true;
    last_frame_time_ = std::chrono::steady_clock::now();
    return true;
}

void Application::Run()
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(display_platform_.width()),
                           static_cast<float>(display_platform_.height()));
    io.MousePos = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

    uint64_t frame_counter = 0;
    auto last_log = std::chrono::steady_clock::now();
    auto frame_start = std::chrono::steady_clock::now();

    while (running_ && !menu_state_->ShouldExit())
    {
        const auto loop_begin = frame_start;
        if (input_controller_)
            input_controller_->Process(running_, display_platform_.width(), display_platform_.height());
        if (remote_sync_)
        {
            remote_sync_->DrainPending();
        }
        UpdateDeltaTime();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        renderer_->Render(running_);
        if (terminal_)
        {
            terminal_->render();
        }
        splash_player_.RenderOverlay();

        ImGui::Render();
        glViewport(0, 0, display_platform_.width(), display_platform_.height());
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglWaitGL();
        auto before_swap = std::chrono::steady_clock::now();
        if (!eglSwapBuffers(display_platform_.display(), display_platform_.surface()))
        {
            EGLint egl_err = eglGetError();
            std::fprintf(stderr, "[AMLgsMenu] eglSwapBuffers failed (err=0x%04x), stopping loop\n",
                         static_cast<unsigned int>(egl_err));
            EGLint width = 0, height = 0;
            if (display_platform_.surface() != EGL_NO_SURFACE &&
                eglQuerySurface(display_platform_.display(), display_platform_.surface(), EGL_WIDTH, &width) &&
                eglQuerySurface(display_platform_.display(), display_platform_.surface(), EGL_HEIGHT, &height))
            {
                GLint current_tex = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &current_tex);
                std::fprintf(stderr, "[AMLgsMenu] Surface query ok (%dx%d), bound texture id=%d\n",
                             width, height, current_tex);
            }
            else
            {
                std::fprintf(stderr, "[AMLgsMenu] Surface query failed, EGL surface may be invalid\n");
            }
            running_ = false;
        }

        ++frame_counter;
        auto frame_end = std::chrono::steady_clock::now();
        auto ms_since_log = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - last_log).count();
        if (ms_since_log >= 30000)
        { // log every 30s to reduce noise
            auto ms_swap = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - before_swap).count();
            std::fprintf(stdout, "[AMLgsMenu] Frame %llu swap done (swap ms=%lld)\n",
                         static_cast<unsigned long long>(frame_counter),
                         static_cast<long long>(ms_swap));
            std::fflush(stdout);
            last_log = frame_end;
        }

        float target_period = 1.0f / kOsdRefreshHz;
        auto frame_elapsed = std::chrono::duration<float>(frame_end - loop_begin).count();
        if (frame_elapsed < target_period)
        {
            float remaining = target_period - frame_elapsed;
            const float slice = 0.003f; // 3ms slices to keep input responsive
            while (remaining > 0.0f)
            {
                float step = std::min(remaining, slice);
                std::this_thread::sleep_for(std::chrono::duration<float>(step));
                remaining -= step;
                if (input_controller_)
                    input_controller_->Process(running_, display_platform_.width(), display_platform_.height());
                if (!running_ || menu_state_->ShouldExit())
                    break;
            }
            frame_end = std::chrono::steady_clock::now();
        }
        frame_start = frame_end;
    }
}

void Application::UpdateCommandRunner(bool menu_visible)
{
    (void)menu_visible;
    if (!cmd_runner_)
    {
        return;
    }
    if (!command_runner_active_)
    {
        cmd_runner_->Start();
        command_runner_active_ = true;
    }
}

std::shared_ptr<CommandTransport> Application::AcquireTransport() const
{
    std::lock_guard<std::mutex> lock(transport_mutex_);
    return transport_;
}

void Application::RebuildTransport(MenuState::FirmwareType type, bool ap_mode)
{
    std::shared_ptr<CommandTransport> new_transport;
    if (ap_mode)
    {
        std::string gw = ap_gateway_.empty() ? DetectApGateway() : ap_gateway_;
        if (gw.empty())
        {
            std::fprintf(stderr, "[AMLgsMenu] AP mode: no gateway detected, remote commands will fail\n");
        }
        else
        {
            ap_gateway_ = gw;
            std::fprintf(stdout, "[AMLgsMenu] AP mode: using air_man_ap at %s:12355\n", gw.c_str());
            new_transport = std::make_shared<ApTcpClient>(gw);
        }
    }
    else if (type == MenuState::FirmwareType::Official)
    {
        new_transport = std::make_shared<SshCommandClient>(ssh_host_, ssh_port_, ssh_user_, ssh_password_);
    }
    else
    {
        new_transport = std::make_shared<UdpCommandClient>();
    }
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport_ = std::move(new_transport);
        firmware_mode_ = type;
    }
    ap_mode_ = ap_mode;
}

void Application::RestartRemoteSync()
{
    StartRemoteSync();
}

void Application::Shutdown()
{
    if (!initialized_)
    {
        return;
    }

    initialized_ = false;
    running_ = false;
    if (input_controller_)
    {
        input_controller_->Shutdown();
        input_controller_.reset();
    }

    if (mav_receiver_)
    {
        mav_receiver_->Stop();
        mav_receiver_.reset();
    }
    splash_player_.Shutdown();
    if (cmd_runner_)
    {
        cmd_runner_->Stop();
        command_runner_active_ = false;
        cmd_runner_.reset();
    }
    if (terminal_)
    {
        terminal_.reset();
    }
    if (telemetry_worker_)
    {
        telemetry_worker_->Stop();
        telemetry_worker_.reset();
    }
    if (signal_monitor_)
    {
        signal_monitor_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport_.reset();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    display_platform_.Shutdown();
}

bool Application::UpdateWifiClientConfig(const std::string &ssid, const std::string &password)
{
    // 更新 Wi‑Fi 客户端（AP）配置后立即让 ConnMan 重新加载
    // 通过调用 LinkSettingsController::ApplyNetworkMode 触发
    // "systemctl restart connman && systemctl restart wifibroadcast"
    bool ok = config_.UpdateWifiClientConfig(ssid, password);
    if (ok && settings_controller_)
    {
        settings_controller_->ApplyNetworkMode(); // 重启 connman 使新 AP 配置生效
    }
    return ok;
}

void Application::StartFirmwareUpdate(const std::string &path)
{
    if (path.empty() || !cmd_runner_)
    {
        update_status_.store(3);
        return;
    }
    if (update_status_.load() == 1)
    {
        return;
    }
    update_status_.store(1);
    std::filesystem::path src(path);
    std::filesystem::path parent = src.parent_path();
    std::string parent_str = parent.string();
    std::string remount_media;
    if (!parent_str.empty() && parent_str.rfind("/media/", 0) == 0)
    {
        remount_media = " && mount -o remount,rw " + ShellEscape(parent_str);
    }
    std::string cmd =
        "mount -o remount,rw /flash" + remount_media +
        " && (bak=/tmp/wfb.conf.bak; "
        "if [ -f /storage/digitalfpv/wfb.conf ]; then cp /storage/digitalfpv/wfb.conf $bak; "
        "elif [ -f /flash/wfb.conf ]; then cp /flash/wfb.conf $bak; fi)" +
        " && mv " + ShellEscape(path) + " /tmp/update_fpv.tar.gz" +
        " && sleep 5" +
        " && tar -zxv -f /tmp/update_fpv.tar.gz -C /" +
        " && (cand=\"\"; "
        "if [ -f /storage/digitalfpv/wfb.conf ]; then cand=/storage/digitalfpv/wfb.conf; "
        "elif [ -f /wfb.conf ]; then cand=/wfb.conf; fi; "
        "if [ -n \"$cand\" ]; then "
        "mkdir -p /storage/digitalfpv; "
        "if [ -f /tmp/wfb.conf.bak ]; then "
        "awk -F= 'function trim(s){sub(/^[ \\t]+/,\"\",s); sub(/[ \\t]+$/,\"\",s); return s} "
        "FNR==NR{if ($0 ~ /^[ \\t]*#/ || $0 ~ /^[ \\t]*$/) next; "
        "if (NF>=2){key=trim($1); user[key]=$0; order[++n]=key;} next} "
        "{if ($0 ~ /^[ \\t]*#/ || $0 ~ /^[ \\t]*$/){print; next} "
        "if (NF>=2){key=trim($1); if (key in user){print user[key]; used[key]=1} else {print $0}; next} print} "
        "END{for (i=1;i<=n;i++){key=order[i]; if (!(key in used)) print user[key]}}' "
        "/tmp/wfb.conf.bak \"$cand\" > /tmp/wfb.conf.merged && "
        "mv /tmp/wfb.conf.merged /storage/digitalfpv/wfb.conf; "
        "else cp \"$cand\" /storage/digitalfpv/wfb.conf; fi; "
        "if [ \"$cand\" = \"/wfb.conf\" ]; then rm -f /wfb.conf; fi; "
        "fi)";
    cmd_runner_->EnqueueRemote([this, cmd]()
                               {
        int rc = std::system(cmd.c_str());
        update_status_.store(rc == 0 ? 2 : 3); });
}

void Application::RequestReboot()
{
    if (cmd_runner_)
    {
        cmd_runner_->EnqueueShell("reboot");
    }
    else
    {
        std::system("reboot");
    }
}

void Application::UpdateDeltaTime()
{
    ImGuiIO &io = ImGui::GetIO();
    auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::chrono::duration<float>(now - last_frame_time_).count();
    last_frame_time_ = now;
}

void Application::LoadConfig()
{
    config_.LoadInto(*menu_state_, ssh_password_);
}

bool Application::EnsureWritableConfig()
{
    return config_.EnsureWritable();
}

void Application::StartRemoteSync()
{
    if (remote_sync_)
    {
        remote_sync_->RequestSync();
    }
}

bool Application::EnsureCommandRunnerForRemoteSync()
{
    if (!cmd_runner_)
    {
        return false;
    }
    if (!command_runner_active_)
    {
        cmd_runner_->Start();
        command_runner_active_ = true;
    }
    return true;
}

void Application::SaveConfigValue(const std::string &key, const std::string &value)
{
    config_.SaveValue(key, value);
}

void Application::SaveConfig()
{
    config_.SaveAll();
}

void Application::ApplyLanguageToImGui(MenuState::Language lang)
{
    // Font loading already follows -t 参数或默认字体，不在语言切换时重建，避免丢失用户指定字体。
    (void)lang;
}
