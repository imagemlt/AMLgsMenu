#include "application.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "video_mode.h"
#include "mavlink_receiver.h"
#include "udp_command_client.h"
#include "ssh_command_client.h"
#include "command_templates.h"
#include "terminal.h"

#include <EGL/egl.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <glob.h>
#include <linux/fb.h>
#include <linux/input-event-codes.h>
#include <linux/joystick.h>
#include <poll.h>
#include <signal.h>
#include <cerrno>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>

Application::Application() = default;
Application::~Application() { Shutdown(); }

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
        }

        out.has_battery = src.has_battery;
        if (src.has_battery)
        {
            out.pack_voltage = src.batt_voltage_v;
            if (src.cell_count > 0 && src.cell_voltage_v > 0.01f)
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
    if (!InitFramebuffer(fb_))
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to init framebuffer (/dev/fb0)\n");
        return false;
    }
    if (!InitEgl(fb_))
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to init EGL/GLES\n");
        return false;
    }
    if (!InitInput())
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to init libinput/udev\n");
        return false;
    }
    ScanJoysticks();
    last_js_scan_ = std::chrono::steady_clock::now();
    command_templates_.LoadFromFile(command_cfg_path_);
    cmd_runner_ = std::make_unique<CommandExecutor>();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    io.MouseDrawCursor = false;

    const float base_size = 26.0f; // larger base size for readability
    if (!font_path.empty())
    {
        ui_font_ = io.Fonts->AddFontFromFileTTF(font_path.c_str(), base_size);
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
        ground_modes = sky_modes;
    }

    menu_state_ = std::make_unique<MenuState>(sky_modes, ground_modes);
    LoadConfig();
    RebuildTransport(menu_state_->GetFirmwareType());
    StartRemoteSync();
    ApplyLanguageToImGui(menu_state_->GetLanguage());
    if (!use_mock_)
    {
        mav_receiver_ = std::make_unique<MavlinkReceiver>();
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
                    // got signal,refresh sky
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

    renderer_ = std::make_unique<MenuRenderer>(*menu_state_, use_mock_, provider, [this]()
                                               {
            if (terminal_)
                terminal_->toggleVisibility(); }, [this]()
                                               { return terminal_ && terminal_->isTerminalVisible(); });

    menu_state_->SetOnChangeCallback([this](MenuState::SettingType type)
                                     {
        switch (type) {
        case MenuState::SettingType::Channel:
            SaveConfigValue("channel", std::to_string(menu_state_->Channels()[menu_state_->ChannelIndex()]));
            ApplyChannel();
            break;
        case MenuState::SettingType::Bandwidth:
            SaveConfigValue("bandwidth", std::to_string((menu_state_->BandwidthIndex() == 0) ? 10 :
                                                        (menu_state_->BandwidthIndex() == 1) ? 20 : 40));
            ApplyBandwidth();
            break;
        case MenuState::SettingType::GroundMode: {
            // update ground_res and clean legacy typo
            config_kv_.erase("groud_res");
            auto label = menu_state_->GroundModes()[menu_state_->GroundModeIndex()].label;
            SaveConfigValue("ground_res", label);
            ApplyGroundDisplayMode(label);
            break;
        }
        case MenuState::SettingType::SkyMode:
            ApplySkyMode();
            break;
        case MenuState::SettingType::GroundPower:
            SaveConfigValue("driver_txpower_override", std::to_string(menu_state_->PowerLevels()[menu_state_->GroundPowerIndex()]));
            ApplyGroundPower();
            break;
        case MenuState::SettingType::Bitrate:
            ApplyBitrate();
            break;
        case MenuState::SettingType::SkyPower:
            ApplySkyPower();
            break;
        case MenuState::SettingType::Language: {
            auto lang = menu_state_->GetLanguage();
            SaveConfigValue("lang", lang == MenuState::Language::CN ? "cn" : "en");
            ApplyLanguageToImGui(lang);
            break;
        }
        case MenuState::SettingType::Recording: {
            bool recording = menu_state_->Recording();
            if (!SendRecordingCommand(recording)) {
                std::fprintf(stderr, "[AMLgsMenu] Failed to send recording command (%s)\n",
                             recording ? "record=1" : "record=0");
            }
            break;
        }
        case MenuState::SettingType::Firmware: {
            auto mode = menu_state_->GetFirmwareType();
            SaveConfigValue("firmware", mode == MenuState::FirmwareType::CCEdition ? "cc" : "official");
            RebuildTransport(mode);
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
    io.DisplaySize = ImVec2(static_cast<float>(fb_.width), static_cast<float>(fb_.height));
    io.MousePos = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

    uint64_t frame_counter = 0;
    auto last_log = std::chrono::steady_clock::now();
    auto frame_start = std::chrono::steady_clock::now();

    while (running_ && !menu_state_->ShouldExit())
    {
        const auto loop_begin = frame_start;
        ProcessInput(running_);
        DrainRemoteState();
        UpdateDeltaTime();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        renderer_->Render(running_);
        if (terminal_)
        {
            terminal_->render();
        }

        ImGui::Render();
        glViewport(0, 0, fb_.width, fb_.height);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglWaitGL();
        auto before_swap = std::chrono::steady_clock::now();
        if (!eglSwapBuffers(egl_display_, egl_surface_))
        {
            EGLint egl_err = eglGetError();
            std::fprintf(stderr, "[AMLgsMenu] eglSwapBuffers failed (err=0x%04x), stopping loop\n",
                         static_cast<unsigned int>(egl_err));
            EGLint width = 0, height = 0;
            if (egl_surface_ != EGL_NO_SURFACE &&
                eglQuerySurface(egl_display_, egl_surface_, EGL_WIDTH, &width) &&
                eglQuerySurface(egl_display_, egl_surface_, EGL_HEIGHT, &height))
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
                ProcessInput(running_);
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
    if (!cmd_runner_)
    {
        return;
    }
    if (menu_visible && !command_runner_active_)
    {
        cmd_runner_->Start();
        command_runner_active_ = true;
    }
    else if (!menu_visible && command_runner_active_)
    {
        cmd_runner_->Stop();
        command_runner_active_ = false;
    }
}

std::shared_ptr<CommandTransport> Application::AcquireTransport() const
{
    std::lock_guard<std::mutex> lock(transport_mutex_);
    return transport_;
}

void Application::RebuildTransport(MenuState::FirmwareType type)
{
    std::shared_ptr<CommandTransport> new_transport;
    if (type == MenuState::FirmwareType::Official)
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
}

void Application::RestartRemoteSync()
{
    if (remote_sync_thread_.joinable())
    {
        remote_sync_thread_.join();
    }
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
    CloseJoysticks();

    if (mav_receiver_)
    {
        mav_receiver_->Stop();
        mav_receiver_.reset();
    }
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
    if (remote_sync_thread_.joinable())
    {
        remote_sync_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport_.reset();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    if (egl_display_ != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (egl_context_ != EGL_NO_CONTEXT)
    {
        eglDestroyContext(egl_display_, egl_context_);
        egl_context_ = EGL_NO_CONTEXT;
    }
    if (egl_surface_ != EGL_NO_SURFACE)
    {
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }
    if (egl_display_ != EGL_NO_DISPLAY)
    {
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }
    if (li_ctx_)
    {
        libinput_unref(li_ctx_);
        li_ctx_ = nullptr;
    }
    if (udev_ctx_)
    {
        udev_unref(udev_ctx_);
        udev_ctx_ = nullptr;
    }
    if (fb_.fd >= 0)
    {
        close(fb_.fd);
        fb_.fd = -1;
    }
}

void Application::ApplyChannel()
{
    const auto &chs = menu_state_->Channels();
    if (chs.empty())
        return;
    int ch = chs[menu_state_->ChannelIndex()];
    if (auto transport = AcquireTransport())
    {
        std::unordered_map<std::string, std::string> vars{{"CHANNEL", std::to_string(ch)}};
        auto cmd = command_templates_.Render("remote", "channel", vars);
        if (!cmd.empty() && cmd_runner_)
        {
            cmd_runner_->EnqueueRemote([transport, cmd]()
                                       {
                if (!transport->Send(cmd, false))
                {
                    std::fprintf(stderr, "[AMLgsMenu] Failed to send channel command\n");
                } });
        }
    }
    ApplyLocalMonitorChannel(ch);
}

void Application::ApplyBandwidth()
{
    int bw = (menu_state_->BandwidthIndex() == 0) ? 10 : (menu_state_->BandwidthIndex() == 1 ? 20 : 40);
    if (auto transport = AcquireTransport())
    {
        std::unordered_map<std::string, std::string> vars{
            {"BANDWIDTH", std::to_string(bw)}};
        auto cmd = command_templates_.Render("remote", "bandwidth", vars);
        if (!cmd.empty() && cmd_runner_)
        {
            cmd_runner_->EnqueueRemote([transport, cmd]()
                                       {
                if (!transport->Send(cmd, false))
                {
                    std::fprintf(stderr, "[AMLgsMenu] Failed to send bandwidth command\n");
                } });
        }
    }
}

void Application::ApplySkyMode()
{
    const auto &sky_modes = menu_state_->SkyModes();
    if (sky_modes.empty())
        return;
    VideoMode mode = sky_modes[menu_state_->SkyModeIndex()];
    if (auto transport = AcquireTransport())
    {
        std::unordered_map<std::string, std::string> vars{
            {"WIDTH", std::to_string(mode.width)},
            {"HEIGHT", std::to_string(mode.height)},
            {"FPS", std::to_string(mode.refresh ? mode.refresh : 60)}};
        auto cmd = command_templates_.Render("remote", "sky_mode", vars);
        if (!cmd.empty() && cmd_runner_)
        {
            cmd_runner_->EnqueueRemote([transport, cmd]()
                                       {
                if (!transport->Send(cmd, false))
                {
                    std::fprintf(stderr, "[AMLgsMenu] Failed to send sky mode command\n");
                } });
        }
    }
}

void Application::ApplyGroundDisplayMode(const std::string &label)
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

void Application::ApplyBitrate()
{
    const auto &bitrates = menu_state_->Bitrates();
    if (bitrates.empty())
        return;
    int br_mbps = bitrates[menu_state_->BitrateIndex()];
    int br_kbps = br_mbps * 1024; // CLI expects kbps; e.g. 2 -> 2048
    if (auto transport = AcquireTransport())
    {
        std::unordered_map<std::string, std::string> vars{
            {"BITRATE_KBPS", std::to_string(br_kbps)}};
        auto cmd = command_templates_.Render("remote", "bitrate", vars);
        if (!cmd.empty() && cmd_runner_)
        {
            cmd_runner_->EnqueueRemote([transport, cmd]()
                                       {
                if (!transport->Send(cmd, false))
                {
                    std::fprintf(stderr, "[AMLgsMenu] Failed to send bitrate command\n");
                } });
        }
    }
}

void Application::ApplySkyPower()
{
    const auto &powers = menu_state_->PowerLevels();
    if (powers.empty())
        return;
    int p = powers[menu_state_->SkyPowerIndex()];
    if (auto transport = AcquireTransport())
    {
        int tx_pwr = p * 50;
        std::unordered_map<std::string, std::string> vars{
            {"POWER", std::to_string(p)},
            {"TXPOWER", std::to_string(tx_pwr)}};
        auto cmd = command_templates_.Render("remote", "sky_power", vars);
        if (!cmd.empty() && cmd_runner_)
        {
            cmd_runner_->EnqueueRemote([transport, cmd]()
                                       {
                if (!transport->Send(cmd, false))
                {
                    std::fprintf(stderr, "[AMLgsMenu] Failed to send tx power command\n");
                } });
        }
    }
}

void Application::ApplyGroundPower()
{
    const auto &powers = menu_state_->PowerLevels();
    if (powers.empty())
        return;
    int p = powers[menu_state_->GroundPowerIndex()];
    ApplyLocalMonitorPower(p);
}

void Application::ApplyLocalMonitorChannel(int channel)
{
    if (channel <= 0)
        return;
    int bw_mhz = (menu_state_->BandwidthIndex() == 0) ? 10 : (menu_state_->BandwidthIndex() == 1 ? 20 : 40);
    std::string bw_suffix;
    if (bw_mhz == 20)
        bw_suffix = " HT20";
    else if (bw_mhz == 40)
        bw_suffix = " HT40+";
    std::unordered_map<std::string, std::string> vars{
        {"CHANNEL", std::to_string(channel)},
        {"BW_SUFFIX", bw_suffix}};
    auto cmd = command_templates_.Render("local", "monitor_channel", vars);
    if (!cmd.empty() && cmd_runner_)
    {
        cmd_runner_->EnqueueShell(cmd);
    }
}

void Application::ApplyLocalMonitorPower(int power_level)
{
    if (power_level <= 0)
        return;
    int tx_pwr = power_level * 50;
    std::unordered_map<std::string, std::string> vars{
        {"POWER", std::to_string(power_level)},
        {"TXPOWER", std::to_string(tx_pwr)}};
    auto cmd = command_templates_.Render("local", "monitor_power", vars);
    if (!cmd.empty() && cmd_runner_)
    {
        cmd_runner_->EnqueueShell(cmd);
    }
}

bool Application::SendRecordingCommand(bool enable)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        std::perror("[AMLgsMenu] socket(record)");
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5612);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const char *payload = enable ? "record=1" : "record=0";
    ssize_t sent = sendto(sock, payload, std::strlen(payload), 0,
                          reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    if (sent < 0)
    {
        std::perror("[AMLgsMenu] sendto(record)");
        close(sock);
        return false;
    }
    close(sock);
    return true;
}

bool Application::InitFramebuffer(FbContext &fb)
{
    fb.fd = open("/dev/fb0", O_RDWR);
    if (fb.fd < 0)
    {
        std::perror("[AMLgsMenu] open(/dev/fb0)");
        return false;
    }
    fb_var_screeninfo vinfo{};
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &vinfo) != 0)
    {
        std::perror("[AMLgsMenu] ioctl(FBIOGET_VSCREENINFO)");
        return false;
    }
    fb.width = static_cast<int>(vinfo.xres);
    fb.height = static_cast<int>(vinfo.yres);
    return true;
}

bool Application::InitEgl(const FbContext &fb)
{
    EGLint major = 0, minor = 0;
    EGLint num_configs = 0;
    EGLint attr[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE};

    egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display_ == EGL_NO_DISPLAY)
    {
        std::fprintf(stderr, "[AMLgsMenu] eglGetDisplay failed\n");
        return false;
    }
    if (!eglInitialize(egl_display_, &major, &minor))
    {
        std::fprintf(stderr, "[AMLgsMenu] eglInitialize failed\n");
        return false;
    }
    if (!eglChooseConfig(egl_display_, attr, &egl_config_, 1, &num_configs) || num_configs == 0)
    {
        std::fprintf(stderr, "[AMLgsMenu] eglChooseConfig failed\n");
        return false;
    }
    EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx_attr);
    if (egl_context_ == EGL_NO_CONTEXT)
    {
        std::fprintf(stderr, "[AMLgsMenu] eglCreateContext failed\n");
        return false;
    }

    fbdev_window native_window{fb.width, fb.height};
    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, &native_window, nullptr);
    if (egl_surface_ == EGL_NO_SURFACE)
    {
        std::fprintf(stderr, "[AMLgsMenu] eglCreateWindowSurface failed\n");
        return false;
    }
    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_))
    {
        std::fprintf(stderr, "[AMLgsMenu] eglMakeCurrent failed\n");
        return false;
    }
    // Restore vsync (swap interval 1); adjust if target platform requires immediate swaps.
    eglSwapInterval(egl_display_, 1);
    return true;
}

