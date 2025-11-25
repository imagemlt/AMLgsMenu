#include "application.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "video_mode.h"

#include <EGL/egl.h>
#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <linux/fb.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

Application::Application() = default;
Application::~Application() { Shutdown(); }

bool Application::Initialize(const std::string &font_path) {
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
    renderer_ = std::make_unique<MenuRenderer>(*menu_state_);

    menu_state_->SetOnChangeCallback([this](MenuState::SettingType type) {
        switch (type) {
        case MenuState::SettingType::Channel:
            SaveConfigValue("channel", std::to_string(menu_state_->Channels()[menu_state_->ChannelIndex()]));
            break;
        case MenuState::SettingType::Bandwidth:
            SaveConfigValue("bandwidth", std::to_string((menu_state_->BandwidthIndex() == 0) ? 10 :
                                                        (menu_state_->BandwidthIndex() == 1) ? 20 : 40));
            break;
        case MenuState::SettingType::GroundMode:
            SaveConfigValue("groud_res", menu_state_->GroundModes()[menu_state_->GroundModeIndex()].label);
            break;
        case MenuState::SettingType::GroundPower:
            SaveConfigValue("driver_txpower_override", std::to_string(menu_state_->PowerLevels()[menu_state_->GroundPowerIndex()]));
            break;
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
        eglSwapBuffers(egl_display_, egl_surface_);
    }
}

void Application::Shutdown() {
    if (!initialized_) {
        return;
    }

    initialized_ = false;
    running_ = false;

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
    auto it_res = config_kv_.find("groud_res");
    if (it_res != config_kv_.end()) {
        int idx = FindGroundModeIndex(it_res->second);
        if (idx >= 0) menu_state_->SetGroundModeIndex(idx);
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
