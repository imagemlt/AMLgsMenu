#include "menu_renderer.h"

#include "imgui.h"
#include "video_mode.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <string>

struct MenuRenderer::TelemetryData {
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

static MenuRenderer::TelemetryData BuildMockTelemetry(const MenuState &state) {
    const float t = static_cast<float>(ImGui::GetTime());

    MenuRenderer::TelemetryData data{};

    data.ground_signal_a = -60.0f + 5.0f * std::sin(t * 0.8f);
    data.ground_signal_b = -62.0f + 6.0f * std::cos(t * 0.65f);
    data.rc_signal = -55.0f + 4.0f * std::sin(t * 1.1f);
    data.has_rc_signal = true;
    data.has_flight_mode = true;
    data.has_attitude = true;
    data.has_gps = true;
    data.has_battery = true;
    data.has_sky_temp = true;

    const char *modes[] = {"HORIZON", "ANGLE", "ACRO", "RTH"};
    data.flight_mode = modes[static_cast<int>(t / 4.0f) % 4];

    data.latitude = 37.773 + 0.001 * std::sin(t * 0.15f);
    data.longitude = -122.431 + 0.0015 * std::cos(t * 0.12f);
    data.altitude_m = 120.0f + 12.0f * std::sin(t * 0.35f);
    data.home_distance_m = 250.0f + 35.0f * std::cos(t * 0.45f);

    const auto &ground_modes = state.GroundModes();
    VideoMode mode = ground_modes.empty() ? VideoMode{"1920x1080 @ 60Hz", 1920, 1080, 60}
                                          : ground_modes[state.GroundModeIndex() % ground_modes.size()];
    std::ostringstream res;
    res << mode.width << "x" << mode.height;
    data.video_resolution = res.str();
    data.video_refresh_hz = mode.refresh ? mode.refresh : 60;
    data.bitrate_mbps = std::max(1.0f, 6.0f + 2.0f * std::sin(t * 0.4f));

    data.cell_voltage = 3.8f + 0.12f * std::sin(t * 0.6f);
    data.pack_voltage = data.cell_voltage * 4.0f + 0.4f * std::cos(t * 0.3f);

    data.sky_temp_c = 45.0f + 5.0f * std::sin(t * 0.22f);
    data.ground_temp_c = 40.0f + 4.0f * std::cos(t * 0.18f);
    data.roll_deg = 10.0f * std::sin(t * 0.6f);
    data.pitch_deg = 5.0f * std::cos(t * 0.5f);

    return data;
}

MenuRenderer::MenuRenderer(MenuState &state) : state_(state) {}

void MenuRenderer::Render(bool &running_flag) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const TelemetryData telemetry = BuildMockTelemetry(state_);

    DrawOsd(viewport, telemetry);

    if (state_.MenuVisible()) {
        DrawMenu(viewport, running_flag);
    }
}

