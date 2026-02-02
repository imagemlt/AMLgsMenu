#include "menu_renderer.h"

#include "imgui.h"
#include "video_mode.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <png.h>
#include <GLES2/gl2.h>
#include <thread>
#include <iostream>
#include <filesystem>

static MenuRenderer::TelemetryData BuildMockTelemetry(const MenuState &state)
{
    using Clock = std::chrono::steady_clock;
    static auto last_ground_sample = Clock::time_point{};
    const auto now_tp = Clock::now();
    const float t = static_cast<float>(ImGui::GetTime());

    MenuRenderer::TelemetryData data{};
    data.has_rc_signal = true;
    data.has_flight_mode = true;
    data.has_attitude = true;
    data.has_gps = true;
    data.has_battery = true;
    data.has_sky_temp = true;
    data.has_ground_batt = true;

    // Always update attitude/flight mode/GPS/battery each tick
    const char *modes[] = {"HORIZON", "ANGLE", "ACRO", "RTH"};
    data.flight_mode = modes[static_cast<int>(t / 4.0f) % 4];
    data.latitude = 37.773 + 0.001 * std::sin(t * 0.15f);
    data.longitude = -122.431 + 0.0015 * std::cos(t * 0.12f);
    data.altitude_m = 120.0f + 12.0f * std::sin(t * 0.35f);
    data.home_distance_m = 250.0f + 35.0f * std::cos(t * 0.45f);
    data.has_home_bearing = true;
    data.home_bearing_rel_deg = 45.0f * std::sin(t * 0.2f);
    data.gps_satellites = 12 + static_cast<int>(3.0f * std::sin(t * 0.25f));
    data.cell_voltage = 3.8f + 0.12f * std::sin(t * 0.6f);
    data.pack_voltage = data.cell_voltage * 4.0f + 0.4f * std::cos(t * 0.3f);
    data.sky_temp_c = 45.0f + 5.0f * std::sin(t * 0.22f);
    data.roll_deg = 10.0f * std::sin(t * 0.6f);
    data.pitch_deg = 15.0f * std::cos(t * 0.5f);
    data.has_speed = true;
    data.ground_speed_mps = 12.0f + 2.0f * std::sin(t * 0.45f);
    data.air_speed_mps = 11.0f + 2.5f * std::cos(t * 0.4f);
    data.rc_signal = -55.0f + 4.0f * std::sin(t * 1.1f);
    data.ground_batt_percent = 70.0f + 10.0f * std::sin(t * 0.3f);
    data.has_rc = true;
    data.rc_left_x = std::sin(t * 0.6f);
    data.rc_left_y = std::cos(t * 0.6f);
    data.rc_right_x = std::sin(t * 0.9f + 1.0f);
    data.rc_right_y = std::cos(t * 0.9f + 1.0f);

    // Ground-related metrics: sample at most once per second
    static MenuRenderer::TelemetryData ground_cache{};
    auto ms_since_ground = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_ground_sample).count();
    const bool update_ground = (last_ground_sample.time_since_epoch().count() == 0 || ms_since_ground >= 1000);
    if (update_ground)
    {
        ground_cache.ground_signal_a = -60.0f + 5.0f * std::sin(t * 0.8f);
        ground_cache.ground_signal_b = -62.0f + 6.0f * std::cos(t * 0.65f);
        ground_cache.ground_temp_c = ReadTemperatureC();
        ground_cache.wifi_monitor_count = 0;
        ground_cache.has_wifi_monitor = false;

        const auto &ground_modes = state.GroundModes();
        VideoMode mode = ground_modes.empty() ? VideoMode{"1920x1080 @ 60Hz", 1920, 1080, 60}
                                              : ground_modes[state.GroundModeIndex() % ground_modes.size()];
        std::ostringstream res;
        res << mode.width << "x" << mode.height;
        ground_cache.video_resolution = res.str();

        static float last_fps_time = -1.0f;
        static int cached_fps = 0;
        const float now = static_cast<float>(ImGui::GetTime());
        if (last_fps_time < 0.0f || (now - last_fps_time) >= 1.0f)
        {
            cached_fps = GetOutputFps();
            last_fps_time = now;
        }
        int fps = cached_fps;
        ground_cache.video_refresh_hz = fps > 0 ? fps : (mode.refresh ? mode.refresh : 60);
        ground_cache.bitrate_mbps = std::max(1.0f, 6.0f + 2.0f * std::sin(t * 0.4f));

        last_ground_sample = now_tp;
    }

    data.ground_signal_a = ground_cache.ground_signal_a;
    data.ground_signal_b = ground_cache.ground_signal_b;
    data.ground_temp_c = ground_cache.ground_temp_c;
    data.wifi_monitor_count = ground_cache.wifi_monitor_count;
    data.has_wifi_monitor = ground_cache.has_wifi_monitor;
    data.video_resolution = ground_cache.video_resolution;
    data.video_refresh_hz = ground_cache.video_refresh_hz;
    data.bitrate_mbps = ground_cache.bitrate_mbps;

    return data;
}

MenuRenderer::MenuRenderer(MenuState &state, bool &use_mock, std::function<TelemetryData(TelemetryData)> provider,
                           std::function<void()> toggle_terminal,
                           std::function<bool()> terminal_visible,
                           std::function<void(const std::string &)> start_update,
                           std::function<int()> update_status,
                           std::function<void()> request_reboot)
    : state_(state), use_mock_(use_mock), telemetry_provider_(std::move(provider)),
      toggle_terminal_(std::move(toggle_terminal)), terminal_visible_(std::move(terminal_visible)),
      start_update_(std::move(start_update)), update_status_(std::move(update_status)),
      request_reboot_(std::move(request_reboot))
{
    int w = 0, h = 0;
    const char *icon_base = "/storage/digitalfpv/icons/";
    LoadIcon(std::string(icon_base + std::string("antenna.png")).c_str(), icon_antenna_, w, h);
    LoadIcon(std::string(icon_base + std::string("battery_per.png")).c_str(), icon_batt_cell_, w, h);
    LoadIcon(std::string(icon_base + std::string("battery_all.png")).c_str(), icon_batt_pack_, w, h);
    LoadIcon(std::string(icon_base + std::string("gps.png")).c_str(), icon_gps_, w, h);
    LoadIcon(std::string(icon_base + std::string("speed.png")).c_str(), icon_speed_, w, h);
    LoadIcon(std::string(icon_base + std::string("monitor.png")).c_str(), icon_monitor_, w, h);
    LoadIcon(std::string(icon_base + std::string("temp_air.png")).c_str(), icon_temp_air_, w, h);
    LoadIcon(std::string(icon_base + std::string("temp_ground.png")).c_str(), icon_temp_ground_, w, h);
}

bool MenuRenderer::LoadIcon(const char *path, ImTextureID &out_id, int &out_w, int &out_h)
{
    FILE *fp = std::fopen(path, "rb");
    if (!fp)
        return false;
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
    {
        std::fclose(fp);
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        std::fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        std::fclose(fp);
        return false;
    }
    png_init_io(png_ptr, fp);
    png_read_info(png_ptr, info_ptr);
    png_uint_32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    png_read_update_info(png_ptr, info_ptr);
    std::vector<png_byte> data;
    data.resize(png_get_rowbytes(png_ptr, info_ptr) * height);
    std::vector<png_bytep> row_ptrs(height);
    for (png_uint_32 y = 0; y < height; ++y)
    {
        row_ptrs[y] = data.data() + y * png_get_rowbytes(png_ptr, info_ptr);
    }
    png_read_image(png_ptr, row_ptrs.data());
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    std::fclose(fp);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    out_id = static_cast<ImTextureID>(tex);
    out_w = static_cast<int>(width);
    out_h = static_cast<int>(height);
    return true;
}
MenuRenderer::~MenuRenderer()
{
    GLuint tex_ids[5]{};
    int count = 0;
    auto collect = [&](ImTextureID id)
    {
        if (id)
        {
            tex_ids[count++] = static_cast<GLuint>(id);
        }
    };
    collect(icon_antenna_);
    collect(icon_batt_cell_);
    collect(icon_batt_pack_);
    collect(icon_gps_);
    collect(icon_monitor_);
    if (count > 0)
    {
        glDeleteTextures(count, tex_ids);
    }
}

