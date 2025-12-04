#pragma once

#include "menu_renderer.h"
#include "menu_state.h"
#include "signal_monitor.h"
#include "udp_command_client.h"
#include "command_templates.h"
#include "command_executor.h"
#include "terminal.h"
#include "custom_osd_monitor.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <libinput.h>
#include <libudev.h>

#include <chrono>
#include <unordered_map>
#include <memory>
#include <regex>
#include <string>
#include <iostream>
#include <functional>

struct ImFont;

class Application
{
public:
    Application();
    ~Application();

    bool Initialize(const std::string &font_path = "", bool use_mock = false,
                    const std::string &terminal_font_path = "");
    void SetCommandCfgPath(const std::string &path) { command_cfg_path_ = path; }
    void Run();
    void Shutdown();

private:
    struct FbContext
    {
        int fd = -1;
        int width = 0;
        int height = 0;
    };

    bool InitFramebuffer(FbContext &fb);
    bool InitEgl(const FbContext &fb);
    bool InitInput();
    static int LibinputOpen(const char *path, int flags, void *user_data);
    static void LibinputClose(int fd, void *user_data);
    void ProcessInput(bool &running);
    void HandleLibinputEvent(struct libinput_event *event, bool &running);
    void UpdateDeltaTime();

public:
    void SetConfigPath(const std::string &path) { config_path_ = path; }

private:
    void LoadConfig();
    void SaveConfigValue(const std::string &key, const std::string &value);
    int FindChannelIndex(int channel_val) const;
    int FindPowerIndex(int power_val) const;
    int FindGroundModeIndex(const std::string &label) const;
    void ApplyLanguageToImGui(MenuState::Language lang);
    void ApplyChannel();
    void ApplyBandwidth();
    void ApplySkyMode();
    void ApplyBitrate();
    void ApplySkyPower();
    void ApplyGroundPower();
    void ApplyLocalMonitorChannel(int channel);
    void ApplyLocalMonitorPower(int power_level);
    std::string command_cfg_path_ = "/flash/command.cfg";
    void UpdateCommandRunner(bool menu_visible);

    FbContext fb_{};
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLConfig egl_config_ = nullptr;
    struct libinput *li_ctx_ = nullptr;
    struct udev *udev_ctx_ = nullptr;
    bool running_ = false;
    bool initialized_ = false;
    std::chrono::steady_clock::time_point last_frame_time_{};

    std::unique_ptr<MenuState> menu_state_;
    std::unique_ptr<MenuRenderer> renderer_;
    std::unique_ptr<class MavlinkReceiver> mav_receiver_;
    std::unique_ptr<UdpCommandClient> udp_client_;
    CommandTemplates command_templates_;
    std::unique_ptr<CommandExecutor> cmd_runner_;
    std::unique_ptr<SignalMonitor> signal_monitor_;
    std::unique_ptr<CustomOsdMonitor> custom_osd_monitor_;
    bool use_mock_ = false;
    bool command_runner_active_ = false;
    std::unique_ptr<Terminal> terminal_;
    ImFont *ui_font_ = nullptr;
    ImFont *terminal_font_ = nullptr;

    std::unordered_map<std::string, std::string> config_kv_;
    std::string config_path_ = "/flash/wfb.conf";
};
