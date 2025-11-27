#include "application.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "video_mode.h"
#include "mavlink_receiver.h"
#include "udp_command_client.h"
#include "command_templates.h"

#include <EGL/egl.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <linux/fb.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <unordered_map>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

Application::Application() = default;
Application::~Application() { Shutdown(); }

namespace {
MenuRenderer::TelemetryData ConvertTelemetry(const ParsedTelemetry &src, const MenuState &state) {
    MenuRenderer::TelemetryData out{};
    // Signals: map RC RSSI (0-255) to a rough dBm-ish scale
    if (src.has_radio_rssi) {
        float rc_dbm = -100.0f + static_cast<float>(src.rc_rssi) * 0.4f;
        out.rc_signal = rc_dbm;
        out.ground_signal_a = rc_dbm;
        out.ground_signal_b = rc_dbm;
        out.has_rc_signal = true;
    } else {
        out.has_rc_signal = false;
    }

    if (src.has_flight_mode && !src.flight_mode.empty()) {
        out.flight_mode = src.flight_mode;
        out.has_flight_mode = true;
    } else {
        out.has_flight_mode = false;
    }
    out.has_attitude = src.has_attitude;
    out.roll_deg = src.roll_deg;
    out.pitch_deg = src.pitch_deg;

    out.has_gps = src.has_gps;
    if (src.has_gps) {
        out.latitude = src.latitude;
        out.longitude = src.longitude;
        out.altitude_m = src.altitude_m;
        out.home_distance_m = src.home_distance_m;
    }

    out.has_battery = src.has_battery;
    if (src.has_battery) {
        out.pack_voltage = src.batt_voltage_v;
        out.cell_voltage = src.batt_voltage_v > 0.1f ? src.batt_voltage_v / 4.0f : 0.0f; // assume 4S if unknown
    }

    out.has_sky_temp = src.has_sky_temp;
    out.sky_temp_c = src.sky_temp_c;
    out.ground_temp_c = ReadTemperatureC();

    if (src.has_video_metrics) {
        out.bitrate_mbps = src.video_bitrate_mbps;
        out.video_resolution = src.video_resolution;
        out.video_refresh_hz = src.video_refresh_hz;
    } else {
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

bool Application::Initialize(const std::string &font_path, bool use_mock) {
    use_mock_ = use_mock;
    if (!InitFramebuffer(fb_)) {
        std::fprintf(stderr, "[AMLgsMenu] Failed to init framebuffer (/dev/fb0)\n");
        return false;
    }
    if (!InitEgl(fb_)) {
        std::fprintf(stderr, "[AMLgsMenu] Failed to init EGL/GLES\n");
        return false;
    }
    if (!InitInput()) {
        std::fprintf(stderr, "[AMLgsMenu] Failed to init libinput/udev\n");
        return false;
    }
    udp_client_ = std::make_unique<UdpCommandClient>();
    command_templates_.LoadFromFile(command_cfg_path_);
    cmd_runner_ = std::make_unique<CommandExecutor>();
    cmd_runner_->Start();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    io.MouseDrawCursor = false;

    const float base_size = 26.0f; // larger base size for readability
    if (!font_path.empty()) {
        io.Fonts->AddFontFromFileTTF(font_path.c_str(), base_size);
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplOpenGL3_Init("#version 100");

    auto sky_modes = DefaultSkyModes();
    auto ground_modes = LoadHdmiModes("/sys/class/amhdmitx/amhdmitx0/disp_cap");
    if (ground_modes.empty()) {
        ground_modes = sky_modes;
    }

    menu_state_ = std::make_unique<MenuState>(sky_modes, ground_modes);
    LoadConfig();
    ApplyLanguageToImGui(menu_state_->GetLanguage());
    if (!use_mock_) {
        mav_receiver_ = std::make_unique<MavlinkReceiver>();
        mav_receiver_->Start();
    }

    std::function<MenuRenderer::TelemetryData()> provider;
    if (!use_mock_ && mav_receiver_) {
        provider = [this]() {
            return ConvertTelemetry(mav_receiver_->Latest(), *menu_state_);
        };
    }

    renderer_ = std::make_unique<MenuRenderer>(*menu_state_, use_mock_, provider);

    menu_state_->SetOnChangeCallback([this](MenuState::SettingType type) {
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
            SaveConfigValue("ground_res", menu_state_->GroundModes()[menu_state_->GroundModeIndex()].label);
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
        default:
            break;
        }
    });

    running_ = true;
    initialized_ = true;
    last_frame_time_ = std::chrono::steady_clock::now();
    return true;
}

void Application::Run() {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(fb_.width), static_cast<float>(fb_.height));
    io.MousePos = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

    uint64_t frame_counter = 0;
    auto last_log = std::chrono::steady_clock::now();

    while (running_ && !menu_state_->ShouldExit()) {
        ProcessInput(running_);
        UpdateDeltaTime();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        renderer_->Render(running_);

        ImGui::Render();
        glViewport(0, 0, fb_.width, fb_.height);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        auto before_swap = std::chrono::steady_clock::now();
        if (!eglSwapBuffers(egl_display_, egl_surface_)) {
            std::fprintf(stderr, "[AMLgsMenu] eglSwapBuffers failed, stopping loop\n");
            running_ = false;
        }

        ++frame_counter;
        auto now = std::chrono::steady_clock::now();
        auto ms_since_log = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
        if (ms_since_log >= 30000) { // log every 30s to reduce noise
            auto ms_swap = std::chrono::duration_cast<std::chrono::milliseconds>(now - before_swap).count();
            std::fprintf(stdout, "[AMLgsMenu] Frame %llu swap done (swap ms=%lld)\n",
                         static_cast<unsigned long long>(frame_counter),
                         static_cast<long long>(ms_swap));
            std::fflush(stdout);
            last_log = now;
        }
    }
}

void Application::Shutdown() {
    if (!initialized_) {
        return;
    }

    initialized_ = false;
    running_ = false;

    if (mav_receiver_) {
        mav_receiver_->Stop();
        mav_receiver_.reset();
    }
    if (cmd_runner_) {
        cmd_runner_->Stop();
        cmd_runner_.reset();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (egl_context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display_, egl_context_);
        egl_context_ = EGL_NO_CONTEXT;
    }
    if (egl_surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }
    if (li_ctx_) {
        libinput_unref(li_ctx_);
        li_ctx_ = nullptr;
    }
    if (udev_ctx_) {
        udev_unref(udev_ctx_);
        udev_ctx_ = nullptr;
    }
    if (fb_.fd >= 0) {
        close(fb_.fd);
        fb_.fd = -1;
    }
}

void Application::ApplyChannel() {
    const auto &chs = menu_state_->Channels();
    if (chs.empty()) return;
    int ch = chs[menu_state_->ChannelIndex()];
    if (udp_client_) {
        std::unordered_map<std::string, std::string> vars{{"CHANNEL", std::to_string(ch)}};
        auto cmd = command_templates_.Render("remote", "channel", vars);
        if (!cmd.empty() && !udp_client_->Send(cmd, false)) {
            std::fprintf(stderr, "[AMLgsMenu] Failed to send channel command\n");
        }
    }
    ApplyLocalMonitorChannel(ch);
}

void Application::ApplyBandwidth() {
    int bw = (menu_state_->BandwidthIndex() == 0) ? 10 : (menu_state_->BandwidthIndex() == 1 ? 20 : 40);
    if (udp_client_) {
        std::unordered_map<std::string, std::string> vars{
            {"BANDWIDTH", std::to_string(bw)}};
        auto cmd = command_templates_.Render("remote", "bandwidth", vars);
        if (!cmd.empty() && !udp_client_->Send(cmd, false)) {
            std::fprintf(stderr, "[AMLgsMenu] Failed to send bandwidth command\n");
        }
    }
}

void Application::ApplySkyMode() {
    const auto &sky_modes = menu_state_->SkyModes();
    if (sky_modes.empty()) return;
    VideoMode mode = sky_modes[menu_state_->SkyModeIndex()];
    if (udp_client_) {
        std::unordered_map<std::string, std::string> vars{
            {"WIDTH", std::to_string(mode.width)},
            {"HEIGHT", std::to_string(mode.height)},
            {"FPS", std::to_string(mode.refresh ? mode.refresh : 60)}};
        auto cmd = command_templates_.Render("remote", "sky_mode", vars);
        if (!cmd.empty() && !udp_client_->Send(cmd, false)) {
            std::fprintf(stderr, "[AMLgsMenu] Failed to send sky mode command\n");
        }
    }
}

void Application::ApplyBitrate() {
    const auto &bitrates = menu_state_->Bitrates();
    if (bitrates.empty()) return;
    int br_mbps = bitrates[menu_state_->BitrateIndex()];
    int br_kbps = br_mbps * 1024; // CLI expects kbps; e.g. 2 -> 2048
    if (udp_client_) {
        std::unordered_map<std::string, std::string> vars{
            {"BITRATE_KBPS", std::to_string(br_kbps)}};
        auto cmd = command_templates_.Render("remote", "bitrate", vars);
        if (!cmd.empty() && !udp_client_->Send(cmd, false)) {
            std::fprintf(stderr, "[AMLgsMenu] Failed to send bitrate command\n");
        }
    }
}

void Application::ApplySkyPower() {
    const auto &powers = menu_state_->PowerLevels();
    if (powers.empty()) return;
    int p = powers[menu_state_->SkyPowerIndex()];
    if (udp_client_) {
        int tx_pwr = p * 50;
        std::unordered_map<std::string, std::string> vars{
            {"POWER", std::to_string(p)},
            {"TXPOWER", std::to_string(tx_pwr)}};
        auto cmd = command_templates_.Render("remote", "sky_power", vars);
        if (!cmd.empty() && !udp_client_->Send(cmd, false)) {
            std::fprintf(stderr, "[AMLgsMenu] Failed to send tx power command\n");
        }
    }
}

void Application::ApplyGroundPower() {
    const auto &powers = menu_state_->PowerLevels();
    if (powers.empty()) return;
    int p = powers[menu_state_->GroundPowerIndex()];
    ApplyLocalMonitorPower(p);
}

void Application::ApplyLocalMonitorChannel(int channel) {
    if (channel <= 0) return;
    int bw_mhz = (menu_state_->BandwidthIndex() == 0) ? 10 : (menu_state_->BandwidthIndex() == 1 ? 20 : 40);
    std::string bw_suffix;
    if (bw_mhz == 20) bw_suffix = " HT20";
    else if (bw_mhz == 40) bw_suffix = " HT40+";
    std::unordered_map<std::string, std::string> vars{
        {"CHANNEL", std::to_string(channel)},
        {"BW_SUFFIX", bw_suffix}};
    auto cmd = command_templates_.Render("local", "monitor_channel", vars);
    if (!cmd.empty() && cmd_runner_) {
        cmd_runner_->Enqueue(cmd);
    }
}

void Application::ApplyLocalMonitorPower(int power_level) {
    if (power_level <= 0) return;
    int tx_pwr = power_level * 50;
    std::unordered_map<std::string, std::string> vars{
        {"POWER", std::to_string(power_level)},
        {"TXPOWER", std::to_string(tx_pwr)}};
    auto cmd = command_templates_.Render("local", "monitor_power", vars);
    if (!cmd.empty() && cmd_runner_) {
        cmd_runner_->Enqueue(cmd);
    }
}

bool Application::InitFramebuffer(FbContext &fb) {
    fb.fd = open("/dev/fb0", O_RDWR);
    if (fb.fd < 0) {
        std::perror("[AMLgsMenu] open(/dev/fb0)");
        return false;
    }
    fb_var_screeninfo vinfo{};
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        std::perror("[AMLgsMenu] ioctl(FBIOGET_VSCREENINFO)");
        return false;
    }
    fb.width = static_cast<int>(vinfo.xres);
    fb.height = static_cast<int>(vinfo.yres);
    return true;
}

bool Application::InitEgl(const FbContext &fb) {
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
    if (egl_display_ == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "[AMLgsMenu] eglGetDisplay failed\n");
        return false;
    }
    if (!eglInitialize(egl_display_, &major, &minor)) {
        std::fprintf(stderr, "[AMLgsMenu] eglInitialize failed\n");
        return false;
    }
    if (!eglChooseConfig(egl_display_, attr, &egl_config_, 1, &num_configs) || num_configs == 0) {
        std::fprintf(stderr, "[AMLgsMenu] eglChooseConfig failed\n");
        return false;
    }
    EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx_attr);
    if (egl_context_ == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "[AMLgsMenu] eglCreateContext failed\n");
        return false;
    }

    fbdev_window native_window{fb.width, fb.height};
    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, &native_window, nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
        std::fprintf(stderr, "[AMLgsMenu] eglCreateWindowSurface failed\n");
        return false;
    }
    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
        std::fprintf(stderr, "[AMLgsMenu] eglMakeCurrent failed\n");
        return false;
    }
    // Restore vsync (swap interval 1); adjust if target platform requires immediate swaps.
    eglSwapInterval(egl_display_, 1);
    return true;
}