bool Application::InitInput()
{
    udev_ctx_ = udev_new();
    if (!udev_ctx_)
    {
        std::fprintf(stderr, "[AMLgsMenu] udev_new failed\n");
        return false;
    }
    static const libinput_interface iface = {
        &Application::LibinputOpen,
        &Application::LibinputClose,
    };
    li_ctx_ = libinput_udev_create_context(&iface, nullptr, udev_ctx_);
    if (!li_ctx_)
    {
        std::fprintf(stderr, "[AMLgsMenu] libinput_udev_create_context failed\n");
        return false;
    }
    if (libinput_udev_assign_seat(li_ctx_, "seat0") != 0)
    {
        std::fprintf(stderr, "[AMLgsMenu] libinput_udev_assign_seat failed\n");
        return false;
    }
    return true;
}

void Application::ProcessInput(bool &running)
{
    if (!li_ctx_)
        return;
    pollfd pfd{};
    pfd.fd = libinput_get_fd(li_ctx_);
    pfd.events = POLLIN;
    poll(&pfd, 1, 1);
    if (libinput_dispatch(li_ctx_) != 0)
        return;
    libinput_event *event = nullptr;
    while ((event = libinput_get_event(li_ctx_)) != nullptr)
    {
        HandleLibinputEvent(event, running);
        libinput_event_destroy(event);
    }
    PollJoysticks(running);
    auto now = std::chrono::steady_clock::now();
    if (now - last_js_scan_ > std::chrono::seconds(2))
    {
        ScanJoysticks();
        last_js_scan_ = now;
    }
}

