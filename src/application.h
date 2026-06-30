#pragma once

#include "app_config.h"
#include "ap_tcp_client.h"
#include "display_platform.h"
#include "input_controller.h"
#include "link_settings_controller.h"
#include "menu_renderer.h"
#include "menu_state.h"
#include "remote_sync_service.h"
#include "splash_player.h"
#include "signal_monitor.h"
#include "command_transport.h"
#include "command_templates.h"
#include "command_executor.h"
#include "terminal.h"
#include "telemetry_worker.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <chrono>
#include <atomic>
#include <memory>
#include <regex>
#include <string>
#include <iostream>
#include <functional>
#include <thread>
#include <mutex>

struct ImFont;

class Application
{
public:
    Application();
    ~Application();

    bool Initialize(const std::string &font_path = "", bool use_mock = false,
                    const std::string &terminal_font_path = "");
    void SetCommandCfgPath(const std::string &path) { command_cfg_path_ = path; }
    void SetMavlinkPort(uint16_t port) { mavlink_port_ = port; }
    void Run();
    void Shutdown();
    void SaveConfig();

private:
    void UpdateDeltaTime();
public:
    void SetConfigPath(const std::string &path) { config_.SetConfigPath(path); }

private:
    void LoadConfig();
    bool EnsureWritableConfig();
    void SaveConfigValue(const std::string &key, const std::string &value);
    void StartRemoteSync();
    void ApplyLanguageToImGui(MenuState::Language lang);
    bool UpdateWifiClientConfig(const std::string &ssid, const std::string &password);
    void StartFirmwareUpdate(const std::string &path);
    int UpdateStatus() const { return update_status_.load(); }
    void RequestReboot();
    void RebuildTransport(MenuState::FirmwareType type, bool ap_mode);
    static std::string DetectApGateway(const std::string &prefer_iface = "");
    std::shared_ptr<CommandTransport> AcquireTransport() const;
    void RestartRemoteSync();
    bool EnsureCommandRunnerForRemoteSync();
    std::string command_cfg_path_ = "/flash/command.cfg";
    void UpdateCommandRunner(bool menu_visible);
    uint16_t mavlink_port_ = 14452;

    DisplayPlatform display_platform_;
    bool running_ = false;
    bool initialized_ = false;
    std::chrono::steady_clock::time_point last_frame_time_{};

    std::unique_ptr<MenuState> menu_state_;
    std::unique_ptr<MenuRenderer> renderer_;
    std::unique_ptr<class MavlinkReceiver> mav_receiver_;
    std::unique_ptr<LinkSettingsController> settings_controller_;
    CommandTemplates command_templates_;
    std::unique_ptr<CommandExecutor> cmd_runner_;
    std::unique_ptr<SignalMonitor> signal_monitor_;
    std::unique_ptr<TelemetryWorker> telemetry_worker_;
    std::unique_ptr<InputController> input_controller_;
    bool use_mock_ = false;
    bool command_runner_active_ = false;
    std::unique_ptr<Terminal> terminal_;
    ImFont *ui_font_ = nullptr;
    ImFont *terminal_font_ = nullptr;
    SplashPlayer splash_player_;
    std::unique_ptr<RemoteSyncService> remote_sync_;
    MenuState::FirmwareType firmware_mode_ = MenuState::FirmwareType::CCEdition;
    const std::string ssh_host_ = "10.5.0.10";
    const uint16_t ssh_port_ = 22;
    const std::string ssh_user_ = "root";
    std::string ssh_password_ = "12345";
    std::shared_ptr<CommandTransport> transport_;
    mutable std::mutex transport_mutex_;

    AppConfig config_;
    bool ap_mode_ = false;
    std::string ap_gateway_;
    std::atomic<int> update_status_{0};
};
