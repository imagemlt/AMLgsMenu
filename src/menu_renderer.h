#pragma once

#include "menu_state.h"

#include "imgui.h"

#include <functional>
#include <chrono>
#include <string>
#include <vector>

class MenuRenderer
{
public:
    struct TelemetryData
    {
        float ground_signal_a = 0.0f;
        float ground_signal_b = 0.0f;
        float rc_signal = 0.0f;
        bool has_rc_signal = false;
        bool has_flight_mode = false;
        bool has_attitude = false;
        bool has_gps = false;
        bool has_battery = false;
        bool has_sky_temp = false;
        std::string flight_mode;
        double latitude = 0.0;
        double longitude = 0.0;
        float altitude_m = 0.0f;
        float home_distance_m = 0.0f;
        float bitrate_mbps = 0.0f;
        std::string video_resolution;
        int video_refresh_hz = 0;
        float cell_voltage = 0.0f;
        float pack_voltage = 0.0f;
        float sky_temp_c = 0.0f;
        float ground_temp_c = 0.0f;
        float roll_deg = 0.0f;
        float pitch_deg = 0.0f;
        float ground_batt_percent = 0.0f;
        bool has_ground_batt = false;
    };

    MenuRenderer(MenuState &state, bool &use_mock, std::function<TelemetryData(TelemetryData)> provider,
                 std::function<void()> toggle_terminal = {}, std::function<bool()> terminal_visible = {});
    ~MenuRenderer();

    void Render(bool &running_flag);

private:
    void DrawOsd(const ImGuiViewport *viewport, const TelemetryData &data) const;
    void DrawMenu(const ImGuiViewport *viewport, bool &running_flag);
    bool LoadIcon(const char *path, ImTextureID &out_id, int &out_w, int &out_h);

    MenuState &state_;
    // Application &application_;
    bool use_mock_ = true;
    std::function<TelemetryData(TelemetryData)> telemetry_provider_;
    TelemetryData cached_telemetry_{};
    float last_osd_update_time_ = -1.0f;
    std::chrono::steady_clock::time_point last_osd_tp_{};
    bool has_mavlink_data_ = false;
    ImTextureID icon_antenna_{};
    ImTextureID icon_batt_cell_{};
    ImTextureID icon_batt_pack_{};
    ImTextureID icon_gps_{};
    ImTextureID icon_monitor_{};
    ImTextureID icon_temp_air_{};
    ImTextureID icon_temp_ground_{};
    std::function<void()> toggle_terminal_;
    std::function<bool()> terminal_visible_;
    bool focus_confirm_to_open_ = false;
    bool focus_open_to_boot_ = false;
    bool focus_boot_to_confirm_ = false;
    bool focus_boot_to_open_ = false;
    bool focus_boot_to_recording_ = false;
    int kodi_popup_focus_index_ = 0;
    bool kodi_popup_focus_dirty_ = false;
    int pending_high_refresh_index_ = -1;
    std::string pending_high_refresh_label_;
    bool high_refresh_popup_pending_ = false;
    bool high_refresh_persist_popup_pending_ = false;
};