void MenuRenderer::DrawOsd(const ImGuiViewport *viewport, const TelemetryData &data) const {
    ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
    const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f);

    const float icon_size = 18.0f;
    const float icon_gap = 6.0f;
    const ImU32 text_shadow = IM_COL32(0, 0, 0, 180);

    auto draw_icon = [&](ImVec2 pos) {
        ImU32 fill = IM_COL32(80, 120, 200, 180);
        ImU32 border = IM_COL32(180, 210, 255, 220);
        draw_list->AddRectFilled(pos, ImVec2(pos.x + icon_size, pos.y + icon_size), fill, 3.0f);
        draw_list->AddRect(pos, ImVec2(pos.x + icon_size, pos.y + icon_size), border, 3.0f, 0, 1.5f);
    };

    auto draw_horizon = [&](float roll_deg, float pitch_deg) {
        const float line_half_len = viewport->Size.x * 0.25f;
        const float rad = roll_deg * 3.1415926f / 180.0f;
        const float cosr = std::cos(rad);
        const float sinr = std::sin(rad);
        ImVec2 left(-line_half_len, 0.0f);
        ImVec2 right(line_half_len, 0.0f);
        auto rotate = [&](const ImVec2 &p) {
            return ImVec2(p.x * cosr - p.y * sinr, p.x * sinr + p.y * cosr);
        };
        const float pitch_offset = pitch_deg * 4.0f; // pixels per degree, tweak as needed
        ImVec2 p1 = rotate(left);
        ImVec2 p2 = rotate(right);
        p1.x += center.x;
        p2.x += center.x;
        p1.y += center.y + pitch_offset;
        p2.y += center.y + pitch_offset;
        draw_list->AddLine(p1, p2, IM_COL32(255, 255, 255, 255), 2.0f);
    };
    if (data.has_attitude) {
        draw_horizon(data.roll_deg, data.pitch_deg);
    } else {
        draw_horizon(0.0f, 0.0f);
    }

    auto draw_centered_text = [&](ImVec2 pos, const std::string &text, ImU32 color) {
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        ImVec2 icon_pos(pos.x - size.x * 0.5f - icon_size - icon_gap, pos.y);
        draw_icon(icon_pos);
        ImVec2 text_pos(icon_pos.x + icon_size + icon_gap, pos.y);
        // Shadow
        draw_list->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), text_shadow, text.c_str());
        draw_list->AddText(text_pos, color, text.c_str());
    };

    auto draw_centered_text_no_icon = [&](ImVec2 pos, const std::string &text, ImU32 color) {
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        ImVec2 text_pos(pos.x - size.x * 0.5f, pos.y);
        draw_list->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), text_shadow, text.c_str());
        draw_list->AddText(text_pos, color, text.c_str());
    };

    std::ostringstream signal;
    signal << "GND A: " << static_cast<int>(data.ground_signal_a) << " dBm  |  "
           << "GND B: " << static_cast<int>(data.ground_signal_b) << " dBm";
    if (data.has_rc_signal) {
        signal << "  |  RC: " << static_cast<int>(data.rc_signal) << " dBm";
    }
    draw_centered_text(ImVec2(center.x, center.y - viewport->Size.y * 0.12f),
                       signal.str(), IM_COL32(255, 255, 255, 230));

    if (data.has_flight_mode) {
        draw_centered_text_no_icon(ImVec2(center.x, center.y - viewport->Size.y * 0.25f),
                                   "Flight Mode: " + data.flight_mode, IM_COL32(0, 200, 255, 255));
    }

    ImGuiWindowFlags overlay_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;

    auto icon_text_line = [&](const char *text) {
        ImVec2 start = ImGui::GetCursorScreenPos();
        draw_icon(start);
        ImGui::Dummy(ImVec2(icon_size + icon_gap, icon_size));
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(start.x + icon_size + icon_gap, start.y));
        ImGui::TextUnformatted(text);
    };

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 16.0f,
                                   viewport->Pos.y + viewport->Size.y - 64.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (data.has_gps && ImGui::Begin("OSD_GPS", nullptr, overlay_flags)) {
        char gps_buf[128];
        snprintf(gps_buf, sizeof(gps_buf), "GPS: %.5f, %.5f, %.1fm",
                 data.latitude, data.longitude, data.altitude_m);
        icon_text_line(gps_buf);
        char home_buf[64];
        snprintf(home_buf, sizeof(home_buf), "\u79bb\u5bb6\u8ddd\u79bb: %.1fm", data.home_distance_m);
        icon_text_line(home_buf);
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - 16.0f,
                                   viewport->Pos.y + viewport->Size.y - 48.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("OSD_VIDEO", nullptr, overlay_flags)) {
        char video_buf[128];
        snprintf(video_buf, sizeof(video_buf), "\u89c6\u9891: %.1f Mbps %s @ %dHz",
                 data.bitrate_mbps, data.video_resolution.c_str(), data.video_refresh_hz);
        icon_text_line(video_buf);
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 16.0f, center.y - 24.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (data.has_battery && ImGui::Begin("OSD_BATT", nullptr, overlay_flags)) {
        char cell_buf[32];
        snprintf(cell_buf, sizeof(cell_buf), "\u5355\u8282: %.2fV", data.cell_voltage);
        icon_text_line(cell_buf);
        char pack_buf[32];
        snprintf(pack_buf, sizeof(pack_buf), "\u603b\u7535: %.2fV", data.pack_voltage);
        icon_text_line(pack_buf);
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - 16.0f, center.y - 24.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("OSD_TEMP", nullptr, overlay_flags)) {
        char sky_buf[32];
        snprintf(sky_buf, sizeof(sky_buf), "\u5929\u7a7a\u7aef\u6e29\u5ea6: %.1f\u2103", data.sky_temp_c);
        if (data.has_sky_temp) {
            icon_text_line(sky_buf);
        }
        char ground_buf[32];
        snprintf(ground_buf, sizeof(ground_buf), "\u5730\u9762\u7aef\u6e29\u5ea6: %.1f\u2103", data.ground_temp_c);
        icon_text_line(ground_buf);
    }
    ImGui::End();
}