void MenuRenderer::Render(bool &running_flag)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    auto now_tp = std::chrono::steady_clock::now();
    bool need_refresh = (last_osd_update_time_ < 0.0f ||
                         last_osd_tp_.time_since_epoch().count() == 0 ||
                         std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_osd_tp_).count() >= 100);
    if (need_refresh)
    {
        TelemetryData new_data = cached_telemetry_;
        if (use_mock_)
        {
            new_data = BuildMockTelemetry(state_);
            has_mavlink_data_ = true;
        }
        else if (telemetry_provider_)
        {
            new_data = telemetry_provider_(cached_telemetry_);

            static auto last_ground_sample = std::chrono::steady_clock::time_point{};
            auto ms_ground = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_ground_sample).count();
            const bool update_ground = (last_ground_sample.time_since_epoch().count() == 0 || ms_ground >= 1000);
            if (!update_ground)
            {
                new_data.ground_signal_a = cached_telemetry_.ground_signal_a;
                new_data.ground_signal_b = cached_telemetry_.ground_signal_b;
                new_data.ground_temp_c = cached_telemetry_.ground_temp_c;
                new_data.video_refresh_hz = cached_telemetry_.video_refresh_hz;
                new_data.video_resolution = cached_telemetry_.video_resolution;
                new_data.bitrate_mbps = cached_telemetry_.bitrate_mbps;
            }
            else
            {
                last_ground_sample = now_tp;
            }
            bool any = new_data.has_attitude || new_data.has_gps || new_data.has_battery ||
                       new_data.has_rc_signal || new_data.has_sky_temp || new_data.has_flight_mode;
            if (any)
                has_mavlink_data_ = true;
        }
        cached_telemetry_ = new_data;
        last_osd_update_time_ = static_cast<float>(ImGui::GetTime());
        last_osd_tp_ = now_tp;
    }

    // No render throttling; always draw each frame. Remove FPS logs to reduce noise.

    DrawOsd(viewport, cached_telemetry_);

    ImGuiIO &io = ImGui::GetIO();
    const bool menu_visible = state_.MenuVisible();
    io.MouseDrawCursor = menu_visible;
    if (menu_visible)
    {
        DrawMenu(viewport, running_flag);
    }
    else
    {
        menu_visible_last_ = false;
    }
}