bool Application::InitInput() {
    udev_ctx_ = udev_new();
    if (!udev_ctx_) {
        std::fprintf(stderr, "[AMLgsMenu] udev_new failed\n");
        return false;
    }
    static const libinput_interface iface = {
        &Application::LibinputOpen,
        &Application::LibinputClose,
    };
    li_ctx_ = libinput_udev_create_context(&iface, nullptr, udev_ctx_);
    if (!li_ctx_) {
        std::fprintf(stderr, "[AMLgsMenu] libinput_udev_create_context failed\n");
        return false;
    }
    if (libinput_udev_assign_seat(li_ctx_, "seat0") != 0) {
        std::fprintf(stderr, "[AMLgsMenu] libinput_udev_assign_seat failed\n");
        return false;
    }
    return true;
}

void Application::ProcessInput(bool &running) {
    if (!li_ctx_) return;
    pollfd pfd{};
    pfd.fd = libinput_get_fd(li_ctx_);
    pfd.events = POLLIN;
    poll(&pfd, 1, 1);
    if (libinput_dispatch(li_ctx_) != 0) return;
    libinput_event *event = nullptr;
    while ((event = libinput_get_event(li_ctx_)) != nullptr) {
        HandleLibinputEvent(event, running);
        libinput_event_destroy(event);
    }
}