void MenuRenderer::DrawMenu(const ImGuiViewport *viewport, bool &running_flag) {
    (void)running_flag;
    const ImVec2 menu_size = ImVec2(viewport->Size.x * 0.5f, viewport->Size.y * 0.5f);
    const ImVec2 menu_pos = ImVec2(viewport->Pos.x + viewport->Size.x * 0.25f,
                                   viewport->Pos.y + viewport->Size.y * 0.25f);

    ImGui::SetNextWindowBgAlpha(0.25f);
    ImGui::SetNextWindowPos(menu_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(menu_size, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("GS Control Menu", nullptr, flags)) {
        ImGui::Text("\u65e0\u7ebf\u94fe\u8def\u914d\u7f6e");
        ImGui::Separator();

        const auto &channels = state_.Channels();
        if (ImGui::BeginCombo("\u4fe1\u9053", std::to_string(channels[state_.ChannelIndex()]).c_str())) {
            for (int i = 0; i < static_cast<int>(channels.size()); ++i) {
                bool selected = (state_.ChannelIndex() == i);
                if (ImGui::Selectable(std::to_string(channels[i]).c_str(), selected)) {
                    state_.SetChannelIndex(i);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const auto &bandwidths = state_.Bandwidths();
        if (ImGui::BeginCombo("\u9891\u5bbd", bandwidths[state_.BandwidthIndex()])) {
            for (int i = 0; i < static_cast<int>(bandwidths.size()); ++i) {
                bool selected = (state_.BandwidthIndex() == i);
                if (ImGui::Selectable(bandwidths[i], selected)) {
                    state_.SetBandwidthIndex(i);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const auto &sky_modes = state_.SkyModes();
        if (!sky_modes.empty() && ImGui::BeginCombo("\u5929\u7a7a\u7aef\u5206\u8fa8\u7387/\u5237\u65b0\u7387", FormatVideoModeLabel(sky_modes[state_.SkyModeIndex()]).c_str())) {
            for (int i = 0; i < static_cast<int>(sky_modes.size()); ++i) {
                bool selected = (state_.SkyModeIndex() == i);
                if (ImGui::Selectable(FormatVideoModeLabel(sky_modes[i]).c_str(), selected)) {
                    state_.SetSkyModeIndex(i);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const auto &ground_modes = state_.GroundModes();
        if (!ground_modes.empty() && ImGui::BeginCombo("\u5730\u9762\u7aef\u5206\u8fa8\u7387/\u5237\u65b0\u7387", FormatVideoModeLabel(ground_modes[state_.GroundModeIndex()]).c_str())) {
            for (int i = 0; i < static_cast<int>(ground_modes.size()); ++i) {
                bool selected = (state_.GroundModeIndex() == i);
                if (ImGui::Selectable(FormatVideoModeLabel(ground_modes[i]).c_str(), selected)) {
                    state_.SetGroundModeIndex(i);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const auto &bitrates = state_.Bitrates();
        if (ImGui::BeginCombo("\u7801\u7387(Mbps)", std::to_string(bitrates[state_.BitrateIndex()]).c_str())) {
            for (int i = 0; i < static_cast<int>(bitrates.size()); ++i) {
                bool selected = (state_.BitrateIndex() == i);
                if (ImGui::Selectable(std::to_string(bitrates[i]).c_str(), selected)) {
                    state_.SetBitrateIndex(i);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const auto &powers = state_.PowerLevels();
        if (ImGui::BeginCombo("\u5929\u7a7a\u7aef\u53d1\u5c04\u529f\u7387", std::to_string(powers[state_.SkyPowerIndex()]).c_str())) {
            for (int i = 0; i < static_cast<int>(powers.size()); ++i) {
                bool selected = (state_.SkyPowerIndex() == i);
                if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected)) {
                    state_.SetSkyPowerIndex(i);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (ImGui::BeginCombo("\u5730\u9762\u7aef\u53d1\u5c04\u529f\u7387", std::to_string(powers[state_.GroundPowerIndex()]).c_str())) {
            for (int i = 0; i < static_cast<int>(powers.size()); ++i) {
                bool selected = (state_.GroundPowerIndex() == i);
                if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected)) {
                    state_.SetGroundPowerIndex(i);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (ImGui::Button(state_.Recording() ? "\u505c\u6b62\u5f55\u50cf" : "\u5f00\u542f\u5f55\u50cf", ImVec2(-1, 0))) {
            state_.ToggleRecording();
        }

        ImGui::Spacing();
        ImGui::Columns(2, nullptr, false);
        if (ImGui::Button("\u786e\u8ba4", ImVec2(-1, 0))) {
            state_.ToggleMenuVisibility();
        }
        ImGui::NextColumn();
        if (ImGui::Button("\u5173\u95ed", ImVec2(-1, 0))) {
            state_.ToggleMenuVisibility();
        }
        ImGui::Columns(1);

    }

    ImGui::End();
}
