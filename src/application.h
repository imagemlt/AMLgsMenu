#pragma once

#include "menu_renderer.h"
#include "menu_state.h"
#include "signal_monitor.h"
#include "command_transport.h"
#include "command_templates.h"
#include "command_executor.h"
#include "terminal.h"
#include "telemetry_worker.h"

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
#include <thread>
#include <mutex>
#include <memory>

struct JoystickDevice
{
    int fd = -1;
    std::string path;
    std::vector<int16_t> axes;
    std::vector<uint8_t> buttons;
    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;
};

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
    void SaveConfig();

private:
    struct RemoteStateSnapshot
    {
        bool has_channel = false;
        int channel = 0;
        bool has_bandwidth = false;
        int bandwidth = 0;
        bool has_power = false;
        int power = 0;
        bool has_bitrate = false;
        int bitrate_kbps = 0;
        bool has_sky_mode = false;
        int width = 0;
        int height = 0;
        int fps = 0;
    };

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
    void PollJoysticks(bool &running);
    void ScanJoysticks();
    void CloseJoysticks();
    void RemoveJoystick(size_t index);
    void HandleJoystickButton(int button, bool pressed);
    void HandleJoystickAxis(JoystickDevice &dev, int axis, int16_t value);
    void UpdateDeltaTime();
    bool configUpdated_ = false;

public:
    void SetConfigPath(const std::string &path) { config_path_ = path; }

private:
    void LoadConfig();
    void SaveConfigValue(const std::string &key, const std::string &value);
    int FindChannelIndex(int channel_val) const;
    int FindPowerIndex(int power_val) const;
    int FindGroundModeIndex(const std::string &label) const;
    int FindSkyModeIndex(int width, int height, int refresh) const;
    int FindBitrateIndex(int bitrate_mbps) const;
    void StartRemoteSync();
    void DrainRemoteState();
    void ApplyRemoteStateSnapshot(const RemoteStateSnapshot &snapshot);
    bool CollectRemoteState(RemoteStateSnapshot &snapshot,
                            const std::shared_ptr<CommandTransport> &transport);
    bool QueryRemoteValue(const std::string &key, std::string &out,
                          const std::shared_ptr<CommandTransport> &transport);
    void ApplyLanguageToImGui(MenuState::Language lang);
    void ApplyChannel();
    void ApplyBandwidth();
    void ApplySkyMode();
    void ApplyGroundDisplayMode(const std::string &label);
    void ApplyBitrate();
    void ApplySkyPower();
    void ApplyGroundPower();
    void ApplyLocalMonitorChannel(int channel);
    void ApplyLocalMonitorPower(int power_level);
    bool SendRecordingCommand(bool enable);
    void RebuildTransport(MenuState::FirmwareType type);
    std::shared_ptr<CommandTransport> AcquireTransport() const;
    void RestartRemoteSync();
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
    CommandTemplates command_templates_;
    std::unique_ptr<CommandExecutor> cmd_runner_;
    std::unique_ptr<SignalMonitor> signal_monitor_;
    std::unique_ptr<TelemetryWorker> telemetry_worker_;
    std::vector<JoystickDevice> joysticks_;
    bool use_mock_ = false;
    bool command_runner_active_ = false;
    std::unique_ptr<Terminal> terminal_;
    ImFont *ui_font_ = nullptr;
    ImFont *terminal_font_ = nullptr;
    MenuState::FirmwareType firmware_mode_ = MenuState::FirmwareType::CCEdition;
    const std::string ssh_host_ = "10.5.0.10";
    const uint16_t ssh_port_ = 22;
    const std::string ssh_user_ = "root";
    const std::string ssh_password_ = "12345";
    std::shared_ptr<CommandTransport> transport_;
    mutable std::mutex transport_mutex_;

    std::unordered_map<std::string, std::string> config_kv_;
    std::string config_path_ = "/flash/wfb.conf";
    std::thread remote_sync_thread_;
    std::mutex remote_state_mutex_;
    RemoteStateSnapshot pending_remote_state_{};
    bool remote_sync_ready_ = false;
    std::chrono::steady_clock::time_point last_js_scan_{};
};