void MenuRenderer::DrawOsd(const ImGuiViewport *viewport, const TelemetryData &data) const
{
    ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
    const bool is_cn = state_.GetLanguage() == MenuState::Language::CN;
    const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f);

    const float icon_size = 18.0f * 1.5f;
    const float icon_gap = 6.0f * 1.5f;
    const ImU32 text_outline = IM_COL32(0, 0, 0, 255);    // solid black edge
    const ImU32 text_fill = IM_COL32(235, 245, 255, 255); // cool light tone for visibility
    auto draw_text_outline = [&](ImFont *font, float size, ImVec2 pos, ImU32 fill, ImU32 outline, const char *text)
    {
        const ImVec2 offsets[] = {
            ImVec2(-1.1f, 0.0f), ImVec2(1.1f, 0.0f), ImVec2(0.0f, -1.1f), ImVec2(0.0f, 1.1f),
            ImVec2(-0.8f, -0.8f), ImVec2(0.8f, -0.8f), ImVec2(-0.8f, 0.8f), ImVec2(0.8f, 0.8f)};
        for (const auto &off : offsets)
        {
            draw_list->AddText(font, size, ImVec2(pos.x + off.x, pos.y + off.y), outline, text);
        }
        draw_list->AddText(font, size, pos, fill, text);
    };

    auto draw_icon = [&](ImVec2 pos, ImTextureID tex)
    {
        if (tex)
        {
            draw_list->AddImage(tex, pos, ImVec2(pos.x + icon_size, pos.y + icon_size));
        }
        else
        {
            ImU32 fill = IM_COL32(80, 120, 200, 180);
            ImU32 border = IM_COL32(180, 210, 255, 220);
            draw_list->AddRectFilled(pos, ImVec2(pos.x + icon_size, pos.y + icon_size), fill, 3.0f);
            draw_list->AddRect(pos, ImVec2(pos.x + icon_size, pos.y + icon_size), border, 3.0f, 0, 1.5f);
        }
    };

    if (!use_mock_ && data.has_wifi_monitor)
    {
        char wifi_buf[64];
        std::snprintf(wifi_buf, sizeof(wifi_buf), "%d WIFI PLUGGED", data.wifi_monitor_count);
        ImVec2 pos(viewport->Pos.x + 20.0f, viewport->Pos.y + 20.0f);
        float small = ImGui::GetFontSize() * 0.85f;
        draw_text_outline(ImGui::GetFont(), small, pos, text_fill, text_outline, wifi_buf);
    }

    auto draw_horizon = [&](float roll_deg, float pitch_deg)
    {
        const float line_half_len = viewport->Size.x * 0.25f * 0.66f; // lengthen a bit vs previous
        const float tick_half_len = line_half_len * 0.04f;
        const float tick_gap = line_half_len;
        const float tick_step_deg = 5.0f;
        const int tick_count = 18;
        const float rad = roll_deg * 3.1415926f / 180.0f;
        const float cosr = std::cos(rad);
        const float sinr = std::sin(rad);
        ImVec2 left(-line_half_len, 0.0f);
        ImVec2 right(line_half_len, 0.0f);
        ImVec2 tick_left(-tick_half_len, 0.0f);
        ImVec2 tick_right(tick_half_len, 0.0f);
        auto rotate = [&](const ImVec2 &p)
        {
            return ImVec2(p.x * cosr - p.y * sinr, p.x * sinr + p.y * cosr);
        };
        const float pitch_offset = pitch_deg * 2.0f; // pixels per degree, tweak as needed
        const ImU32 tick_outline = IM_COL32(255, 255, 255, 220);
        ImVec2 p1 = rotate(left);
        ImVec2 p2 = rotate(right);
        p1.x += center.x;
        p2.x += center.x;
        p1.y += center.y + pitch_offset;
        p2.y += center.y + pitch_offset;
        draw_list->AddLine(p1, p2, IM_COL32(255, 255, 255, 255), 2.6f);

        // Pitch ladder ticks (screen-aligned, fixed on screen)
        ImVec2 zero_l(center.x - tick_gap, center.y);
        ImVec2 zero_r(center.x + tick_gap, center.y);
        draw_list->AddLine(zero_l, zero_r, tick_outline, 2.2f);
        draw_list->AddLine(zero_l, zero_r, IM_COL32(255, 255, 255, 140), 1.2f);
        for (int i = 1; i <= tick_count; ++i)
        {
            float delta = tick_step_deg * static_cast<float>(i);
            float y_up = center.y - (delta * 2.0f);
            float y_dn = center.y + (delta * 2.0f);

            ImVec2 up_l1(center.x - tick_gap - tick_half_len, y_up);
            ImVec2 up_l2(center.x - tick_gap + tick_half_len, y_up);
            ImVec2 up_r1(center.x + tick_gap - tick_half_len, y_up);
            ImVec2 up_r2(center.x + tick_gap + tick_half_len, y_up);
            draw_list->AddLine(up_l1, up_l2, tick_outline, 2.6f);
            draw_list->AddLine(up_r1, up_r2, tick_outline, 2.6f);
            draw_list->AddLine(up_l1, up_l2, IM_COL32(255, 255, 255, 200), 1.6f);
            draw_list->AddLine(up_r1, up_r2, IM_COL32(255, 255, 255, 200), 1.6f);

            ImVec2 dn_l1(center.x - tick_gap - tick_half_len, y_dn);
            ImVec2 dn_l2(center.x - tick_gap + tick_half_len, y_dn);
            ImVec2 dn_r1(center.x + tick_gap - tick_half_len, y_dn);
            ImVec2 dn_r2(center.x + tick_gap + tick_half_len, y_dn);
            draw_list->AddLine(dn_l1, dn_l2, tick_outline, 2.6f);
            draw_list->AddLine(dn_r1, dn_r2, tick_outline, 2.6f);
            draw_list->AddLine(dn_l1, dn_l2, IM_COL32(255, 255, 255, 200), 1.6f);
            draw_list->AddLine(dn_r1, dn_r2, IM_COL32(255, 255, 255, 200), 1.6f);
        }
    };
    if (data.has_attitude)
    {
        draw_horizon(data.roll_deg, data.pitch_deg);
    }

    /*if (data.has_rc)
    {
        const float box_size = 70.0f;
        const float box_gap = 26.0f;
        const float box_margin = 6.0f;
        ImVec2 center_bottom(viewport->Pos.x + viewport->Size.x * 0.5f,
                             viewport->Pos.y + viewport->Size.y - 18.0f);
        float total_w = box_size * 2.0f + box_gap;
        ImVec2 left_top(center_bottom.x - total_w * 0.5f,
                        center_bottom.y - box_size);
        ImVec2 right_top(left_top.x + box_size + box_gap, left_top.y);
        ImU32 box_col = IM_COL32(255, 255, 255, 190);
        draw_list->AddRect(left_top, ImVec2(left_top.x + box_size, left_top.y + box_size), box_col, 3.0f, 0, 1.6f);
        draw_list->AddRect(right_top, ImVec2(right_top.x + box_size, right_top.y + box_size), box_col, 3.0f, 0, 1.6f);

        auto clamp = [](float v) {
            if (v > 1.0f) return 1.0f;
            if (v < -1.0f) return -1.0f;
            return v;
        };
        float range = (box_size * 0.5f) - box_margin;
        ImVec2 left_center(left_top.x + box_size * 0.5f, left_top.y + box_size * 0.5f);
        ImVec2 right_center(right_top.x + box_size * 0.5f, right_top.y + box_size * 0.5f);
        float lx = clamp(data.rc_left_x);
        float ly = clamp(data.rc_left_y);
        float rx = clamp(data.rc_right_x);
        float ry = clamp(data.rc_right_y);
        ImVec2 left_dot(left_center.x + lx * range, left_center.y - ly * range);
        ImVec2 right_dot(right_center.x + rx * range, right_center.y - ry * range);
        draw_list->AddCircleFilled(left_dot, 6.0f, IM_COL32(255, 255, 255, 230));
        draw_list->AddCircleFilled(right_dot, 6.0f, IM_COL32(255, 255, 255, 230));
    }*/

    auto draw_centered_text = [&](ImVec2 pos, const std::string &text, ImU32 color, ImTextureID tex)
    {
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        ImVec2 icon_pos(pos.x - size.x * 0.5f - icon_size - icon_gap, pos.y);
        draw_icon(icon_pos, tex);
        ImVec2 text_pos(icon_pos.x + icon_size + icon_gap, pos.y);
        draw_text_outline(ImGui::GetFont(), ImGui::GetFontSize(), text_pos, color, text_outline, text.c_str());
    };

    auto draw_centered_text_no_icon = [&](ImVec2 pos, const std::string &text, ImU32 color)
    {
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        ImVec2 text_pos(pos.x - size.x * 0.5f, pos.y);
        draw_text_outline(ImGui::GetFont(), ImGui::GetFontSize(), text_pos, color, text_outline, text.c_str());
    };

    float signal_block_bottom = viewport->Pos.y + viewport->Size.y * 0.05f;
    if (data.has_rc_signal || data.ground_signal_a != 0.0f || data.ground_signal_b != 0.0f)
    {
        std::ostringstream signal;
        if (is_cn)
        {
            signal << "\u5730\u9762A: " << static_cast<int>(data.ground_signal_a) << " dBm  |  "
                   << "\u5730\u9762B: " << static_cast<int>(data.ground_signal_b) << " dBm";
            if (data.has_rc_signal)
            {
                signal << "  |  RC: " << static_cast<int>(data.rc_signal) << " dBm";
            }
        }
        else
        {
            signal << "GND A: " << static_cast<int>(data.ground_signal_a) << " dBm  |  "
                   << "GND B: " << static_cast<int>(data.ground_signal_b) << " dBm";
            if (data.has_rc_signal)
            {
                signal << "  |  RC: " << static_cast<int>(data.rc_signal) << " dBm";
            }
        }
        draw_centered_text(ImVec2(center.x, viewport->Pos.y + viewport->Size.y * 0.05f),
                           signal.str(), text_fill, icon_antenna_);
        signal_block_bottom = viewport->Pos.y + viewport->Size.y * 0.05f + ImGui::GetFontSize() * 1.2f;
    }

    if (data.has_flight_mode)
    {
        ImFont *font = ImGui::GetFont();
        float base = ImGui::GetFontSize();
        float mode_size = base * 1.5f;                  // enlarge flight mode text
        ImU32 mode_fill = IM_COL32(170, 220, 255, 255); // soft cyan for readability
        ImU32 mode_outline = IM_COL32(10, 26, 42, 240); // deep navy outline
        std::string label = data.flight_mode;
        ImVec2 size = ImGui::CalcTextSize(label.c_str());
        ImVec2 pos(center.x - size.x * 0.5f, center.y - viewport->Size.y * 0.25f);
        ImDrawList *dl = draw_list;
        draw_text_outline(font, mode_size, pos, mode_fill, mode_outline, label.c_str());
    }

    ImGuiWindowFlags overlay_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;

    auto icon_text_line = [&](const char *text, ImTextureID tex)
    {
        ImVec2 start = ImGui::GetCursorScreenPos();
        draw_icon(start, tex);
        ImVec2 text_pos(start.x + icon_size + icon_gap, start.y);
        ImVec2 text_size = ImGui::CalcTextSize(text);
        draw_text_outline(ImGui::GetFont(), ImGui::GetFontSize(), text_pos, text_fill, text_outline, text);
        ImGui::Dummy(ImVec2(icon_size + icon_gap + text_size.x, icon_size));
    };

    if (data.has_gps)
    {
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 16.0f,
                                       viewport->Pos.y + viewport->Size.y - 140.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("OSD_GPS", nullptr, overlay_flags))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, text_fill);
            if (data.has_speed)
            {
                char speed_buf[64];
                if (is_cn)
                {
                    snprintf(speed_buf, sizeof(speed_buf), "\u5730\u901f/\u7a7a\u901f: %.1f/%.1f m/s",
                             data.ground_speed_mps, data.air_speed_mps);
                }
                else
                {
                    snprintf(speed_buf, sizeof(speed_buf), "GS/AS: %.1f/%.1f m/s",
                             data.ground_speed_mps, data.air_speed_mps);
                }
                icon_text_line(speed_buf, icon_speed_);
            }
            char sat_buf[64];
            if (data.gps_satellites >= 0)
            {
                if (is_cn)
                {
                    snprintf(sat_buf, sizeof(sat_buf), "\u536b\u661f\u6570: %d", data.gps_satellites);
                }
                else
                {
                    snprintf(sat_buf, sizeof(sat_buf), "Satellites: %d", data.gps_satellites);
                }
                icon_text_line(sat_buf, icon_gps_);
            }
            char gps_buf[128];
            if (is_cn)
            {
                snprintf(gps_buf, sizeof(gps_buf), "GPS: %.5f, %.5f, %.1fm",
                         data.latitude, data.longitude, data.altitude_m);
            }
            else
            {
                snprintf(gps_buf, sizeof(gps_buf), "GPS: %.5f, %.5f, %.1fm",
                         data.latitude, data.longitude, data.altitude_m);
            }
            icon_text_line(gps_buf, icon_gps_);
            char home_buf[64];
            if (is_cn)
            {
                snprintf(home_buf, sizeof(home_buf), "\u79bb\u5bb6\u8ddd\u79bb: %.1fm", data.home_distance_m);
            }
            else
            {
                snprintf(home_buf, sizeof(home_buf), "Home Dist: %.1fm", data.home_distance_m);
            }
            icon_text_line(home_buf, icon_gps_);
            if (data.has_home_bearing)
            {
                ImVec2 line_start = ImGui::GetItemRectMin();
                ImVec2 line_end = ImGui::GetItemRectMax();
                const char *home_label = is_cn ? "回家方向" : "HOME DIRCT";
                ImVec2 label_size = ImGui::CalcTextSize(home_label);
                ImVec2 label_pos(line_end.x + 8.0f, line_start.y);
                draw_text_outline(ImGui::GetFont(), ImGui::GetFontSize(), label_pos, text_fill, text_outline, home_label);
                float size = 21.0f;
                ImVec2 arrow_center(label_pos.x + label_size.x + size + 8.0f,
                                    line_start.y + icon_size * 0.5f);
                float rel_rad = data.home_bearing_rel_deg * 3.1415926f / 180.0f;
                ImVec2 dir(std::sin(rel_rad), -std::cos(rel_rad));
                ImVec2 side(-dir.y, dir.x);
                ImVec2 tip = ImVec2(arrow_center.x + dir.x * size, arrow_center.y + dir.y * size);
                ImVec2 left = ImVec2(arrow_center.x - dir.x * size * 0.6f + side.x * size * 0.6f,
                                     arrow_center.y - dir.y * size * 0.6f + side.y * size * 0.6f);
                ImVec2 right = ImVec2(arrow_center.x - dir.x * size * 0.6f - side.x * size * 0.6f,
                                      arrow_center.y - dir.y * size * 0.6f - side.y * size * 0.6f);
                draw_list->AddTriangleFilled(tip, left, right, IM_COL32(255, 255, 255, 220));
                draw_list->AddTriangle(tip, left, right, IM_COL32(0, 0, 0, 180), 1.5f);
            }
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - 16.0f,
                                   viewport->Pos.y + viewport->Size.y - 48.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("OSD_VIDEO", nullptr, overlay_flags))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, text_fill);
        char video_buf[128];
        if (is_cn)
        {
            snprintf(video_buf, sizeof(video_buf), "\u89c6\u9891: %.1f Mbps %s @ %dHz",
                     data.bitrate_mbps, data.video_resolution.c_str(), data.video_refresh_hz);
        }
        else
        {
            snprintf(video_buf, sizeof(video_buf), "Video: %.1f Mbps %s @ %dHz",
                     data.bitrate_mbps, data.video_resolution.c_str(), data.video_refresh_hz);
        }
        icon_text_line(video_buf, icon_monitor_);
        ImGui::PopStyleColor();
    }
    ImGui::End();

    if (data.has_ground_batt)
    {
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - 16.0f,
                                       viewport->Pos.y + 80.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("OSD_GROUND_BATT", nullptr, overlay_flags))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, text_fill);
            char ground_batt_buf[64];
            if (is_cn)
            {
                snprintf(ground_batt_buf, sizeof(ground_batt_buf), "\u672c\u673a\u7535\u91cf: %.0f%%", data.ground_batt_percent);
            }
            else
            {
                snprintf(ground_batt_buf, sizeof(ground_batt_buf), "Ground Batt: %.0f%%", data.ground_batt_percent);
            }
            icon_text_line(ground_batt_buf, icon_batt_pack_);
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }

    if (data.has_battery)
    {
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 16.0f, center.y - 24.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("OSD_BATT", nullptr, overlay_flags))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, text_fill);
            char cell_buf[32];
            if (is_cn)
            {
                snprintf(cell_buf, sizeof(cell_buf), "%s: %.2fV",
                         data.cell_voltage_estimated ? "\u5355\u8282(\u4f30)" : "\u5355\u8282",
                         data.cell_voltage);
            }
            else
            {
                snprintf(cell_buf, sizeof(cell_buf), "%s: %.2fV",
                         data.cell_voltage_estimated ? "Cell~" : "Cell",
                         data.cell_voltage);
            }
            icon_text_line(cell_buf, icon_batt_cell_);
            char pack_buf[32];
            if (is_cn)
            {
                snprintf(pack_buf, sizeof(pack_buf), "\u603b\u7535: %.2fV", data.pack_voltage);
            }
            else
            {
                snprintf(pack_buf, sizeof(pack_buf), "Pack: %.2fV", data.pack_voltage);
            }
            icon_text_line(pack_buf, icon_batt_pack_);
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - 16.0f, center.y - 24.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("OSD_TEMP", nullptr, overlay_flags))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, text_fill);
        char sky_buf[32];
        snprintf(sky_buf, sizeof(sky_buf), is_cn ? "\u5929\u7a7a\u7aef\u6e29\u5ea6: %.1f\u2103" : "Air Temp: %.1fC", data.sky_temp_c);
        if (data.has_sky_temp)
        {
            icon_text_line(sky_buf, icon_temp_air_);
        }
        char ground_buf[32];
        snprintf(ground_buf, sizeof(ground_buf), is_cn ? "\u5730\u9762\u7aef\u6e29\u5ea6: %.1f\u2103" : "Ground Temp: %.1fC", data.ground_temp_c);
        icon_text_line(ground_buf, icon_temp_ground_);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