void Application::HandleLibinputEvent(struct libinput_event *event, bool &running) {
    ImGuiIO &io = ImGui::GetIO();
    switch (libinput_event_get_type(event)) {
    case LIBINPUT_EVENT_POINTER_MOTION: {
        auto *m = libinput_event_get_pointer_event(event);
        io.MousePos.x += static_cast<float>(libinput_event_pointer_get_dx(m));
        io.MousePos.y += static_cast<float>(libinput_event_pointer_get_dy(m));
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON: {
        auto *b = libinput_event_get_pointer_event(event);
        uint32_t button = libinput_event_pointer_get_button(b);
        auto state = libinput_event_pointer_get_button_state(b);
        bool pressed = (state == LIBINPUT_BUTTON_STATE_PRESSED);
        if (button == BTN_LEFT) io.MouseDown[0] = pressed;
        if (button == BTN_RIGHT && pressed) menu_state_->ToggleMenuVisibility();
        break;
    }
    case LIBINPUT_EVENT_POINTER_AXIS: {
        auto *a = libinput_event_get_pointer_event(event);
        if (libinput_event_pointer_has_axis(a, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
            double v = libinput_event_pointer_get_axis_value(a, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
            const float kScrollScale = 0.35f; // slow down scroll for combos
            io.MouseWheel += static_cast<float>(-v) * kScrollScale;
        }
        break;
    }
    case LIBINPUT_EVENT_KEYBOARD_KEY: {
        auto *k = libinput_event_get_keyboard_event(event);
        uint32_t key = libinput_event_keyboard_get_key(k);
        auto state = libinput_event_keyboard_get_key_state(k);
        if (state == LIBINPUT_KEY_STATE_PRESSED) {
            if (key == KEY_X || key == BTN_WEST) {
                menu_state_->ToggleMenuVisibility();
            }
            if (key == KEY_ESC) {
                running = false;
            }
        }
        break;
    }
    default:
        break;
    }
    io.MousePos.x = std::max(0.0f, std::min(io.MousePos.x, static_cast<float>(fb_.width)));
    io.MousePos.y = std::max(0.0f, std::min(io.MousePos.y, static_cast<float>(fb_.height)));
}

void Application::UpdateDeltaTime() {
    ImGuiIO &io = ImGui::GetIO();
    auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::chrono::duration<float>(now - last_frame_time_).count();
    last_frame_time_ = now;
}

void Application::LoadConfig() {
    std::ifstream file(config_path_);
    if (!file.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
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
    if (it_ch != config_kv_.end()) {
        int ch = std::stoi(it_ch->second);
        int idx = FindChannelIndex(ch);
        if (idx >= 0) menu_state_->SetChannelIndex(idx);
    }
    auto it_bw = config_kv_.find("bandwidth");
    if (it_bw != config_kv_.end()) {
        int bw = std::stoi(it_bw->second);
        if (bw == 10) menu_state_->SetBandwidthIndex(0);
        else if (bw == 20) menu_state_->SetBandwidthIndex(1);
        else if (bw == 40) menu_state_->SetBandwidthIndex(2);
    }
    auto it_power = config_kv_.find("driver_txpower_override");
    if (it_power != config_kv_.end()) {
        int p = std::stoi(it_power->second);
        int idx = FindPowerIndex(p);
        if (idx >= 0) {
            menu_state_->SetGroundPowerIndex(idx);
            menu_state_->SetSkyPowerIndex(idx);
        }
    }
    // support both correct key and legacy typo
    auto it_res = config_kv_.find("ground_res");
    if (it_res == config_kv_.end()) {
        it_res = config_kv_.find("groud_res");
    }
    if (it_res != config_kv_.end()) {
        std::string label = it_res->second;
        while (!label.empty() && (label.back() == '*' || label.back() == ' ' || label.back() == '\t'))
            label.pop_back();
        int idx = FindGroundModeIndex(label);
        if (idx >= 0) menu_state_->SetGroundModeIndex(idx);
    }
    auto it_lang = config_kv_.find("lang");
    if (it_lang != config_kv_.end()) {
        std::string v = it_lang->second;
        for (auto &c : v) c = static_cast<char>(tolower(c));
        if (v == "en") menu_state_->SetLanguage(MenuState::Language::EN);
        else if (v == "cn") menu_state_->SetLanguage(MenuState::Language::CN);
    }
}

void Application::SaveConfigValue(const std::string &key, const std::string &value) {
    config_kv_[key] = value;
    std::ofstream file(config_path_, std::ios::trunc);
    if (!file.is_open()) return;
    for (const auto &kv : config_kv_) {
        file << kv.first << "=" << kv.second << "\n";
    }
}

int Application::FindChannelIndex(int channel_val) const {
    const auto &chs = menu_state_->Channels();
    for (size_t i = 0; i < chs.size(); ++i) {
        if (chs[i] == channel_val) return static_cast<int>(i);
    }
    return -1;
}

int Application::FindPowerIndex(int power_val) const {
    const auto &pwr = menu_state_->PowerLevels();
    for (size_t i = 0; i < pwr.size(); ++i) {
        if (pwr[i] == power_val) return static_cast<int>(i);
    }
    return -1;
}

int Application::FindGroundModeIndex(const std::string &label) const {
    const auto &modes = menu_state_->GroundModes();
    for (size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].label == label) return static_cast<int>(i);
    }
    return -1;
}

void Application::ApplyLanguageToImGui(MenuState::Language lang) {
    // Font loading already follows -t 参数或默认字体，不在语言切换时重建，避免丢失用户指定字体。
    (void)lang;
}

int Application::LibinputOpen(const char *path, int flags, void *user_data) {
    (void)user_data;
    return open(path, flags | O_NONBLOCK);
}

void Application::LibinputClose(int fd, void *user_data) {
    (void)user_data;
    if (fd >= 0) {
        close(fd);
    }
}