void Application::HandleLibinputEvent(struct libinput_event *event, bool &running)
{
    ImGuiIO &io = ImGui::GetIO();
    auto add_key = [&](ImGuiKey key, bool down)
    {
        io.AddKeyEvent(key, down);
    };
    auto add_key_if_mapped = [&](uint32_t code, bool down)
    {
        ImGuiKey mapped = ImGuiKey_None;
        if (code >= KEY_A && code <= KEY_Z)
        {
            mapped = static_cast<ImGuiKey>(ImGuiKey_A + (code - KEY_A));
        }
        else if (code >= KEY_1 && code <= KEY_9)
        {
            mapped = static_cast<ImGuiKey>(ImGuiKey_1 + (code - KEY_1));
        }
        else if (code == KEY_0)
        {
            mapped = ImGuiKey_0;
        }
        else if (code == KEY_SPACE)
        {
            mapped = ImGuiKey_Space;
        }
        else if (code == KEY_MINUS)
        {
            mapped = ImGuiKey_Minus;
        }
        else if (code == KEY_EQUAL)
        {
            mapped = ImGuiKey_Equal;
        }
        else if (code == KEY_DOT)
        {
            mapped = ImGuiKey_Period;
        }
        else if (code == KEY_COMMA)
        {
            mapped = ImGuiKey_Comma;
        }
        else if (code == KEY_SLASH)
        {
            mapped = ImGuiKey_Slash;
        }
        else if (code == KEY_SEMICOLON)
        {
            mapped = ImGuiKey_Semicolon;
        }
        else if (code == KEY_APOSTROPHE)
        {
            mapped = ImGuiKey_Apostrophe;
        }
        else if (code == KEY_GRAVE)
        {
            mapped = ImGuiKey_GraveAccent;
        }
        else if (code == KEY_LEFTBRACE)
        {
            mapped = ImGuiKey_LeftBracket;
        }
        else if (code == KEY_RIGHTBRACE)
        {
            mapped = ImGuiKey_RightBracket;
        }
        else if (code == KEY_BACKSLASH)
        {
            mapped = ImGuiKey_Backslash;
        }
        else if (code >= KEY_KP1 && code <= KEY_KP9)
        {
            mapped = static_cast<ImGuiKey>(ImGuiKey_Keypad1 + (code - KEY_KP1));
        }
        else if (code == KEY_KP0)
        {
            mapped = ImGuiKey_Keypad0;
        }
        else if (code == KEY_KPPLUS)
        {
            mapped = ImGuiKey_KeypadAdd;
        }
        else if (code == KEY_KPMINUS)
        {
            mapped = ImGuiKey_KeypadSubtract;
        }
        else if (code == KEY_KPASTERISK)
        {
            mapped = ImGuiKey_KeypadMultiply;
        }
        else if (code == KEY_KPSLASH)
        {
            mapped = ImGuiKey_KeypadDivide;
        }
        else if (code == KEY_KPDOT)
        {
            mapped = ImGuiKey_KeypadDecimal;
        }
        else if (code == KEY_KPENTER)
        {
            mapped = ImGuiKey_KeypadEnter;
        }

        if (mapped != ImGuiKey_None)
        {
            io.AddKeyEvent(mapped, down);
        }
    };
    switch (libinput_event_get_type(event))
    {
    case LIBINPUT_EVENT_POINTER_MOTION:
    {
        auto *m = libinput_event_get_pointer_event(event);
        io.MousePos.x += static_cast<float>(libinput_event_pointer_get_dx(m));
        io.MousePos.y += static_cast<float>(libinput_event_pointer_get_dy(m));
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON:
    {
        auto *b = libinput_event_get_pointer_event(event);
        uint32_t button = libinput_event_pointer_get_button(b);
        auto state = libinput_event_pointer_get_button_state(b);
        bool pressed = (state == LIBINPUT_BUTTON_STATE_PRESSED);
        if (button == BTN_LEFT)
            io.MouseDown[0] = pressed;
        if (button == BTN_RIGHT && pressed)
        {
            menu_state_->ToggleMenuVisibility();
            UpdateCommandRunner(menu_state_->MenuVisible());
        }
        break;
    }
    case LIBINPUT_EVENT_POINTER_AXIS:
    {
        auto *a = libinput_event_get_pointer_event(event);
        if (libinput_event_pointer_has_axis(a, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
        {
            double v = libinput_event_pointer_get_axis_value(a, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
            const float kScrollScale = 0.35f; // slow down scroll for combos
            io.MouseWheel += static_cast<float>(-v) * kScrollScale;
        }
        break;
    }
    case LIBINPUT_EVENT_KEYBOARD_KEY:
    {
        auto *k = libinput_event_get_keyboard_event(event);
        uint32_t key = libinput_event_keyboard_get_key(k);
        auto state = libinput_event_keyboard_get_key_state(k);
        bool down = (state == LIBINPUT_KEY_STATE_PRESSED);
        bool terminal_visible = terminal_ && terminal_->isTerminalVisible();
        bool menu_visible = menu_state_->MenuVisible();
        auto handle_gamepad_nav = [&](uint32_t code, bool pressed) -> bool
        {
            if (!menu_visible)
                return false;
            switch (code)
            {
            case BTN_DPAD_UP:
                add_key(ImGuiKey_UpArrow, pressed);
                return true;
            case BTN_DPAD_DOWN:
                add_key(ImGuiKey_DownArrow, pressed);
                return true;
            case BTN_DPAD_LEFT:
                add_key(ImGuiKey_LeftArrow, pressed);
                return true;
            case BTN_DPAD_RIGHT:
                add_key(ImGuiKey_RightArrow, pressed);
                return true;
            case BTN_SOUTH: // A button confirms selections
                add_key(ImGuiKey_Enter, pressed);
                return true;
            default:
                return false;
            }
        };

        if (state == LIBINPUT_KEY_STATE_PRESSED)
        {
            if (!terminal_visible && (key == KEY_X || key == BTN_WEST))
            {
                menu_state_->ToggleMenuVisibility();
                UpdateCommandRunner(menu_state_->MenuVisible());
            }
            if (!terminal_visible && (key == KEY_LEFTALT || key == KEY_RIGHTALT))
            {
                menu_state_->ToggleMenuVisibility();
                UpdateCommandRunner(menu_state_->MenuVisible());
            }
            if (key == KEY_ESC)
            {
                running = false;
            }
            if (terminal_visible && key == KEY_C && (io.KeyCtrl || io.KeySuper))
            {
                std::fprintf(stdout, "[AMLgsMenu] Terminal ctrl-c triggered\n");
                std::fflush(stdout);
                terminal_->SendControlChar('\x03');
                terminal_->SendSignal(SIGINT);
            }
            if (!terminal_visible && key == KEY_C && (io.KeyCtrl || io.KeySuper))
            {
                std::fprintf(stdout, "[AMLgsMenu] Ctrl-C quitting app\n");
                std::fflush(stdout);
                running = false;
            }
        }

        handle_gamepad_nav(key, down);
        switch (key)
        {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            io.KeyShift = down;
            add_key(ImGuiKey_LeftShift, down);
            add_key(ImGuiKey_RightShift, down);
            io.AddKeyEvent(ImGuiMod_Shift, down);
            break;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            io.KeyCtrl = down;
            add_key(ImGuiKey_LeftCtrl, down);
            add_key(ImGuiKey_RightCtrl, down);
            io.AddKeyEvent(ImGuiMod_Ctrl, down);
            break;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
            io.KeyAlt = down;
            add_key(ImGuiKey_LeftAlt, down);
            add_key(ImGuiKey_RightAlt, down);
            io.AddKeyEvent(ImGuiMod_Alt, down);
            break;
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            io.KeySuper = down;
            add_key(ImGuiKey_LeftSuper, down);
            add_key(ImGuiKey_RightSuper, down);
            io.AddKeyEvent(ImGuiMod_Super, down);
            break;
        case KEY_ENTER:
        case KEY_KPENTER:
            add_key(ImGuiKey_Enter, down);
            break;
        case KEY_BACKSPACE:
            add_key(ImGuiKey_Backspace, down);
            break;
        case KEY_TAB:
            add_key(ImGuiKey_Tab, down);
            break;
        case KEY_UP:
            add_key(ImGuiKey_UpArrow, down);
            break;
        case KEY_DOWN:
            add_key(ImGuiKey_DownArrow, down);
            break;
        case KEY_LEFT:
            add_key(ImGuiKey_LeftArrow, down);
            break;
        case KEY_RIGHT:
            add_key(ImGuiKey_RightArrow, down);
            break;
        case KEY_HOME:
            add_key(ImGuiKey_Home, down);
            break;
        case KEY_END:
            add_key(ImGuiKey_End, down);
            break;
        case KEY_DELETE:
            add_key(ImGuiKey_Delete, down);
            break;
        case KEY_PAGEUP:
            add_key(ImGuiKey_PageUp, down);
            break;
        case KEY_PAGEDOWN:
            add_key(ImGuiKey_PageDown, down);
            break;
        default:
            break;
        }

        add_key_if_mapped(key, down);

        if (down)
        {
            auto emit_char = [&](ImWchar c)
            { io.AddInputCharacter(c); };
            auto emit_printable = [&](uint32_t code, bool shift) -> bool
            {
                switch (code)
                {
                case KEY_A:
                    emit_char(shift ? 'A' : 'a');
                    return true;
                case KEY_B:
                    emit_char(shift ? 'B' : 'b');
                    return true;
                case KEY_C:
                    emit_char(shift ? 'C' : 'c');
                    return true;
                case KEY_D:
                    emit_char(shift ? 'D' : 'd');
                    return true;
                case KEY_E:
                    emit_char(shift ? 'E' : 'e');
                    return true;
                case KEY_F:
                    emit_char(shift ? 'F' : 'f');
                    return true;
                case KEY_G:
                    emit_char(shift ? 'G' : 'g');
                    return true;
                case KEY_H:
                    emit_char(shift ? 'H' : 'h');
                    return true;
                case KEY_I:
                    emit_char(shift ? 'I' : 'i');
                    return true;
                case KEY_J:
                    emit_char(shift ? 'J' : 'j');
                    return true;
                case KEY_K:
                    emit_char(shift ? 'K' : 'k');
                    return true;
                case KEY_L:
                    emit_char(shift ? 'L' : 'l');
                    return true;
                case KEY_M:
                    emit_char(shift ? 'M' : 'm');
                    return true;
                case KEY_N:
                    emit_char(shift ? 'N' : 'n');
                    return true;
                case KEY_O:
                    emit_char(shift ? 'O' : 'o');
                    return true;
                case KEY_P:
                    emit_char(shift ? 'P' : 'p');
                    return true;
                case KEY_Q:
                    emit_char(shift ? 'Q' : 'q');
                    return true;
                case KEY_R:
                    emit_char(shift ? 'R' : 'r');
                    return true;
                case KEY_S:
                    emit_char(shift ? 'S' : 's');
                    return true;
                case KEY_T:
                    emit_char(shift ? 'T' : 't');
                    return true;
                case KEY_U:
                    emit_char(shift ? 'U' : 'u');
                    return true;
                case KEY_V:
                    emit_char(shift ? 'V' : 'v');
                    return true;
                case KEY_W:
                    emit_char(shift ? 'W' : 'w');
                    return true;
                case KEY_X:
                    emit_char(shift ? 'X' : 'x');
                    return true;
                case KEY_Y:
                    emit_char(shift ? 'Y' : 'y');
                    return true;
                case KEY_Z:
                    emit_char(shift ? 'Z' : 'z');
                    return true;
                case KEY_1:
                    emit_char(shift ? '!' : '1');
                    return true;
                case KEY_2:
                    emit_char(shift ? '@' : '2');
                    return true;
                case KEY_3:
                    emit_char(shift ? '#' : '3');
                    return true;
                case KEY_4:
                    emit_char(shift ? '$' : '4');
                    return true;
                case KEY_5:
                    emit_char(shift ? '%' : '5');
                    return true;
                case KEY_6:
                    emit_char(shift ? '^' : '6');
                    return true;
                case KEY_7:
                    emit_char(shift ? '&' : '7');
                    return true;
                case KEY_8:
                    emit_char(shift ? '*' : '8');
                    return true;
                case KEY_9:
                    emit_char(shift ? '(' : '9');
                    return true;
                case KEY_0:
                    emit_char(shift ? ')' : '0');
                    return true;
                case KEY_SPACE:
                    emit_char(' ');
                    return true;
                case KEY_MINUS:
                    emit_char(shift ? '_' : '-');
                    return true;
                case KEY_EQUAL:
                    emit_char(shift ? '+' : '=');
                    return true;
                case KEY_LEFTBRACE:
                    emit_char(shift ? '{' : '[');
                    return true;
                case KEY_RIGHTBRACE:
                    emit_char(shift ? '}' : ']');
                    return true;
                case KEY_BACKSLASH:
                    emit_char(shift ? '|' : '\\');
                    return true;
                case KEY_SEMICOLON:
                    emit_char(shift ? ':' : ';');
                    return true;
                case KEY_APOSTROPHE:
                    emit_char(shift ? '"' : '\'');
                    return true;
                case KEY_GRAVE:
                    emit_char(shift ? '~' : '`');
                    return true;
                case KEY_COMMA:
                    emit_char(shift ? '<' : ',');
                    return true;
                case KEY_DOT:
                    emit_char(shift ? '>' : '.');
                    return true;
                case KEY_SLASH:
                    emit_char(shift ? '?' : '/');
                    return true;
                case KEY_KP0:
                    emit_char('0');
                    return true;
                case KEY_KP1:
                    emit_char('1');
                    return true;
                case KEY_KP2:
                    emit_char('2');
                    return true;
                case KEY_KP3:
                    emit_char('3');
                    return true;
                case KEY_KP4:
                    emit_char('4');
                    return true;
                case KEY_KP5:
                    emit_char('5');
                    return true;
                case KEY_KP6:
                    emit_char('6');
                    return true;
                case KEY_KP7:
                    emit_char('7');
                    return true;
                case KEY_KP8:
                    emit_char('8');
                    return true;
                case KEY_KP9:
                    emit_char('9');
                    return true;
                case KEY_KPPLUS:
                    emit_char('+');
                    return true;
                case KEY_KPMINUS:
                    emit_char('-');
                    return true;
                case KEY_KPASTERISK:
                    emit_char('*');
                    return true;
                case KEY_KPSLASH:
                    emit_char('/');
                    return true;
                case KEY_KPDOT:
                    emit_char('.');
                    return true;
                default:
                    return false;
                }
            };

            emit_printable(key, io.KeyShift);
        }
        break;
    }
    default:
        break;
    }
    io.MousePos.x = std::max(0.0f, std::min(io.MousePos.x, static_cast<float>(fb_.width)));
    io.MousePos.y = std::max(0.0f, std::min(io.MousePos.y, static_cast<float>(fb_.height)));
}

void Application::ScanJoysticks()
{
    glob_t globbuf{};
    if (glob("/dev/input/js*", 0, nullptr, &globbuf) != 0)
    {
        return;
    }
    for (size_t i = 0; i < globbuf.gl_pathc; ++i)
    {
        const std::string path = globbuf.gl_pathv[i];
        bool exists = std::any_of(joysticks_.begin(), joysticks_.end(),
                                  [&](const JoystickDevice &dev)
                                  { return dev.path == path; });
        if (exists)
            continue;

        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        JoystickDevice dev;
        dev.fd = fd;
        dev.path = path;
        unsigned char axes = 0;
        if (ioctl(fd, JSIOCGAXES, &axes) < 0 || axes == 0)
            axes = 8;
        dev.axes.assign(axes, 0);
        unsigned char buttons = 0;
        if (ioctl(fd, JSIOCGBUTTONS, &buttons) < 0 || buttons == 0)
            buttons = 16;
        dev.buttons.assign(buttons, 0);
        joysticks_.push_back(std::move(dev));
        std::fprintf(stdout, "[AMLgsMenu] Gamepad attached: %s\n", path.c_str());
        std::fflush(stdout);
    }
    globfree(&globbuf);
}

void Application::CloseJoysticks()
{
    for (auto &dev : joysticks_)
    {
        if (dev.fd >= 0)
        {
            close(dev.fd);
            dev.fd = -1;
        }
    }
    joysticks_.clear();
}

void Application::RemoveJoystick(size_t index)
{
    if (index >= joysticks_.size())
        return;

    ImGuiIO &io = ImGui::GetIO();
    auto release_dir = [&](bool &state, ImGuiKey key)
    {
        if (state)
        {
            io.AddKeyEvent(key, false);
            state = false;
        }
    };
    release_dir(joysticks_[index].dpad_up, ImGuiKey_UpArrow);
    release_dir(joysticks_[index].dpad_down, ImGuiKey_DownArrow);
    release_dir(joysticks_[index].dpad_left, ImGuiKey_LeftArrow);
    release_dir(joysticks_[index].dpad_right, ImGuiKey_RightArrow);

    if (joysticks_[index].fd >= 0)
    {
        close(joysticks_[index].fd);
    }
    std::fprintf(stdout, "[AMLgsMenu] Gamepad removed: %s\n", joysticks_[index].path.c_str());
    std::fflush(stdout);
    joysticks_.erase(joysticks_.begin() + index);
}

void Application::PollJoysticks(bool &running)
{
    (void)running;
    if (joysticks_.empty())
        return;

    std::vector<size_t> to_remove;
    for (size_t i = 0; i < joysticks_.size(); ++i)
    {
        auto &dev = joysticks_[i];
        pollfd pfd{};
        pfd.fd = dev.fd;
        pfd.events = POLLIN;
        int pret = poll(&pfd, 1, 0);
        if (pret < 0)
        {
            if (errno == EINTR)
                continue;
            to_remove.push_back(i);
            continue;
        }
        if (pret == 0)
            continue;

        if (pfd.revents & (POLLERR | POLLHUP))
        {
            to_remove.push_back(i);
            continue;
        }

        while (true)
        {
            js_event ev{};
            ssize_t n = read(dev.fd, &ev, sizeof(ev));
            if (n == static_cast<ssize_t>(sizeof(ev)))
            {
                uint8_t type = ev.type & ~JS_EVENT_INIT;
                if (type == JS_EVENT_BUTTON)
                {
                    bool pressed = ev.value != 0;
                    if (ev.number < dev.buttons.size())
                        dev.buttons[ev.number] = pressed;
                    HandleJoystickButton(ev.number, pressed);
                }
                else if (type == JS_EVENT_AXIS)
                {
                    if (ev.number < dev.axes.size())
                        dev.axes[ev.number] = ev.value;
                    HandleJoystickAxis(dev, ev.number, ev.value);
                }
            }
            else
            {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                to_remove.push_back(i);
                break;
            }
        }
    }

    if (!to_remove.empty())
    {
        std::sort(to_remove.begin(), to_remove.end());
        to_remove.erase(std::unique(to_remove.begin(), to_remove.end()), to_remove.end());
        for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it)
        {
            RemoveJoystick(*it);
        }
    }
}

void Application::HandleJoystickButton(int button, bool pressed)
{
    if (!menu_state_)
        return;
    ImGuiIO &io = ImGui::GetIO();
    bool menu_visible = menu_state_->MenuVisible();
    bool terminal_visible = terminal_ && terminal_->isTerminalVisible();

    switch (button)
    {
    case 0: // A
        if (!menu_visible && pressed)
        {
            menu_state_->SetMenuVisible(true);
        }
        else if (menu_visible)
        {
            io.AddKeyEvent(ImGuiKey_Enter, pressed);
        }
        break;
    case 1: // B
        if (menu_visible)
            io.AddKeyEvent(ImGuiKey_Escape, pressed);
        break;
    case 2: // X
        if (pressed && !terminal_visible)
        {
            menu_state_->ToggleMenuVisibility();
            UpdateCommandRunner(menu_state_->MenuVisible());
        }
        break;
    case 3: // Y
        if (pressed && !terminal_visible)
            menu_state_->SetMenuVisible(true);
        break;
    case 6: // Back
        if (pressed && !terminal_visible)
        {
            menu_state_->ToggleMenuVisibility();
            UpdateCommandRunner(menu_state_->MenuVisible());
        }
        break;
    default:
        break;
    }
}

void Application::HandleJoystickAxis(JoystickDevice &dev, int axis, int16_t value)
{
    if (!menu_state_)
        return;
    ImGuiIO &io = ImGui::GetIO();
    bool menu_visible = menu_state_->MenuVisible();
    auto release = [&](bool &state, ImGuiKey key)
    {
        if (state)
        {
            io.AddKeyEvent(key, false);
            state = false;
        }
    };
    if (!menu_visible)
    {
        release(dev.dpad_up, ImGuiKey_UpArrow);
        release(dev.dpad_down, ImGuiKey_DownArrow);
        release(dev.dpad_left, ImGuiKey_LeftArrow);
        release(dev.dpad_right, ImGuiKey_RightArrow);
        return;
    }

    constexpr int16_t kDeadZone = 12000;
    auto update_dir = [&](bool &state, bool new_state, ImGuiKey key)
    {
        if (state != new_state)
        {
            io.AddKeyEvent(key, new_state);
            state = new_state;
        }
    };

    if (axis == 6)
    {
        update_dir(dev.dpad_left, value < -kDeadZone, ImGuiKey_LeftArrow);
        update_dir(dev.dpad_right, value > kDeadZone, ImGuiKey_RightArrow);
    }
    else if (axis == 7)
    {
        update_dir(dev.dpad_up, value < -kDeadZone, ImGuiKey_UpArrow);
        update_dir(dev.dpad_down, value > kDeadZone, ImGuiKey_DownArrow);
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
    std::ifstream file(config_path_);
    if (!file.is_open())
    {
        return;
    }
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        // trim spaces
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t\r\n") + 1);
        config_kv_[key] = val;
    }

    auto it_ch = config_kv_.find("channel");
    if (it_ch != config_kv_.end())
    {
        int ch = std::stoi(it_ch->second);
        int idx = FindChannelIndex(ch);
        if (idx >= 0)
            menu_state_->SetChannelIndex(idx);
    }
    auto it_bw = config_kv_.find("bandwidth");
    if (it_bw != config_kv_.end())
    {
        int bw = std::stoi(it_bw->second);
        if (bw == 10)
            menu_state_->SetBandwidthIndex(0);
        else if (bw == 20)
            menu_state_->SetBandwidthIndex(1);
        else if (bw == 40)
            menu_state_->SetBandwidthIndex(2);
    }
    auto it_power = config_kv_.find("driver_txpower_override");
    if (it_power != config_kv_.end())
    {
        int p = std::stoi(it_power->second);
        int idx = FindPowerIndex(p);
        if (idx >= 0)
        {
            menu_state_->SetGroundPowerIndex(idx);
            menu_state_->SetSkyPowerIndex(idx);
        }
    }
    // support both correct key and legacy typo
    auto it_res = config_kv_.find("ground_res");
    if (it_res == config_kv_.end())
    {
        it_res = config_kv_.find("groud_res");
    }
    if (it_res != config_kv_.end())
    {
        std::string label = it_res->second;
        while (!label.empty() && (label.back() == '*' || label.back() == ' ' || label.back() == '\t'))
            label.pop_back();
        int idx = FindGroundModeIndex(label);
        if (idx >= 0)
            menu_state_->SetGroundModeIndex(idx);
    }
    auto it_lang = config_kv_.find("lang");
    if (it_lang != config_kv_.end())
    {
        std::string v = it_lang->second;
        for (auto &c : v)
            c = static_cast<char>(tolower(c));
        if (v == "en")
            menu_state_->SetLanguage(MenuState::Language::EN);
        else if (v == "cn")
            menu_state_->SetLanguage(MenuState::Language::CN);
    }
    auto it_fw = config_kv_.find("firmware");
    if (it_fw != config_kv_.end())
    {
        std::string v = it_fw->second;
        for (auto &c : v)
            c = static_cast<char>(tolower(c));
        if (v == "official")
        {
            menu_state_->SetFirmwareType(MenuState::FirmwareType::Official);
        }
        else
        {
            menu_state_->SetFirmwareType(MenuState::FirmwareType::CCEdition);
        }
    }
}

void Application::StartRemoteSync()
{
    if (remote_sync_thread_.joinable())
    {
        return;
    }
    auto transport = AcquireTransport();
    if (!transport)
    {
        return;
    }
    remote_sync_thread_ = std::thread([this, transport]()
                                      {
        RemoteStateSnapshot snapshot{};
        if (CollectRemoteState(snapshot, transport))
        {
            std::lock_guard<std::mutex> lock(remote_state_mutex_);
            pending_remote_state_ = snapshot;
            remote_sync_ready_ = true;
        } });
}

void Application::DrainRemoteState()
{
    RemoteStateSnapshot snapshot{};
    bool apply = false;
    {
        std::lock_guard<std::mutex> lock(remote_state_mutex_);
        if (remote_sync_ready_)
        {
            snapshot = pending_remote_state_;
            remote_sync_ready_ = false;
            apply = true;
        }
    }
    if (apply)
    {
        ApplyRemoteStateSnapshot(snapshot);
    }
}

bool Application::CollectRemoteState(RemoteStateSnapshot &snapshot,
                                     const std::shared_ptr<CommandTransport> &transport)
{
    if (!transport)
    {
        return false;
    }
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
    std::string value;
    if (QueryRemoteValue("channel", value, transport))
    {
        int ch = 0;
        if (try_parse_int(value, ch))
        {
            snapshot.has_channel = true;
            snapshot.channel = ch;
            any = true;
        }
    }
    if (QueryRemoteValue("bandwidth", value, transport))
    {
        int bw = 0;
        if (try_parse_int(value, bw))
        {
            snapshot.has_bandwidth = true;
            snapshot.bandwidth = bw;
            any = true;
        }
    }
    if (QueryRemoteValue("sky_power", value, transport))
    {
        int p = 0;
        if (try_parse_int(value, p))
        {
            snapshot.has_power = true;
            snapshot.power = p;
            any = true;
        }
    }
    if (QueryRemoteValue("bitrate", value, transport))
    {
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
    bool have_size = QueryRemoteValue("sky_size", size_value, transport);
    bool have_fps = QueryRemoteValue("sky_fps", fps_value, transport);
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

void Application::ApplyRemoteStateSnapshot(const RemoteStateSnapshot &snapshot)
{
    if (!menu_state_)
        return;
    if (snapshot.has_channel)
    {
        int idx = FindChannelIndex(snapshot.channel);
        if (idx >= 0)
        {
            menu_state_->SetChannelIndex(idx);
            config_kv_["channel"] = std::to_string(snapshot.channel);
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
            menu_state_->SetBandwidthIndex(idx);
            config_kv_["bandwidth"] = std::to_string(bw);
            std::fprintf(stdout, "[AMLgsMenu] Remote bandwidth synced: %d MHz\n", bw);
        }
    }
    if (snapshot.has_power)
    {
        int idx = FindPowerIndex(snapshot.power);
        if (idx >= 0)
        {
            menu_state_->SetSkyPowerIndex(idx);
            menu_state_->SetGroundPowerIndex(idx);
            config_kv_["driver_txpower_override"] = std::to_string(snapshot.power);
            std::fprintf(stdout, "[AMLgsMenu] Remote TX power synced: %d\n", snapshot.power);
        }
    }
    if (snapshot.has_bitrate)
    {
        int kbps = snapshot.bitrate_kbps;
        int mbps = static_cast<int>(std::round(static_cast<double>(kbps) / 1024.0));
        if (mbps < 1)
            mbps = 1;
        int idx = FindBitrateIndex(mbps);
        if (idx >= 0)
        {
            menu_state_->SetBitrateIndex(idx);
            std::fprintf(stdout, "[AMLgsMenu] Remote bitrate synced: %d kbps\n", kbps);
        }
    }
    if (snapshot.has_sky_mode)
    {
        int idx = FindSkyModeIndex(snapshot.width, snapshot.height, snapshot.fps);
        if (idx >= 0)
        {
            menu_state_->SetSkyModeIndex(idx);
            std::fprintf(stdout, "[AMLgsMenu] Remote sky mode synced: %dx%d @ %dHz\n",
                         snapshot.width, snapshot.height, snapshot.fps);
        }
    }
}

bool Application::QueryRemoteValue(const std::string &key, std::string &out,
                                   const std::shared_ptr<CommandTransport> &transport)
{
    if (!transport)
        return false;
    auto cmd = command_templates_.Render("remote_query", key, {});
    if (cmd.empty())
        return false;
    std::vector<std::string> response;
    if (!transport->SendWithReply(cmd, response, 1000))
        return false;
    for (const auto &line : response)
    {
        auto trimmed = TrimCopy(line);
        if (trimmed.empty())
            continue;
        if (trimmed == "timeout")
            continue;
        out = trimmed;
        return true;
    }
    return false;
}

void Application::SaveConfigValue(const std::string &key, const std::string &value)
{
    config_kv_[key] = value;
    const std::string tmp = config_path_ + ".tmp" + std::to_string(std::rand());

    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open())
            return;

        for (const auto &kv : config_kv_)
            file << kv.first << "=" << kv.second << "\n";
    }
    std::rename(tmp.c_str(), config_path_.c_str());
    // configUpdated_ = true;
}

void Application::SaveConfig()
{
    // std::cout << "[AMLgsMenu] Saving config to " << config_path_ << " " << configUpdated_ << std::endl;
    if (configUpdated_)
    {
        const std::string tmp = config_path_ + ".tmp";

        {
            std::ofstream file(tmp, std::ios::trunc);
            if (!file.is_open())
                return;

            for (const auto &kv : config_kv_)
                file << kv.first << "=" << kv.second << "\n";
        }
        std::rename(tmp.c_str(), config_path_.c_str());
    }
    configUpdated_ = false;
}

int Application::FindChannelIndex(int channel_val) const
{
    const auto &chs = menu_state_->Channels();
    for (size_t i = 0; i < chs.size(); ++i)
    {
        if (chs[i] == channel_val)
            return static_cast<int>(i);
    }
    return -1;
}

int Application::FindPowerIndex(int power_val) const
{
    const auto &pwr = menu_state_->PowerLevels();
    for (size_t i = 0; i < pwr.size(); ++i)
    {
        if (pwr[i] == power_val)
            return static_cast<int>(i);
    }
    return -1;
}

int Application::FindGroundModeIndex(const std::string &label) const
{
    const auto &modes = menu_state_->GroundModes();
    for (size_t i = 0; i < modes.size(); ++i)
    {
        if (modes[i].label == label)
            return static_cast<int>(i);
    }
    return -1;
}

int Application::FindSkyModeIndex(int width, int height, int refresh) const
{
    const auto &modes = menu_state_->SkyModes();
    for (size_t i = 0; i < modes.size(); ++i)
    {
        if (modes[i].width == width && modes[i].height == height && modes[i].refresh == refresh)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int Application::FindBitrateIndex(int bitrate_mbps) const
{
    const auto &bitrates = menu_state_->Bitrates();
    for (size_t i = 0; i < bitrates.size(); ++i)
    {
        if (bitrates[i] == bitrate_mbps)
            return static_cast<int>(i);
    }
    return -1;
}

void Application::ApplyLanguageToImGui(MenuState::Language lang)
{
    // Font loading already follows -t 参数或默认字体，不在语言切换时重建，避免丢失用户指定字体。
    (void)lang;
}

int Application::LibinputOpen(const char *path, int flags, void *user_data)
{
    (void)user_data;
    return open(path, flags | O_NONBLOCK);
}

void Application::LibinputClose(int fd, void *user_data)
{
    (void)user_data;
    if (fd >= 0)
    {
        close(fd);
    }
}