void MenuRenderer::DrawMenu(const ImGuiViewport *viewport, bool &running_flag)
{
    const float base_font_size = 26.0f;
    const float font_scale = ImGui::GetFontSize() / base_font_size;
    const float menu_height_scale = std::min(1.15f, std::max(1.0f, font_scale));
    const float menu_height_ratio = 0.70f + (menu_height_scale - 1.0f) * 0.45f;
    const ImVec2 menu_size = ImVec2(viewport->Size.x * 0.54f, viewport->Size.y * menu_height_ratio);
    const float menu_pos_y_ratio = std::max(0.10f, 0.16f - (menu_height_ratio - 0.70f) * 0.5f);
    const ImVec2 menu_pos = ImVec2(viewport->Pos.x + viewport->Size.x * 0.23f,
                                   viewport->Pos.y + viewport->Size.y * menu_pos_y_ratio);
    const bool is_cn = state_.GetLanguage() == MenuState::Language::CN;
    bool kodi_popup_requested = false;

    if (!menu_visible_last_)
    {
        last_focus_index_ = -1;
    }
    menu_visible_last_ = true;
    if (update_status_)
    {
        int status = update_status_();
        if (status != update_status_last_)
        {
            if (status == 2)
            {
                update_reboot_popup_pending_ = true;
            }
            else if (status == 3)
            {
                update_failed_popup_pending_ = true;
            }
            update_status_last_ = status;
        }
    }

    ImGui::SetNextWindowBgAlpha(0.6f); // keep menu semi-transparent
    ImGui::SetNextWindowPos(menu_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(menu_size, ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.3f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.3f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(18, 26, 36, 230));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(90, 220, 210, 255)); // brighter teal border
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 245, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(36, 52, 72, 230));  // glassy blue
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(58, 92, 122, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(76, 120, 150, 255));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(56, 110, 150, 235)); // vivid buttons
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(78, 140, 185, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(48, 96, 135, 255));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("GS Control Menu", nullptr, flags))
    {
        const bool popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        const bool nav_down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false);
        const bool nav_up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false);
        if (!popup_open && (nav_down || nav_up) && last_focus_index_ < 0 && pending_focus_index_ < 0)
        {
            pending_focus_index_ = 0;
            // Prevent ImGui default nav from skipping the first item on initial focus.
            ImGuiIO &io = ImGui::GetIO();
            const ImGuiKey key = nav_down ? ImGuiKey_DownArrow : ImGuiKey_UpArrow;
            ImGuiKeyData &key_data = io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
            key_data.Down = false;
            key_data.DownDuration = -1.0f;
            key_data.DownDurationPrev = -1.0f;
        }

        int focus_index = 0;
        int focused_index = -1;
        std::array<std::vector<int>, 2> focus_columns;
        std::vector<int> index_to_col;
        std::vector<int> index_to_pos;
        auto register_focus = [&](int col, const std::function<void()> &draw)
        {
            const bool want_focus = (pending_focus_index_ == focus_index);
            if (want_focus)
            {
                ImGui::SetKeyboardFocusHere();
            }
            draw();
            if (want_focus)
            {
                pending_focus_index_ = -1;
            }
            if (ImGui::IsItemFocused())
            {
                focused_index = focus_index;
            }
            index_to_col.push_back(col);
            index_to_pos.push_back(static_cast<int>(focus_columns[col].size()));
            focus_columns[col].push_back(focus_index);
            ++focus_index;
        };

        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(210, 225, 240, 230));
        ImGui::TextUnformatted(is_cn ? "\u65e0\u7ebf\u94fe\u8def\u914d\u7f6e" : "Wireless Link Settings");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(12, 10)); // slightly taller rows
        if (ImGui::BeginTable("menu_table", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
        {
            ImGui::TableSetupColumn("L1", ImGuiTableColumnFlags_WidthStretch, 0.24f);
            ImGui::TableSetupColumn("C1", ImGuiTableColumnFlags_WidthStretch, 0.24f);
            ImGui::TableSetupColumn("L2", ImGuiTableColumnFlags_WidthStretch, 0.24f);
            ImGui::TableSetupColumn("C2", ImGuiTableColumnFlags_WidthStretch, 0.24f);

            auto row_pair = [&](const char *l1, const std::function<void()> &c1,
                                const char *l2, const std::function<void()> &c2)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(l1);
                ImGui::TableSetColumnIndex(1);
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.8f);
                c1();
                ImGui::PopItemWidth();
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(l2);
                ImGui::TableSetColumnIndex(3);
                ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.8f);
                c2();
                ImGui::PopItemWidth();
            };
            const auto &channels = state_.Channels();
            const auto &bandwidths = state_.Bandwidths();
            row_pair(is_cn ? "\u4fe1\u9053" : "Channel", [&]
                     { register_focus(0, [&]
                       {
                         if (ImGui::BeginCombo("##channel", std::to_string(channels[state_.ChannelIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(channels.size()); ++i) {
                                 bool selected = (state_.ChannelIndex() == i);
                                 if (ImGui::Selectable(std::to_string(channels[i]).c_str(), selected)) {
                                     state_.SetChannelIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         } }); }, is_cn ? "\u9891\u5bbd" : "Bandwidth", [&]
                     { register_focus(1, [&]
                       {
                         if (ImGui::BeginCombo("##bandwidth", bandwidths[state_.BandwidthIndex()])) {
                             for (int i = 0; i < static_cast<int>(bandwidths.size()); ++i) {
                                 bool selected = (state_.BandwidthIndex() == i);
                                 if (ImGui::Selectable(bandwidths[i], selected)) {
                                     state_.SetBandwidthIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         } }); });

            const auto &sky_modes = state_.SkyModes();
            const auto &ground_modes = state_.GroundModes();
            row_pair(is_cn ? "\u5929\u7a7a\u7aef\u5206\u8fa8\u7387/\u5237\u65b0\u7387" : "Air Res/Refresh", [&]
                     { register_focus(0, [&]
                       {
                         if (!sky_modes.empty() && ImGui::BeginCombo("##sky_mode", FormatVideoModeLabel(sky_modes[state_.SkyModeIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(sky_modes.size()); ++i) {
                                 bool selected = (state_.SkyModeIndex() == i);
                                 if (ImGui::Selectable(FormatVideoModeLabel(sky_modes[i]).c_str(), selected)) {
                                     state_.SetSkyModeIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         } }); }, is_cn ? "\u5730\u9762\u7aef\u5206\u8fa8\u7387/\u5237\u65b0\u7387" : "Ground Res/Refresh", [&]
                     { register_focus(1, [&]
                       {
        if (!ground_modes.empty() && ImGui::BeginCombo("##ground_mode", FormatVideoModeLabel(ground_modes[state_.GroundModeIndex()]).c_str())) {
            for (int i = 0; i < static_cast<int>(ground_modes.size()); ++i) {
                bool selected = (state_.GroundModeIndex() == i);
                if (ImGui::Selectable(FormatVideoModeLabel(ground_modes[i]).c_str(), selected)) {
                    const auto &mode = ground_modes[i];
                    const bool requires_warning = (mode.refresh > 60) && !state_.IsGroundModePersisted(mode.label);
                    if (requires_warning) {
                        pending_high_refresh_index_ = i;
                        pending_high_refresh_label_ = mode.label;
                        high_refresh_popup_pending_ = true;
                    } else {
                        state_.SetGroundModeIndex(i);
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        } }); });

            const auto &bitrates = state_.Bitrates();
            const auto &powers = state_.PowerLevels();
            const auto &mcs_levels = state_.McsLevels();
            row_pair(is_cn ? "\u7801\u7387(Mbps)" : "Bitrate (Mbps)", [&]
                     { register_focus(0, [&]
                       {
                         if (ImGui::BeginCombo("##bitrate", std::to_string(bitrates[state_.BitrateIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(bitrates.size()); ++i) {
                                 bool selected = (state_.BitrateIndex() == i);
                                 if (ImGui::Selectable(std::to_string(bitrates[i]).c_str(), selected)) {
                                     state_.SetBitrateIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                         }
                            ImGui::EndCombo();
                        } }); }, is_cn ? "\u5929\u7a7a\u7aef\u53d1\u5c04\u529f\u7387" : "Air TX Power", [&]
                     { register_focus(1, [&]
                       {
                         if (ImGui::BeginCombo("##sky_power", std::to_string(powers[state_.SkyPowerIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(powers.size()); ++i) {
                                 bool selected = (state_.SkyPowerIndex() == i);
                                 if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected)) {
                                     state_.SetSkyPowerIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         } }); });

            row_pair(is_cn ? "\u5929\u7a7a\u7aefMCS" : "Air MCS", [&]
                     {
                         if (mcs_levels.empty())
                         {
                             ImGui::TextUnformatted("--");
                             return;
                         }
                         register_focus(0, [&]
                         {
                         const int current_mcs = mcs_levels[state_.SkyMcsIndex()];
                         if (ImGui::BeginCombo("##sky_mcs", std::to_string(current_mcs).c_str()))
                         {
                             for (int i = 0; i < static_cast<int>(mcs_levels.size()); ++i)
                             {
                                 bool selected = (state_.SkyMcsIndex() == i);
                                 if (ImGui::Selectable(std::to_string(mcs_levels[i]).c_str(), selected))
                                 {
                                     state_.SetSkyMcsIndex(i);
                                 }
                                 if (selected)
                                     ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         } });
                     }, is_cn ? "\u5730\u9762\u7aef\u53d1\u5c04\u529f\u7387" : "Ground TX Power", [&]
                     { register_focus(1, [&]
                       {
                         if (ImGui::BeginCombo("##ground_power", std::to_string(powers[state_.GroundPowerIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(powers.size()); ++i) {
                                 bool selected = (state_.GroundPowerIndex() == i);
                                 if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected)) {
                                     state_.SetGroundPowerIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         } }); });

            const auto &volume_levels = state_.VolumeLevels();
            row_pair(is_cn ? "\u58f0\u97f3\u5f00\u5173" : "Sound", [&]
                     { register_focus(0, [&]
                       {
                         bool enabled = state_.SoundEnabled();
                         if (ImGui::Checkbox("##sound_enable", &enabled))
                         {
                             state_.SetSoundEnabled(enabled);
                         } }); }, is_cn ? "\u97f3\u91cf" : "Volume", [&]
                     {
                         if (volume_levels.empty())
                         {
                             ImGui::TextUnformatted("--");
                             return;
                         }
                         register_focus(1, [&]
                         {
                         int volume_index = state_.SoundVolumeIndex();
                         if (volume_index < 0 || volume_index >= static_cast<int>(volume_levels.size()))
                         {
                             volume_index = std::max(0, std::min(static_cast<int>(volume_levels.size()) - 1, volume_index));
                         }
                         const std::string label = std::to_string(volume_levels[volume_index]) + "%";
                         if (ImGui::BeginCombo("##sound_volume", label.c_str()))
                         {
                             for (int i = 0; i < static_cast<int>(volume_levels.size()); ++i)
                             {
                                 const std::string item_label = std::to_string(volume_levels[i]) + "%";
                                 bool selected = (volume_index == i);
                                 if (ImGui::Selectable(item_label.c_str(), selected))
                                 {
                                     state_.SetSoundVolumeIndex(i);
                                 }
                                 if (selected)
                                     ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         } });
                     });

            row_pair(is_cn ? "\u81ea\u9002\u5e94\u94fe\u8def" : "Adaptive Link", [&]
                     {
                         register_focus(0, [&]
                         {
                             bool enabled = state_.AdaptiveLinkEnabled();
                             if (ImGui::Checkbox("##adaptive_link", &enabled))
                             {
                                 state_.SetAdaptiveLinkEnabled(enabled);
                             }
                         });
                     }, is_cn ? "\u574f\u5305\u7b56\u7565" : "Bad Frame Policy", [&]
                     {
                         register_focus(1, [&]
                         {
                             const char *label_cn[] = {"\u4e22\u5305", "\u5ffd\u7565"};
                             const char *label_en[] = {"Drop", "Ignore"};
                             const char *current = (state_.BadFrameIndex() == 0)
                                                       ? (is_cn ? label_cn[0] : label_en[0])
                                                       : (is_cn ? label_cn[1] : label_en[1]);
                             if (ImGui::BeginCombo("##bad_frame", current))
                             {
                                 for (int i = 0; i < 2; ++i)
                                 {
                                     const char *label = is_cn ? label_cn[i] : label_en[i];
                                     bool selected = (state_.BadFrameIndex() == i);
                                     if (ImGui::Selectable(label, selected))
                                     {
                                         state_.SetBadFrameIndex(i);
                                     }
                                     if (selected)
                                         ImGui::SetItemDefaultFocus();
                                 }
                                 ImGui::EndCombo();
                             }
                         });
                     });

            const auto &buffer_levels = state_.BufferLevels();
            row_pair(is_cn ? "\u7f13\u5b58\u7ea7\u522b" : "Buffer Level", [&]
                     {
                         if (buffer_levels.empty())
                         {
                             ImGui::TextUnformatted("--");
                             return;
                         }
                         register_focus(0, [&]
                         {
                             int idx = state_.BufferLevelIndex();
                             if (idx < 0 || idx >= static_cast<int>(buffer_levels.size()))
                             {
                                 idx = 0;
                             }
                             const std::string label = std::to_string(buffer_levels[idx]);
                             if (ImGui::BeginCombo("##buffer_level", label.c_str()))
                             {
                                 for (int i = 0; i < static_cast<int>(buffer_levels.size()); ++i)
                                 {
                                     const std::string item_label = std::to_string(buffer_levels[i]);
                                     bool selected = (idx == i);
                                     if (ImGui::Selectable(item_label.c_str(), selected))
                                     {
                                         state_.SetBufferLevelIndex(i);
                                     }
                                     if (selected)
                                         ImGui::SetItemDefaultFocus();
                                 }
                                 ImGui::EndCombo();
                             }
                         });
                     },
                     "",
                     [&]
                     {
                             ImGui::Dummy(ImVec2(-1, 0));
                     });

            row_pair(is_cn ? "\u56fa\u4ef6\u6a21\u5f0f" : "Firmware", [&]
                     { register_focus(0, [&]
                       {
                         const bool is_cc = state_.GetFirmwareType() == MenuState::FirmwareType::CCEdition;
                         if (ImGui::RadioButton(is_cn ? "CC (UDP)" : "CC (UDP)", is_cc)) {
                             state_.SetFirmwareType(MenuState::FirmwareType::CCEdition);
                         }
                       }); }, is_cn ? "\u8bed\u8a00" : "Language", [&]
                     { register_focus(1, [&]
                       {
                         const char *label = state_.GetLanguage() == MenuState::Language::CN ? "\u4e2d\u6587" : "English";
                         if (ImGui::BeginCombo("##lang", label)) {
                             if (ImGui::Selectable("\u4e2d\u6587", state_.GetLanguage() == MenuState::Language::CN)) {
                                 state_.SetLanguage(MenuState::Language::CN);
                             }
                             if (ImGui::Selectable("English", state_.GetLanguage() == MenuState::Language::EN)) {
                                 state_.SetLanguage(MenuState::Language::EN);
                             }
                             ImGui::EndCombo();
                         } }); });

            row_pair("", [&]
                     { register_focus(0, [&]
                       {
                         const bool is_official = state_.GetFirmwareType() == MenuState::FirmwareType::Official;
                         if (ImGui::RadioButton(is_cn ? "\u5b98\u65b9 (SSH)" : "Official (SSH)", is_official)) {
                             state_.SetFirmwareType(MenuState::FirmwareType::Official);
                         }
                       }); }, "", [&]
                     { ImGui::Dummy(ImVec2(-1, 0)); });

            row_pair("",
                     [&]
                     {
                         if (!toggle_terminal_)
                         {
                             ImGui::Dummy(ImVec2(-1, 0));
                             return;
                         }
                         register_focus(0, [&]
                         {
                             const char *label = (terminal_visible_ && terminal_visible_())
                                                     ? (is_cn ? "\u5173\u95ed\u7ec8\u7aef" : "Hide Terminal")
                                                     : (is_cn ? "\u6253\u5f00\u7ec8\u7aef" : "Open Terminal");
                             if (ImGui::Button(label, ImVec2(-1, 0)))
                             {
                                 toggle_terminal_();
                             }
                         });
                     },
                     "",
                     [&]
                     {
                         register_focus(1, [&]
                         {
                             const char *label = state_.Recording()
                                                     ? (is_cn ? "\u505c\u6b62\u5f55\u50cf" : "Stop Recording")
                                                     : (is_cn ? "\u5f00\u542f\u5f55\u50cf" : "Start Recording");
                             if (ImGui::Button(label, ImVec2(-1, 0)))
                             {
                                 state_.ToggleRecording();
                             }
                         });
                     });

            row_pair("",
                     [&]
                     {
                        register_focus(0, [&]
                        {
                            if (ImGui::Button(is_cn ? "\u66f4\u65b0\u56fa\u4ef6" : "Update", ImVec2(-1, 0)))
                            {
                                namespace fs = std::filesystem;
                                std::string found_path;
                                fs::path flash_path("/flash/update_fpv.tar.gz");
                                std::error_code ec;
                                if (fs::exists(flash_path, ec))
                                {
                                    found_path = flash_path.string();
                                }
                                else
                                {
                                    fs::path media_root("/media");
                                    if (fs::exists(media_root, ec))
                                    {
                                        for (const auto &entry : fs::directory_iterator(media_root, ec))
                                        {
                                            if (!entry.is_directory())
                                                continue;
                                            fs::path candidate = entry.path() / "update_fpv.tar.gz";
                                            if (fs::exists(candidate, ec))
                                            {
                                                found_path = candidate.string();
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (!found_path.empty())
                                {
                                    update_path_ = found_path;
                                    update_popup_pending_ = true;
                                }
                                else
                                {
                                    update_missing_popup_pending_ = true;
                                }
                            }
                        });
                     },
                     "",
                     [&]
                     {
                        register_focus(1, [&]
                        {
                            if (ImGui::Button(is_cn ? "\u6253\u5f00 KODI" : "Open KODI", ImVec2(-1, 0)))
                            {
                                kodi_popup_requested = true;
                            }
                        });
                     });

            row_pair("",
                     [&]
                     {
                        ImGui::Dummy(ImVec2(-1, 0));
                     },
                     "",
                     [&]
                     {
                        register_focus(1, [&]
                        {
                            if (ImGui::Button(is_cn ? "\u542f\u52a8\u5230\u5b89\u5353" : "Boot to Android", ImVec2(-1, 0)))
                            {
                                std::system("/sbin/rebootfromnand;reboot");
                                running_flag = false;
                            }
                        });
                     });

            row_pair("",
                     [&]
                     {
                         ImGui::Dummy(ImVec2(-1, 0));
                     },
                     "",
                     [&]
                     {
                         register_focus(1, [&]
                         {
                             if (ImGui::Button(is_cn ? "\u786e\u8ba4" : "OK", ImVec2(-1, 0)))
                             {
                                 state_.ToggleMenuVisibility();
                             }
                         });
                     });

            ImGui::EndTable();
            if (!popup_open && focused_index >= 0 && (nav_down || nav_up))
            {
                const int col = index_to_col[focused_index];
                const int pos = index_to_pos[focused_index];
                const auto &col_list = focus_columns[col];
                const auto &other_list = focus_columns[1 - col];
                // Edge wrap: use actual focus lists so new items don't need manual index updates.
                bool wrapped = false;
                if (nav_down && pos == static_cast<int>(col_list.size()) - 1 && !other_list.empty())
                {
                    pending_focus_index_ = other_list.front();
                    wrapped = true;
                }
                else if (nav_up && pos == 0 && !other_list.empty())
                {
                    pending_focus_index_ = other_list.back();
                    wrapped = true;
                }
                if (wrapped)
                {
                    // Prevent ImGui default nav from moving focus inside the same column first.
                    ImGuiIO &io = ImGui::GetIO();
                    const ImGuiKey key = nav_down ? ImGuiKey_DownArrow : ImGuiKey_UpArrow;
                    ImGuiKeyData &key_data = io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
                    key_data.Down = false;
                    key_data.DownDuration = -1.0f;
                    key_data.DownDurationPrev = -1.0f;
                }
            }
            // When adding/removing focusable widgets, keep the registration order
            // aligned with the table layout for predictable Up/Down navigation.
            last_focus_columns_ = focus_columns;
            last_focus_index_to_col_ = index_to_col;
            last_focus_index_to_pos_ = index_to_pos;
            if (focused_index >= 0)
            {
                last_focus_index_ = focused_index;
            }
        }
        ImGui::PopStyleVar();

        if (high_refresh_popup_pending_ && pending_high_refresh_index_ >= 0)
        {
            ImGui::OpenPopup("confirm_high_refresh");
            high_refresh_popup_pending_ = false;
        }
        if (high_refresh_persist_popup_pending_)
        {
            ImGui::OpenPopup("confirm_high_refresh_persist");
            high_refresh_persist_popup_pending_ = false;
        }

        ImGui::SetNextWindowSize(ImVec2(420.0f, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("confirm_high_refresh", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            const char *msg = is_cn ? "\u8be5\u5206\u8fa8\u7387\u5237\u65b0\u7387\u8d85\u8fc760Hz\uff0c\u53ef\u80fd\u5bfc\u81f4\u9ed1\u5c4f\uff0c\u662f\u5426\u7ee7\u7eed\uff1f"
                                    : "This refresh rate exceeds 60Hz and may cause a black screen. Continue?";
            ImGui::TextWrapped("%s", msg);
            ImGui::Spacing();
            const float hr_button_width = 130.0f;
            const float hr_spacing = ImGui::GetStyle().ItemSpacing.x;
            const float hr_total_width = hr_button_width * 2.0f + hr_spacing;
            const float hr_region_width = ImGui::GetContentRegionAvail().x;
            const float hr_base_x = ImGui::GetCursorPosX();
            if (hr_region_width > hr_total_width)
            {
                ImGui::SetCursorPosX(hr_base_x + (hr_region_width - hr_total_width) * 0.5f);
            }
            if (ImGui::Button(is_cn ? "\u53d6\u6d88" : "Cancel", ImVec2(hr_button_width, 0)))
            {
                pending_high_refresh_index_ = -1;
                pending_high_refresh_label_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(is_cn ? "\u7ee7\u7eed" : "Continue", ImVec2(hr_button_width, 0)))
            {
                if (pending_high_refresh_index_ >= 0)
                {
                    state_.RequestGroundModeSkipSaveOnce();
                    state_.SetGroundModeIndex(pending_high_refresh_index_);
                    pending_high_refresh_index_ = -1;
                }
                high_refresh_persist_popup_pending_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SetNextWindowSize(ImVec2(360.0f, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("confirm_high_refresh_persist", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            const char *msg = is_cn ? "\u662f\u5426\u4ee5\u540e\u9ed8\u8ba4\u4f7f\u7528\u8be5\u9ad8\u5237\u65b9\u6848\uff1f"
                                    : "Use this high refresh rate by default in the future?";
            ImGui::TextWrapped("%s", msg);
            ImGui::Spacing();
            const float persist_button_width = 120.0f;
            const float persist_spacing = ImGui::GetStyle().ItemSpacing.x;
            const float persist_total = persist_button_width * 2.0f + persist_spacing;
            const float persist_region = ImGui::GetContentRegionAvail().x;
            const float persist_base = ImGui::GetCursorPosX();
            if (persist_region > persist_total)
            {
                ImGui::SetCursorPosX(persist_base + (persist_region - persist_total) * 0.5f);
            }
            if (ImGui::Button(is_cn ? "\u5426" : "No", ImVec2(persist_button_width, 0)))
            {
                pending_high_refresh_label_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(is_cn ? "\u662f" : "Yes", ImVec2(persist_button_width, 0)))
            {
                if (!pending_high_refresh_label_.empty())
                {
                    state_.RequestGroundModeForceSaveOnce();
                    state_.ForceGroundModeNotifyOnce();
                    state_.SetGroundModeIndex(state_.GroundModeIndex());
                    state_.SetGroundModePersisted(pending_high_refresh_label_, true);
                    pending_high_refresh_label_.clear();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (kodi_popup_requested)
        {
            kodi_popup_focus_index_ = 0;
            kodi_popup_focus_dirty_ = true;
            ImGui::OpenPopup("confirm_kodi");
        }
        ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("confirm_kodi", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            const char *msg = is_cn ? "\u6253\u5f00 KODI \u5c06\u5173\u95ed\u56fe\u4f20\u7a0b\u5e8f\uff0c\u662f\u5426\u7ee7\u7eed\uff1f"
                                    : "Opening KODI will close the video link process. Continue?";
            ImGui::TextWrapped("%s", msg);
            ImGui::Spacing();
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
            {
                kodi_popup_focus_index_ = 1 - kodi_popup_focus_index_;
                kodi_popup_focus_dirty_ = true;
            }
            const float button_width = 140.0f;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float total_width = button_width * 2.0f + spacing;
            const float region_width = ImGui::GetContentRegionAvail().x;
            const float base_x = ImGui::GetCursorPosX();
            if (region_width > total_width)
            {
                ImGui::SetCursorPosX(base_x + (region_width - total_width) * 0.5f);
            }

            auto render_popup_button = [&](int index, const char *label, auto &&handler)
            {
                if (index > 0)
                {
                    ImGui::SameLine();
                }
                if (kodi_popup_focus_dirty_ && kodi_popup_focus_index_ == index)
                {
                    ImGui::SetKeyboardFocusHere();
                    kodi_popup_focus_dirty_ = false;
                }
                if (ImGui::Button(label, ImVec2(button_width, 0)))
                {
                    handler();
                }
            };

            render_popup_button(0, is_cn ? "\u53d6\u6d88" : "Cancel", [&]()
                                {
                ImGui::CloseCurrentPopup();
                kodi_popup_focus_index_ = 0;
                kodi_popup_focus_dirty_ = true; });

            render_popup_button(1, is_cn ? "\u786e\u8ba4" : "Confirm", [&]()
                                {
                std::system("bash -lc 'systemctl stop amldigitalfpv || true; systemctl start kodi2'"); // restart kodi and exit
                running_flag = false;
                ImGui::CloseCurrentPopup();
                kodi_popup_focus_index_ = 0;
                kodi_popup_focus_dirty_ = true; });
            ImGui::EndPopup();
        }

        if (update_popup_pending_)
        {
            ImGui::OpenPopup("confirm_update");
            update_popup_pending_ = false;
        }
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("confirm_update", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            const char *msg = is_cn ? "\u53d1\u73b0\u66f4\u65b0\u5305\uff0c\u662f\u5426\u5f00\u59cb\u66f4\u65b0\uff1f"
                                    : "Update package found. Start update?";
            ImGui::TextWrapped("%s", msg);
            if (!update_path_.empty())
            {
                ImGui::TextWrapped("%s", update_path_.c_str());
            }
            ImGui::Spacing();
            const float button_width = 140.0f;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float total_width = button_width * 2.0f + spacing;
            const float region_width = ImGui::GetContentRegionAvail().x;
            const float base_x = ImGui::GetCursorPosX();
            if (region_width > total_width)
            {
                ImGui::SetCursorPosX(base_x + (region_width - total_width) * 0.5f);
            }
            if (ImGui::Button(is_cn ? "\u53d6\u6d88" : "Cancel", ImVec2(button_width, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(is_cn ? "\u786e\u8ba4" : "Confirm", ImVec2(button_width, 0)))
            {
                if (start_update_)
                {
                    start_update_(update_path_);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (update_missing_popup_pending_)
        {
            ImGui::OpenPopup("update_not_found");
            update_missing_popup_pending_ = false;
        }
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("update_not_found", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            const char *msg = is_cn ? "\u672a\u627e\u5230\u66f4\u65b0\u5305" : "Update package not found";
            const float button_width = 120.0f;
            const float region_width = ImGui::GetContentRegionAvail().x;
            const float base_x = ImGui::GetCursorPosX();
            const float text_width = ImGui::CalcTextSize(msg).x;
            if (region_width > text_width)
            {
                ImGui::SetCursorPosX(base_x + (region_width - text_width) * 0.5f);
            }
            ImGui::TextWrapped("%s", msg);
            ImGui::Spacing();
            if (region_width > button_width)
            {
                ImGui::SetCursorPosX(base_x + (region_width - button_width) * 0.5f);
            }
            if (ImGui::Button(is_cn ? "\u786e\u5b9a" : "OK", ImVec2(button_width, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (update_failed_popup_pending_)
        {
            ImGui::OpenPopup("update_failed");
            update_failed_popup_pending_ = false;
        }
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("update_failed", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            const char *msg = is_cn ? "\u66f4\u65b0\u5931\u8d25" : "Update failed";
            ImGui::TextWrapped("%s", msg);
            ImGui::Spacing();
            if (ImGui::Button(is_cn ? "\u786e\u5b9a" : "OK", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (update_reboot_popup_pending_)
        {
            ImGui::OpenPopup("update_reboot");
            update_reboot_popup_pending_ = false;
        }
        if (update_status_)
        {
            const int status = update_status_();
            if (status == 1 && !ImGui::IsPopupOpen("update_in_progress"))
            {
                ImGui::OpenPopup("update_in_progress");
            }
        }
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("update_reboot", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            const char *msg = is_cn ? "\u66f4\u65b0\u5b8c\u6210\uff0c\u662f\u5426\u91cd\u542f\uff1f"
                                    : "Update completed. Reboot now?";
            ImGui::TextWrapped("%s", msg);
            ImGui::Spacing();
            const float button_width = 140.0f;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float total_width = button_width * 2.0f + spacing;
            const float region_width = ImGui::GetContentRegionAvail().x;
            const float base_x = ImGui::GetCursorPosX();
            if (region_width > total_width)
            {
                ImGui::SetCursorPosX(base_x + (region_width - total_width) * 0.5f);
            }
            if (ImGui::Button(is_cn ? "\u7a0d\u540e" : "Later", ImVec2(button_width, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(is_cn ? "\u91cd\u542f" : "Reboot", ImVec2(button_width, 0)))
            {
                if (request_reboot_)
                {
                    request_reboot_();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("update_in_progress", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            if (update_status_ && update_status_() != 1)
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                // Popup will be closed next frame once update completes.
            }
            else
            {
            const char *msg = is_cn ? "\u6b63\u5728\u66f4\u65b0\uff0c\u8bf7\u7a0d\u5019..." : "Updating, please wait...";
            ImGui::TextWrapped("%s", msg);
            ImGui::EndPopup();
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(5);
}
