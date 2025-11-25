#pragma once

#include "menu_state.h"

#include "imgui.h"
#include <vector>

class MenuRenderer {
public:
    explicit MenuRenderer(MenuState &state);
    ~MenuRenderer();

    void Render(bool &running_flag);
    struct TelemetryData {
        float ground_signal_a = 0.0f;
        float ground_signal_b = 0.0f;
        float rc_signal = 0.0f;
        bool has_rc_signal = true;
        bool has_flight_mode = true;
        bool has_attitude = true;
        bool has_gps = true;
        bool has_battery = true;
        bool has_sky_temp = true;
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
    };

private:
    void DrawOsd(const ImGuiViewport *viewport, const TelemetryData &data) const;
    void DrawMenu(const ImGuiViewport *viewport, bool &running_flag);
    bool LoadIcon(const char *path, ImTextureID &out_id, int &out_w, int &out_h);

    MenuState &state_;
    TelemetryData cached_telemetry_{};
    float last_osd_update_time_ = -1.0f;
    ImTextureID icon_antenna_{};
    ImTextureID icon_batt_cell_{};
    ImTextureID icon_batt_pack_{};
    ImTextureID icon_gps_{};
    ImTextureID icon_monitor_{};
};
