#include "menu_renderer.h"

#include "dock_icons.h"
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

static ImTextureID UploadIcon(const png_byte *pixels, png_uint_32 width, png_uint_32 height)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return static_cast<ImTextureID>(tex);
}

template <std::size_t Size>
static ImTextureID LoadDockIcon(const unsigned char (&bytes)[Size], const char *name)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    ImTextureID texture{};
    if (png_image_begin_read_from_memory(&image, bytes, Size))
    {
        image.format = PNG_FORMAT_RGBA;
        std::vector<png_byte> pixels(PNG_IMAGE_SIZE(image));
        if (png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr))
            texture = UploadIcon(pixels.data(), image.width, image.height);
    }
    if (!texture)
        std::fprintf(stderr, "[AMLgsMenu] Could not load Dock icon %s: %s\n", name, image.message);
    png_image_free(&image);
    return texture;
}

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
                           std::function<void()> request_reboot,
                           std::function<bool(const std::string &, const std::string &)> apply_wifi_client,
                           std::string wifi_client_ssid, std::string wifi_client_password)
    : state_(state), use_mock_(use_mock), telemetry_provider_(std::move(provider)),
      toggle_terminal_(std::move(toggle_terminal)), terminal_visible_(std::move(terminal_visible)),
      start_update_(std::move(start_update)), update_status_(std::move(update_status)),
      request_reboot_(std::move(request_reboot)),
      apply_wifi_client_(std::move(apply_wifi_client))
{
    std::snprintf(wifi_client_ssid_, sizeof(wifi_client_ssid_), "%s", wifi_client_ssid.c_str());
    std::snprintf(wifi_client_password_, sizeof(wifi_client_password_), "%s", wifi_client_password.c_str());
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
    dock_icons_ = {
        LoadDockIcon(dock_icons::link, "link"),
        LoadDockIcon(dock_icons::display, "display"),
        LoadDockIcon(dock_icons::network, "network"),
        LoadDockIcon(dock_icons::audio, "audio"),
        LoadDockIcon(dock_icons::record, "record"),
        LoadDockIcon(dock_icons::terminal, "terminal"),
        LoadDockIcon(dock_icons::system, "system"),
    };
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

    out_id = UploadIcon(data.data(), width, height);
    out_w = static_cast<int>(width);
    out_h = static_cast<int>(height);
    return true;
}
MenuRenderer::~MenuRenderer()
{
    GLuint tex_ids[8 + std::tuple_size_v<decltype(dock_icons_)>]{};
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
    collect(icon_speed_);
    collect(icon_monitor_);
    collect(icon_temp_air_);
    collect(icon_temp_ground_);
    for (ImTextureID icon : dock_icons_)
        collect(icon);
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
    const bool ap_mode = state_.NetworkModeIndex() == static_cast<int>(MenuState::NetworkMode::ConnectAp);
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

    if (!use_mock_ && (ap_mode || data.has_wifi_monitor))
    {
        char wifi_buf[64];
        if (ap_mode)
        {
            if (data.has_ap_signal)
                std::snprintf(wifi_buf, sizeof(wifi_buf), "WIFI %d dBm", static_cast<int>(data.ap_signal_dbm));
            else
                std::snprintf(wifi_buf, sizeof(wifi_buf), "WIFI DISCONNECTED");
        }
        else
        {
            std::snprintf(wifi_buf, sizeof(wifi_buf), "%d WIFI PLUGGED", data.wifi_monitor_count);
        }
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

    if (!ap_mode && (data.has_rc_signal || data.ground_signal_a != 0.0f || data.ground_signal_b != 0.0f))
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
    const float animation_dt = std::min(0.05f, ImGui::GetIO().DeltaTime);
    const bool is_cn = state_.GetLanguage() == MenuState::Language::CN;
    const bool opened_this_frame = !menu_visible_last_;
    bool kodi_popup_requested = false;
    int rendered_category_this_frame = -1;

    if (opened_this_frame)
    {
        last_focus_index_ = -1;
        if (active_category_ < 0)
        {
            active_category_ = 0;
        }
        pending_focus_index_ = 0;
        last_rendered_category_ = -1;
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

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(18, 20, 25, 215));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 38));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(245, 245, 248, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(32, 36, 44, 200));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(50, 56, 68, 240));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(235, 155, 35, 230));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 45, 56, 220));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(58, 65, 80, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(235, 155, 35, 230));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, IM_COL32(245, 166, 35, 255));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(22, 25, 32, 245));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(235, 155, 35, 75));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(235, 155, 35, 130));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(235, 155, 35, 200));

    const bool popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

    // ==========================================
    // Dock (Bottom Bar - macOS style, icon-only with magnification)
    // ==========================================
    const int dock_category[] = {0, 1, 2, 3, -1, -1, 4};
    const float dock_item_width = 54.0f;
    const float dock_item_height = 54.0f;
    const float dock_spacing = 8.0f;
    const float dock_pad_x = 16.0f;
    const float dock_pad_y = 10.0f;
    const float dock_width = dock_item_width * 7.0f + dock_spacing * 6.0f + dock_pad_x * 2.0f;
    const float dock_height = dock_item_height + dock_pad_y * 2.0f;
    const ImVec2 dock_pos(viewport->Pos.x + viewport->Size.x * 0.5f,
                          viewport->Pos.y + viewport->Size.y - 18.0f);

    ImGui::SetNextWindowPos(dock_pos, ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(dock_width, dock_height), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);

    // Dock shadow on background draw list
    ImDrawList *bg_draw_dock = ImGui::GetBackgroundDrawList();
    const ImVec2 dock_rect_min(dock_pos.x - dock_width * 0.5f, dock_pos.y - dock_height);
    const ImVec2 dock_rect_max(dock_pos.x + dock_width * 0.5f, dock_pos.y);
    for (int layer = 4; layer >= 1; --layer)
    {
        const float sp = static_cast<float>(layer) * 3.0f;
        bg_draw_dock->AddRect(ImVec2(dock_rect_min.x - sp, dock_rect_min.y - sp),
                              ImVec2(dock_rect_max.x + sp, dock_rect_max.y + sp),
                              IM_COL32(0, 0, 0, 50 / layer),
                              22.0f + sp, 0, sp);
    }

    if (opened_this_frame || (settings_visible_last_ && active_category_ < 0))
    {
        ImGui::SetNextWindowFocus();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 22.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dock_pad_x, dock_pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dock_spacing, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 24, 34, 250));

    if (ImGui::Begin("AMLgsMenu Dock", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
    {
        if (opened_this_frame || (settings_visible_last_ && active_category_ < 0))
        {
            ImGui::SetKeyboardFocusHere();
        }

        const bool terminal_open = terminal_visible_ && terminal_visible_();
        if (active_category_ < 0 && !popup_open && !terminal_open && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            state_.ToggleMenuVisibility();
        }

        ImDrawList *dock_draw = ImGui::GetWindowDrawList();

        for (int i = 0; i < 7; ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0f, dock_spacing);
            }
            ImGui::PushID(i);

            const bool is_cat_active = (dock_category[i] >= 0 && active_category_ == dock_category[i]);
            const ImVec2 item_size(dock_item_width, dock_item_height);
            const bool pressed = ImGui::InvisibleButton("dock_btn", item_size, ImGuiButtonFlags_EnableNav);
            const bool focused = ImGui::IsItemFocused();
            const bool hovered = ImGui::IsItemHovered();

            // macOS magnification target calculation
            float target_hover = 0.0f;
            if (hovered || focused)
            {
                target_hover = 1.0f;
            }
            else if (is_cat_active)
            {
                target_hover = 0.35f;
            }

            const float hover_step = animation_dt * 14.0f;
            dock_hover_amount_[i] += (target_hover - dock_hover_amount_[i]) * std::min(1.0f, hover_step);

            const ImVec2 item_min = ImGui::GetItemRectMin();
            const ImVec2 item_max = ImGui::GetItemRectMax();

            // Background pill on active/hover
            if (is_cat_active)
            {
                dock_draw->AddRectFilled(item_min, item_max, IM_COL32(245, 166, 35, 55), 14.0f);
                dock_draw->AddRect(item_min, item_max, IM_COL32(245, 166, 35, 200), 14.0f, 0, 1.5f);
            }
            else if (hovered || focused)
            {
                dock_draw->AddRectFilled(item_min, item_max, IM_COL32(255, 255, 255, 32), 14.0f);
            }

            // Keyboard navigation focus border
            if (focused)
            {
                dock_draw->AddRect(item_min, item_max, IM_COL32(245, 166, 35, 240), 14.0f, 0, 2.0f);
            }

            // Draw Icon with macOS smooth magnification scale (up to 1.35x, expanding upwards)
            const float base_icon_size = 36.0f;
            const float scale = 1.0f + dock_hover_amount_[i] * 0.35f;
            const float icon_size = base_icon_size * scale;
            const float center_x = item_min.x + dock_item_width * 0.5f;
            const float center_y = item_min.y + dock_item_height * 0.5f - (dock_hover_amount_[i] * 5.0f);
            const ImVec2 icon_min(center_x - icon_size * 0.5f, center_y - icon_size * 0.5f);
            const ImVec2 icon_max(center_x + icon_size * 0.5f, center_y + icon_size * 0.5f);

            if (dock_icons_[i])
            {
                dock_draw->AddImage(dock_icons_[i], icon_min, icon_max);
            }

            // Recording badge: pulsing glowing red dot on record button
            if (i == 4 && state_.Recording())
            {
                const ImVec2 red_dot(icon_max.x - 2.0f, icon_min.y + 4.0f);
                dock_draw->AddCircleFilled(red_dot, 4.5f, IM_COL32(255, 59, 48, 255));
                dock_draw->AddCircle(red_dot, 6.5f, IM_COL32(255, 59, 48, 140), 16, 1.5f);
            }

            // Active category macOS-style dot indicator at bottom
            if (is_cat_active)
            {
                const ImVec2 dot_pos(center_x, item_max.y - 4.0f);
                dock_draw->AddCircleFilled(dot_pos, 2.5f, IM_COL32(245, 166, 35, 255));
                dock_draw->AddCircle(dot_pos, 4.0f, IM_COL32(245, 166, 35, 140), 12, 1.0f);
            }

            if (pressed)
            {
                if (i == 4)
                {
                    state_.ToggleRecording();
                }
                else if (i == 5)
                {
                    if (toggle_terminal_)
                        toggle_terminal_();
                }
                else
                {
                    active_category_ = dock_category[i];
                    last_focus_index_ = -1;
                    pending_focus_index_ = 0;
                }
            }
            ImGui::PopID();
        }

        // Draw fully closed crisp outer border for Dock
        const ImVec2 d_min = ImGui::GetWindowPos();
        const ImVec2 d_max(d_min.x + dock_width, d_min.y + dock_height);
        dock_draw->AddRect(d_min, d_max, IM_COL32(255, 255, 255, 75), 22.0f, 0, 1.5f);
    }
    ImGui::End();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(4);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings;

    if (active_category_ >= 0)
    {
        settings_open_amount_ += (1.0f - settings_open_amount_) * std::min(1.0f, animation_dt * 12.0f);
        const ImVec2 base_menu_size(std::min(840.0f, viewport->Size.x * 0.84f),
                                    std::min(520.0f, viewport->Size.y * 0.68f));
        const float open_scale = 0.96f + settings_open_amount_ * 0.04f;
        const ImVec2 menu_size(base_menu_size.x * open_scale, base_menu_size.y * open_scale);
        const ImVec2 menu_pos(viewport->Pos.x + viewport->Size.x * 0.5f,
                              viewport->Pos.y + viewport->Size.y * (0.42f + (1.0f - settings_open_amount_) * 0.015f));

        ImGui::SetNextWindowBgAlpha(0.96f);
        ImGui::SetNextWindowPos(menu_pos, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(menu_size, ImGuiCond_Always);

        // Soft ambient back-glow
        ImDrawList *bg_draw = ImGui::GetBackgroundDrawList();
        const ImVec2 shadow_min(menu_pos.x - menu_size.x * 0.5f, menu_pos.y - menu_size.y * 0.5f);
        const ImVec2 shadow_max(menu_pos.x + menu_size.x * 0.5f, menu_pos.y + menu_size.y * 0.5f);
        for (int layer = 5; layer >= 1; --layer)
        {
            const float spread = static_cast<float>(layer) * 3.0f;
            bg_draw->AddRect(ImVec2(shadow_min.x - spread, shadow_min.y - spread),
                             ImVec2(shadow_max.x + spread, shadow_max.y + spread),
                             IM_COL32(0, 0, 0, 10 + layer * 6),
                             16.0f + spread, 0, spread);
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 24, 34, 252));

        if (ImGui::Begin("AMLgsMenu Settings", nullptr, flags))
        {
            const int rendered_category = active_category_;
            rendered_category_this_frame = rendered_category;

            const bool terminal_open = terminal_visible_ && terminal_visible_();
            if (!popup_open && !terminal_open && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                state_.ToggleMenuVisibility();
            }

            if (rendered_category != last_rendered_category_)
            {
                last_focus_index_ = -1;
                pending_focus_index_ = rendered_category >= 0 ? 0 : -1;
            }

            // Left / Right arrow navigation to switch tabs smoothly
            const bool nav_tab_next = ImGui::IsKeyPressed(ImGuiKey_PageDown, false) ||
                                      (!ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false));
            const bool nav_tab_prev = ImGui::IsKeyPressed(ImGuiKey_PageUp, false) ||
                                      (!ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false));
            if (!popup_open && (nav_tab_next || nav_tab_prev))
            {
                if (nav_tab_next)
                    active_category_ = (active_category_ + 1) % 5;
                else if (nav_tab_prev)
                    active_category_ = (active_category_ + 4) % 5;
                pending_focus_index_ = 0;
            }

            const bool nav_down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false);
            const bool nav_up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false);
            if (!popup_open && (nav_down || nav_up) && last_focus_index_ < 0 && pending_focus_index_ < 0)
            {
                pending_focus_index_ = 0;
                ImGuiIO &io = ImGui::GetIO();
                const ImGuiKey key = nav_down ? ImGuiKey_DownArrow : ImGuiKey_UpArrow;
                ImGuiKeyData &key_data = io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
                key_data.Down = false;
                key_data.DownDuration = -1.0f;
                key_data.DownDurationPrev = -1.0f;
            }

            int focus_index = 0;
            int focused_index = -1;
            std::vector<int> focus_items;
            auto register_focus = [&](const std::function<void()> &draw)
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
                focus_items.push_back(focus_index);
                ++focus_index;
            };

            const char *category_titles_cn[] = {"图传设置", "画面设置", "网络设置", "声音设置", "系统设置"};
            const char *category_titles_en[] = {"Link Settings", "Display Settings", "Network Settings", "Audio Settings", "System Settings"};
            const char *category_names_cn[] = {"图传", "画面", "网络", "声音", "系统"};
            const char *category_names_en[] = {"Link", "Display", "Network", "Audio", "System"};

            const ImVec2 window_pos = ImGui::GetWindowPos();
            const float window_w = ImGui::GetWindowWidth();
            const float window_h = ImGui::GetWindowHeight();
            const float header_h = 52.0f;
            const float sidebar_w = 175.0f;

            ImDrawList *win_draw = ImGui::GetWindowDrawList();

            // Header background solid fill & sharp separator line
            win_draw->AddRectFilled(window_pos, ImVec2(window_pos.x + window_w, window_pos.y + header_h),
                                    IM_COL32(30, 36, 50, 255), 16.0f, ImDrawFlags_RoundCornersTop);
            win_draw->AddLine(ImVec2(window_pos.x + 1.0f, window_pos.y + header_h),
                              ImVec2(window_pos.x + window_w - 1.0f, window_pos.y + header_h),
                              IM_COL32(255, 255, 255, 60), 1.5f);

            // Header Icon & Title
            const ImTextureID cat_icon = dock_icons_[rendered_category == 4 ? 6 : rendered_category];
            const float header_icon_size = 24.0f;
            const float header_center_y = window_pos.y + header_h * 0.5f;
            if (cat_icon)
            {
                win_draw->AddImage(cat_icon,
                                   ImVec2(window_pos.x + 18.0f, header_center_y - header_icon_size * 0.5f),
                                   ImVec2(window_pos.x + 18.0f + header_icon_size, header_center_y + header_icon_size * 0.5f));
            }
            const char *cat_title = is_cn ? category_titles_cn[rendered_category] : category_titles_en[rendered_category];
            const ImVec2 cat_title_sz = ImGui::CalcTextSize(cat_title);
            win_draw->AddText(ImVec2(window_pos.x + 50.0f, header_center_y - cat_title_sz.y * 0.5f),
                              IM_COL32(255, 255, 255, 255), cat_title);

            // Header Close / Back button
            const char *back_label = is_cn ? "ESC  关闭" : "ESC  Close";
            const ImVec2 back_btn_size(105.0f, 32.0f);
            ImGui::SetCursorPos(ImVec2(window_w - back_btn_size.x - 16.0f, 10.0f));
            if (ImGui::Button(back_label, back_btn_size))
            {
                state_.ToggleMenuVisibility();
            }

            // Sidebar background solid fill
            win_draw->AddRectFilled(ImVec2(window_pos.x, window_pos.y + header_h),
                                    ImVec2(window_pos.x + sidebar_w, window_pos.y + window_h),
                                    IM_COL32(15, 18, 26, 255), 16.0f, ImDrawFlags_RoundCornersBottomLeft);

            // Vertical divider between sidebar and content
            win_draw->AddLine(ImVec2(window_pos.x + sidebar_w, window_pos.y + header_h),
                              ImVec2(window_pos.x + sidebar_w, window_pos.y + window_h - 1.0f),
                              IM_COL32(255, 255, 255, 50), 1.5f);

            // Sidebar Categories
            ImGui::SetCursorPos(ImVec2(10.0f, header_h + 12.0f));
            ImGui::BeginGroup();
            for (int i = 0; i < 5; ++i)
            {
                ImGui::PushID(200 + i);
                const bool is_active = (rendered_category == i);
                const char *label = is_cn ? category_names_cn[i] : category_names_en[i];
                const ImTextureID icon = dock_icons_[i == 4 ? 6 : i];

                const ImVec2 item_size(sidebar_w - 20.0f, 44.0f);
                const bool pressed = ImGui::InvisibleButton("cat_item", item_size, ImGuiButtonFlags_EnableNav);
                const bool item_focused = ImGui::IsItemFocused();
                if (pressed || (item_focused && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false))))
                {
                    active_category_ = i;
                    pending_focus_index_ = 0;
                }
                const bool hovered = ImGui::IsItemHovered();
                const ImVec2 item_min = ImGui::GetItemRectMin();
                const ImVec2 item_max = ImGui::GetItemRectMax();

                if (is_active)
                {
                    win_draw->AddRectFilled(item_min, item_max, IM_COL32(245, 166, 35, 55), 10.0f);
                    win_draw->AddRect(item_min, item_max, IM_COL32(245, 166, 35, 200), 10.0f, 0, 1.5f);
                    win_draw->AddRectFilled(ImVec2(item_min.x + 3.0f, item_min.y + 9.0f),
                                            ImVec2(item_min.x + 6.5f, item_max.y - 9.0f),
                                            IM_COL32(245, 166, 35, 255), 1.5f);
                }
                else if (hovered || item_focused)
                {
                    win_draw->AddRectFilled(item_min, item_max, IM_COL32(255, 255, 255, 24), 10.0f);
                }
                if (item_focused)
                {
                    win_draw->AddRect(item_min, item_max, IM_COL32(245, 166, 35, 240), 10.0f, 0, 2.0f);
                }

                const float item_center_y = item_min.y + item_size.y * 0.5f;
                const float sb_icon_sz = 24.0f;
                if (icon)
                {
                    win_draw->AddImage(icon,
                                       ImVec2(item_min.x + 12.0f, item_center_y - sb_icon_sz * 0.5f),
                                       ImVec2(item_min.x + 12.0f + sb_icon_sz, item_center_y + sb_icon_sz * 0.5f));
                }
                const ImVec2 label_sz = ImGui::CalcTextSize(label);
                win_draw->AddText(ImVec2(item_min.x + 44.0f, item_center_y - label_sz.y * 0.5f),
                                  is_active ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 202, 208, 220),
                                  label);
                ImGui::PopID();
                ImGui::Spacing();
            }
            ImGui::EndGroup();

            // Right Content Area
            const float content_x = sidebar_w + 14.0f;
            const float content_y = header_h + 12.0f;
            const float content_w = window_w - sidebar_w - 24.0f;
            const float content_h = window_h - header_h - 20.0f;
            ImGui::SetCursorPos(ImVec2(content_x, content_y));
            ImGui::BeginChild("settings_content", ImVec2(content_w, content_h), false, ImGuiWindowFlags_NoScrollbar);

            const auto &channels = state_.Channels();
            const auto &bandwidths = state_.Bandwidths();
            const auto &sky_modes = state_.SkyModes();
            const auto &ground_modes = state_.GroundModes();
            const auto &powers = state_.PowerLevels();
            const auto &mcs_levels = state_.McsLevels();
            const auto &bitrates = state_.Bitrates();
            const auto &volume_levels = state_.VolumeLevels();
            const auto &buffer_levels = state_.BufferLevels();
            const auto &ack_time_levels = state_.AckTimeLevels();
            const auto &hdmi_attrs = state_.HdmiAttrs();
            const auto &network_modes = state_.NetworkModes();

            if (ImGui::BeginTable("settings_grid", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
            {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, 250.0f);

                int row_counter = 0;
                auto render_setting_row = [&](const char *label_text, const std::function<void()> &control_func, bool focusable = true)
                {
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, 44.0f);
                    const bool is_even = (row_counter % 2 == 0);

                    // Visible card background
                    const ImU32 row_bg = is_even ? IM_COL32(255, 255, 255, 14) : IM_COL32(255, 255, 255, 6);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, row_bg);

                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 238, 245, 255));
                    ImGui::TextUnformatted(label_text);
                    ImGui::PopStyleColor();

                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-1.0f);
                    if (focusable)
                    {
                        register_focus(control_func);
                    }
                    else
                    {
                        control_func();
                    }

                    ++row_counter;
                };

                // ---- Category 0: 图传 / Link ----
                if (rendered_category == 0)
                {
                    render_setting_row(is_cn ? "信道" : "Channel", [&]
                    {
                        if (ImGui::BeginCombo("##channel", std::to_string(channels[state_.ChannelIndex()]).c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(channels.size()); ++i)
                            {
                                bool selected = (state_.ChannelIndex() == i);
                                if (ImGui::Selectable(std::to_string(channels[i]).c_str(), selected))
                                    state_.SetChannelIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "频宽" : "Bandwidth", [&]
                    {
                        if (ImGui::BeginCombo("##bandwidth", bandwidths[state_.BandwidthIndex()]))
                        {
                            for (int i = 0; i < static_cast<int>(bandwidths.size()); ++i)
                            {
                                bool selected = (state_.BandwidthIndex() == i);
                                if (ImGui::Selectable(bandwidths[i], selected))
                                    state_.SetBandwidthIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "天空端发射功率" : "Air TX Power", [&]
                    {
                        if (ImGui::BeginCombo("##sky_power", std::to_string(powers[state_.SkyPowerIndex()]).c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(powers.size()); ++i)
                            {
                                bool selected = (state_.SkyPowerIndex() == i);
                                if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected))
                                    state_.SetSkyPowerIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "地面端发射功率" : "Ground TX Power", [&]
                    {
                        if (ImGui::BeginCombo("##ground_power", std::to_string(powers[state_.GroundPowerIndex()]).c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(powers.size()); ++i)
                            {
                                bool selected = (state_.GroundPowerIndex() == i);
                                if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected))
                                    state_.SetGroundPowerIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "天空端MCS" : "Air MCS", [&]
                    {
                        if (mcs_levels.empty())
                        {
                            ImGui::TextUnformatted("--");
                            return;
                        }
                        const int current_mcs = mcs_levels[state_.SkyMcsIndex()];
                        if (ImGui::BeginCombo("##sky_mcs", std::to_string(current_mcs).c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(mcs_levels.size()); ++i)
                            {
                                bool selected = (state_.SkyMcsIndex() == i);
                                if (ImGui::Selectable(std::to_string(mcs_levels[i]).c_str(), selected))
                                    state_.SetSkyMcsIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "自适应链路" : "Adaptive Link", [&]
                    {
                        bool enabled = state_.AdaptiveLinkEnabled();
                        if (ImGui::Checkbox("##adaptive_link", &enabled))
                        {
                            state_.SetAdaptiveLinkEnabled(enabled);
                        }
                    });

                    render_setting_row(is_cn ? "ACK时间" : "ACK Time", [&]
                    {
                        if (ack_time_levels.empty())
                        {
                            ImGui::TextUnformatted("--");
                            return;
                        }
                        int idx = state_.AckTimeIndex();
                        if (idx < 0 || idx >= static_cast<int>(ack_time_levels.size()))
                            idx = 0;
                        const std::string label = std::to_string(ack_time_levels[idx]);
                        if (ImGui::BeginCombo("##ack_time", label.c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(ack_time_levels.size()); ++i)
                            {
                                bool selected = (idx == i);
                                if (ImGui::Selectable(std::to_string(ack_time_levels[i]).c_str(), selected))
                                    state_.SetAckTimeIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "坏包策略" : "Bad Frame Policy", [&]
                    {
                        const char *label_cn[] = {"丢包", "忽略"};
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
                                    state_.SetBadFrameIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });
                }

                // ---- Category 1: 画面 / Display ----
                else if (rendered_category == 1)
                {
                    render_setting_row(is_cn ? "天空端分辨率/刷新率" : "Air Res/Refresh", [&]
                    {
                        if (!sky_modes.empty() && ImGui::BeginCombo("##sky_mode", FormatVideoModeLabel(sky_modes[state_.SkyModeIndex()]).c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(sky_modes.size()); ++i)
                            {
                                bool selected = (state_.SkyModeIndex() == i);
                                if (ImGui::Selectable(FormatVideoModeLabel(sky_modes[i]).c_str(), selected))
                                    state_.SetSkyModeIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "地面端分辨率/刷新率" : "Ground Res/Refresh", [&]
                    {
                        if (!ground_modes.empty() && ImGui::BeginCombo("##ground_mode", FormatVideoModeLabel(ground_modes[state_.GroundModeIndex()]).c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(ground_modes.size()); ++i)
                            {
                                bool selected = (state_.GroundModeIndex() == i);
                                if (ImGui::Selectable(FormatVideoModeLabel(ground_modes[i]).c_str(), selected))
                                {
                                    const auto &mode = ground_modes[i];
                                    const bool requires_warning = (mode.refresh > 60) && !state_.IsGroundModePersisted(mode.label);
                                    if (requires_warning)
                                    {
                                        pending_high_refresh_index_ = i;
                                        pending_high_refresh_label_ = mode.label;
                                        high_refresh_popup_pending_ = true;
                                    }
                                    else
                                    {
                                        state_.SetGroundModeIndex(i);
                                    }
                                }
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "码率 (Mbps)" : "Bitrate (Mbps)", [&]
                    {
                        if (ImGui::BeginCombo("##bitrate", std::to_string(bitrates[state_.BitrateIndex()]).c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(bitrates.size()); ++i)
                            {
                                bool selected = (state_.BitrateIndex() == i);
                                if (ImGui::Selectable(std::to_string(bitrates[i]).c_str(), selected))
                                    state_.SetBitrateIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "缓存级别" : "Buffer Level", [&]
                    {
                        if (buffer_levels.empty())
                        {
                            ImGui::TextUnformatted("--");
                            return;
                        }
                        int idx = state_.BufferLevelIndex();
                        if (idx < 0 || idx >= static_cast<int>(buffer_levels.size()))
                            idx = 0;
                        const std::string label = std::to_string(buffer_levels[idx]);
                        if (ImGui::BeginCombo("##buffer_level", label.c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(buffer_levels.size()); ++i)
                            {
                                bool selected = (idx == i);
                                if (ImGui::Selectable(std::to_string(buffer_levels[i]).c_str(), selected))
                                    state_.SetBufferLevelIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "HDMI色彩格式" : "HDMI Colorspace", [&]
                    {
                        if (hdmi_attrs.empty())
                        {
                            ImGui::TextUnformatted("--");
                            return;
                        }
                        int idx = state_.HdmiAttrIndex();
                        if (idx < 0 || idx >= static_cast<int>(hdmi_attrs.size()))
                            idx = 0;
                        const std::string value = hdmi_attrs[idx];
                        const char *label = (value == "rgb") ? "RGB" : ((value == "422") ? "YUV422" : "YUV420");
                        if (ImGui::BeginCombo("##hdmi_attr", label))
                        {
                            for (int i = 0; i < static_cast<int>(hdmi_attrs.size()); ++i)
                            {
                                const std::string item_value = hdmi_attrs[i];
                                const char *item_label = (item_value == "rgb") ? "RGB" : ((item_value == "422") ? "YUV422" : "YUV420");
                                bool selected = (idx == i);
                                if (ImGui::Selectable(item_label, selected))
                                {
                                    if (i != idx)
                                    {
                                        pending_hdmi_attr_index_ = i;
                                        pending_hdmi_attr_value_ = item_value;
                                        hdmi_attr_popup_pending_ = true;
                                    }
                                }
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });
                }

                // ---- Category 2: 网络 / Network ----
                else if (rendered_category == 2)
                {
                    render_setting_row(is_cn ? "网络模式" : "Network mode", [&]
                    {
                        if (network_modes.empty())
                        {
                            ImGui::TextUnformatted("--");
                            return;
                        }
                        int idx = state_.NetworkModeIndex();
                        if (idx < 0 || idx >= static_cast<int>(network_modes.size()))
                            idx = 0;
                        const char *label = (idx == static_cast<int>(MenuState::NetworkMode::ConnectAp))
                                                ? (is_cn ? "连接热点" : "Connect AP")
                                                : "WFB";
                        if (ImGui::BeginCombo("##network_mode", label))
                        {
                            for (int i = 0; i < static_cast<int>(network_modes.size()); ++i)
                            {
                                const char *item_label = (i == static_cast<int>(MenuState::NetworkMode::ConnectAp))
                                                             ? (is_cn ? "连接热点" : "Connect AP")
                                                             : "WFB";
                                bool selected = (idx == i);
                                if (ImGui::Selectable(item_label, selected))
                                {
                                    if (i == static_cast<int>(MenuState::NetworkMode::ConnectAp))
                                    {
                                        wifi_client_popup_pending_ = true;
                                    }
                                    else if (i != idx)
                                    {
                                        state_.SetNetworkModeIndex(i);
                                    }
                                }
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "热点配置" : "AP Config", [&]
                    {
                        const char *label = is_cn ? "编辑热点配置" : "Edit AP Config";
                        if (ImGui::Button(label, ImVec2(-1, 0)))
                        {
                            wifi_client_popup_pending_ = true;
                        }
                    });

                    const int net_idx = state_.NetworkModeIndex();
                    const bool is_ap = (net_idx == static_cast<int>(MenuState::NetworkMode::ConnectAp));
                    render_setting_row(is_cn ? "连接状态" : "Status", [&]
                    {
                        if (is_ap)
                        {
                            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), "%s",
                                               is_cn ? "热点模式 (Wi-Fi 客户端)" : "AP Mode (Client)");
                        }
                        else
                        {
                            ImGui::TextColored(ImVec4(0.40f, 0.75f, 1.0f, 1.0f), "%s",
                                               is_cn ? "WFB 专网图传工作模式" : "WFB Dedicated Link");
                        }
                    }, false);
                }

                // ---- Category 3: 声音 / Audio ----
                else if (rendered_category == 3)
                {
                    render_setting_row(is_cn ? "声音开关" : "Sound", [&]
                    {
                        bool enabled = state_.SoundEnabled();
                        if (ImGui::Checkbox("##sound_enable", &enabled))
                        {
                            state_.SetSoundEnabled(enabled);
                        }
                    });

                    render_setting_row(is_cn ? "音量" : "Volume", [&]
                    {
                        if (volume_levels.empty())
                        {
                            ImGui::TextUnformatted("--");
                            return;
                        }
                        int volume_index = state_.SoundVolumeIndex();
                        if (volume_index < 0 || volume_index >= static_cast<int>(volume_levels.size()))
                            volume_index = std::max(0, std::min(static_cast<int>(volume_levels.size()) - 1, volume_index));
                        const std::string label = std::to_string(volume_levels[volume_index]) + "%";
                        if (ImGui::BeginCombo("##sound_volume", label.c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(volume_levels.size()); ++i)
                            {
                                const std::string item_label = std::to_string(volume_levels[i]) + "%";
                                bool selected = (volume_index == i);
                                if (ImGui::Selectable(item_label.c_str(), selected))
                                    state_.SetSoundVolumeIndex(i);
                                if (selected)
                                    ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    });
                }

                // ---- Category 4: 系统 / System ----
                else if (rendered_category == 4)
                {
                    render_setting_row(is_cn ? "固件模式" : "Firmware", [&]
                    {
                        const bool is_cc = state_.GetFirmwareType() == MenuState::FirmwareType::CCEdition;
                        const char *label = is_cc ? "CC (UDP)" : (is_cn ? "官方 (SSH)" : "Official (SSH)");
                        if (ImGui::BeginCombo("##firmware", label))
                        {
                            if (ImGui::Selectable("CC (UDP)", is_cc))
                                state_.SetFirmwareType(MenuState::FirmwareType::CCEdition);
                            const bool official = !is_cc;
                            const char *official_label = is_cn ? "官方 (SSH)" : "Official (SSH)";
                            if (ImGui::Selectable(official_label, official))
                                state_.SetFirmwareType(MenuState::FirmwareType::Official);
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "系统语言" : "Language", [&]
                    {
                        const char *label = state_.GetLanguage() == MenuState::Language::CN ? "中文" : "English";
                        if (ImGui::BeginCombo("##lang", label))
                        {
                            if (ImGui::Selectable("中文", state_.GetLanguage() == MenuState::Language::CN))
                                state_.SetLanguage(MenuState::Language::CN);
                            if (ImGui::Selectable("English", state_.GetLanguage() == MenuState::Language::EN))
                                state_.SetLanguage(MenuState::Language::EN);
                            ImGui::EndCombo();
                        }
                    });

                    render_setting_row(is_cn ? "终端调试" : "Terminal", [&]
                    {
                        if (!toggle_terminal_)
                        {
                            ImGui::TextUnformatted("--");
                            return;
                        }
                        const char *label = (terminal_visible_ && terminal_visible_())
                                                ? (is_cn ? "关闭终端" : "Hide Terminal")
                                                : (is_cn ? "打开终端" : "Open Terminal");
                        if (ImGui::Button(label, ImVec2(-1, 0)))
                        {
                            toggle_terminal_();
                        }
                    });

                    render_setting_row(is_cn ? "视频录制" : "Recording", [&]
                    {
                        const char *label = state_.Recording()
                                                ? (is_cn ? "停止录像" : "Stop Recording")
                                                : (is_cn ? "开启录像" : "Start Recording");
                        if (ImGui::Button(label, ImVec2(-1, 0)))
                        {
                            state_.ToggleRecording();
                        }
                    });

                    render_setting_row(is_cn ? "固件更新" : "Firmware Update", [&]
                    {
                        if (ImGui::Button(is_cn ? "检查并更新" : "Check & Update", ImVec2(-1, 0)))
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

                    render_setting_row(is_cn ? "媒体中心" : "Media Center", [&]
                    {
                        if (ImGui::Button(is_cn ? "打开 KODI" : "Open KODI", ImVec2(-1, 0)))
                        {
                            kodi_popup_requested = true;
                        }
                    });

                    render_setting_row(is_cn ? "双系统切换" : "Dual Boot", [&]
                    {
                        if (ImGui::Button(is_cn ? "启动到安卓" : "Boot to Android", ImVec2(-1, 0)))
                        {
                            std::system("/sbin/rebootfromnand;reboot");
                            running_flag = false;
                        }
                    });
                }

                ImGui::EndTable();
            }
            ImGui::EndChild();

            // Up/Down navigation wrapping with deadlock recovery failsafe
            if (!popup_open && (nav_down || nav_up) && !focus_items.empty())
            {
                int next_idx = 0;
                if (focused_index >= 0)
                {
                    if (nav_down)
                        next_idx = (focused_index + 1) % static_cast<int>(focus_items.size());
                    else if (nav_up)
                        next_idx = (focused_index - 1 + static_cast<int>(focus_items.size())) % static_cast<int>(focus_items.size());
                }
                pending_focus_index_ = next_idx;
                ImGuiIO &io = ImGui::GetIO();
                const ImGuiKey key = nav_down ? ImGuiKey_DownArrow : ImGuiKey_UpArrow;
                ImGuiKeyData &key_data = io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
                key_data.Down = false;
                key_data.DownDuration = -1.0f;
                key_data.DownDurationPrev = -1.0f;
            }
            if (focused_index >= 0)
            {
                last_focus_index_ = focused_index;
            }

            // Draw crisp fully closed outer border for the Settings window
            win_draw->AddRect(window_pos, ImVec2(window_pos.x + window_w, window_pos.y + window_h),
                              IM_COL32(255, 255, 255, 75), 16.0f, 0, 1.5f);
        }
        ImGui::End();
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(3);
    }

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
        if (hdmi_attr_popup_pending_ && pending_hdmi_attr_index_ >= 0)
        {
            ImGui::OpenPopup("confirm_hdmi_attr");
            hdmi_attr_popup_pending_ = false;
        }
        if (hdmi_attr_persist_popup_pending_)
        {
            ImGui::OpenPopup("confirm_hdmi_attr_persist");
            hdmi_attr_persist_popup_pending_ = false;
        }
        if (wifi_client_popup_pending_)
        {
            ImGui::OpenPopup("configure_wifi_client");
            wifi_client_popup_pending_ = false;
        }

        ImGui::SetNextWindowSize(ImVec2(420.0f, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("confirm_high_refresh", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                pending_high_refresh_index_ = -1;
                pending_high_refresh_label_.clear();
                ImGui::CloseCurrentPopup();
            }
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
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                pending_high_refresh_label_.clear();
                ImGui::CloseCurrentPopup();
            }
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

        ImGui::SetNextWindowSize(ImVec2(420.0f, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("confirm_hdmi_attr", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                pending_hdmi_attr_index_ = -1;
                pending_hdmi_attr_value_.clear();
                ImGui::CloseCurrentPopup();
            }
            const char *msg = is_cn ? "\u5207\u6362 HDMI \u8272\u5f69\u683c\u5f0f\u53ef\u80fd\u5bfc\u81f4\u9ed1\u5c4f\uff0c\u662f\u5426\u7ee7\u7eed\uff1f"
                                    : "Switching HDMI colorspace may cause a black screen. Continue?";
            ImGui::TextWrapped("%s", msg);
            ImGui::Spacing();
            const float button_width = 130.0f;
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
                pending_hdmi_attr_index_ = -1;
                pending_hdmi_attr_value_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(is_cn ? "\u7ee7\u7eed" : "Continue", ImVec2(button_width, 0)))
            {
                if (pending_hdmi_attr_index_ >= 0)
                {
                    state_.RequestHdmiAttrSkipSaveOnce();
                    state_.SetHdmiAttrIndex(pending_hdmi_attr_index_);
                    pending_hdmi_attr_index_ = -1;
                }
                hdmi_attr_persist_popup_pending_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SetNextWindowSize(ImVec2(360.0f, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("confirm_hdmi_attr_persist", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                ImGui::CloseCurrentPopup();
            }
            const char *msg = is_cn ? "\u662f\u5426\u4ee5\u540e\u9ed8\u8ba4\u4f7f\u7528\u8be5\u8272\u5f69\u683c\u5f0f\uff1f"
                                    : "Use this colorspace by default in the future?";
            ImGui::TextWrapped("%s", msg);
            ImGui::Spacing();
            const float button_width = 120.0f;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float total_width = button_width * 2.0f + spacing;
            const float region_width = ImGui::GetContentRegionAvail().x;
            const float base_x = ImGui::GetCursorPosX();
            if (region_width > total_width)
            {
                ImGui::SetCursorPosX(base_x + (region_width - total_width) * 0.5f);
            }
            if (ImGui::Button(is_cn ? "\u5426" : "No", ImVec2(button_width, 0)))
            {
                pending_hdmi_attr_value_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(is_cn ? "\u662f" : "Yes", ImVec2(button_width, 0)))
            {
                state_.RequestHdmiAttrForceSaveOnce();
                state_.ForceHdmiAttrNotifyOnce();
                state_.SetHdmiAttrIndex(state_.HdmiAttrIndex());
                pending_hdmi_attr_value_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ================= Wi-Fi 连接向导 =================
        // 步骤 0：选网卡 -> 1：扫描/选 AP -> 2：输密码 -> 3：连接结果
        ImGui::SetNextWindowSize(ImVec2(560.0f, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("configure_wifi_client", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                wifi_step_ = 0;
                wifi_ifaces_.clear();
                wifi_connecting_ = false;
                ImGui::CloseCurrentPopup();
            }

            const char *title = is_cn ? "Wi-Fi 连接向导" : "Wi-Fi Connect Wizard";

            // 每帧刷新当前网卡的实时状态（轻量：读 iw link）
            wifi_status_timer_ += ImGui::GetIO().DeltaTime;
            if (wifi_status_timer_ > 1.0f)
            {
                wifi_status_timer_ = 0.0f;
                if (!wifi_.Iface().empty())
                    wifi_live_status_ = WifiController::QueryStatus(wifi_.Iface());
            }

            // 后台扫描结果
            {
                std::vector<WifiAp> got;
                bool scanning = false;
                if (wifi_.PollScan(&got, &scanning))
                    wifi_aps_ = std::move(got);
                wifi_scanning_ = scanning;
            }
            // 后台连接结果
            {
                bool ok = false, running = false;
                std::string msg;
                if (wifi_.PollConnect(&ok, &msg, &running))
                {
                    wifi_connecting_ = false;
                    wifi_result_ok_ = ok;
                    wifi_result_msg_ = msg;
                    wifi_has_result_ = true;
                    wifi_step_ = 3;
                }
                else
                {
                    wifi_connecting_ = running;
                }
            }

            ImGui::TextColored(ImVec4(0.96f, 0.65f, 0.14f, 1.0f), "●  %s", title);
            ImGui::Spacing();

            // ---- 步骤指示 ----
            {
                const char *steps_cn[] = {"1 选网卡", "2 选热点", "3 输密码", "4 连接"};
                const char *steps_en[] = {"1 Iface", "2 Select AP", "3 Password", "4 Connect"};
                const char **steps = is_cn ? steps_cn : steps_en;
                for (int i = 0; i < 4; ++i)
                {
                    if (i > 0)
                    {
                        ImGui::SameLine(0.0f, 8.0f);
                        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.50f, 1.0f), ">");
                        ImGui::SameLine(0.0f, 8.0f);
                    }
                    const bool cur = (i == wifi_step_);
                    const bool done = (i < wifi_step_);
                    if (cur)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(245, 166, 35, 45));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(245, 166, 35, 65));
                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
                        ImGui::SmallButton(steps[i]);
                        ImGui::PopStyleColor(3);
                    }
                    else if (done)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 180, 90, 35));
                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(80, 220, 120, 255));
                        ImGui::SmallButton(steps[i]);
                        ImGui::PopStyleColor(2);
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 255, 255, 10));
                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 155, 165, 200));
                        ImGui::SmallButton(steps[i]);
                        ImGui::PopStyleColor(2);
                    }
                }
            }
            ImGui::Separator();
            ImGui::Spacing();

            // ---- 步骤 0：选网卡 ----
            if (wifi_step_ == 0)
            {
                ImGui::TextWrapped("%s", is_cn
                    ? "选择用于连接热点的无线网卡（WFB 虚拟卡已被过滤）。"
                    : "Select the wireless interface used to connect the AP (WFB virtual interfaces excluded).");

                if (wifi_ifaces_.empty())
                {
                    wifi_ifaces_ = WifiController::ListIfaces();
                    if (wifi_iface_idx_ >= static_cast<int>(wifi_ifaces_.size()))
                        wifi_iface_idx_ = 0;
                }

                ImGui::Spacing();
                if (wifi_ifaces_.empty())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s",
                        is_cn ? "没有找到可用的无线网卡" : "No usable wireless interface found");
                    ImGui::Spacing();
                    if (ImGui::Button(is_cn ? "刷新" : "Refresh", ImVec2(120, 0)))
                        wifi_ifaces_ = WifiController::ListIfaces();
                }
                else
                {
                    for (int i = 0; i < static_cast<int>(wifi_ifaces_.size()); ++i)
                    {
                        const WifiIface &it = wifi_ifaces_[i];
                        std::string label = it.name + "  [" + it.driver + "]  " + it.bands;
                        if (!it.ssid.empty())
                            label += std::string("  * ") + it.ssid;
                        if (ImGui::RadioButton(label.c_str(), wifi_iface_idx_ == i))
                            wifi_iface_idx_ = i;
                    }
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                bool can_next = !wifi_ifaces_.empty();
                if (!can_next)
                    ImGui::BeginDisabled();
                if (ImGui::Button(is_cn ? "下一步" : "Next", ImVec2(130, 0)))
                {
                    wifi_.SetIface(wifi_ifaces_[wifi_iface_idx_].name);
                    wifi_aps_.clear();
                    wifi_scan_requested_ = false;
                    wifi_step_ = 1;
                }
                if (!can_next)
                    ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(is_cn ? "取消" : "Cancel", ImVec2(130, 0)))
                {
                    wifi_step_ = 0;
                    ImGui::CloseCurrentPopup();
                }
            }

            // ---- 步骤 1：扫描并选 AP ----
            else if (wifi_step_ == 1)
            {
                ImGui::Text(is_cn ? "网卡：%s（%s）" : "Interface: %s (%s)",
                            wifi_.Iface().c_str(),
                            wifi_ifaces_.empty() ? "-" : wifi_ifaces_[wifi_iface_idx_].bands.c_str());

                if (!wifi_scan_requested_)
                {
                    wifi_.StartScan(wifi_.Iface());
                    wifi_scan_requested_ = true;
                }

                ImGui::Spacing();
                if (wifi_scanning_)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s",
                        is_cn ? "正在扫描周边 Wi-Fi 热点..." : "Scanning for Wi-Fi APs...");
                }
                else if (wifi_aps_.empty())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s",
                        is_cn ? "未扫描到热点（网卡频段可能不匹配）"
                              : "No AP found (check card band compatibility)");
                }
                else
                {
                    ImGui::Text("%s %d %s", is_cn ? "发现" : "Found", static_cast<int>(wifi_aps_.size()), is_cn ? "个热点" : "APs");
                }

                ImGui::BeginChild("ap_list", ImVec2(520, 230), true);
                for (int i = 0; i < static_cast<int>(wifi_aps_.size()); ++i)
                {
                    const WifiAp &ap = wifi_aps_[i];
                    int pct = ap.signal_dbm;
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;

                    ImVec4 sc = (pct >= 60) ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
                              : (pct >= 30) ? ImVec4(1.0f, 0.82f, 0.30f, 1.0f)
                                            : ImVec4(1.0f, 0.45f, 0.35f, 1.0f);
                    const char *st_tag = "";
                    if (ap.state == "ready" || ap.state == "online")
                        st_tag = is_cn ? "  [已连]" : "  [Connected]";
                    else if (ap.state == "failure")
                        st_tag = is_cn ? "  [失败]" : "  [Failed]";

                    char line[256];
                    std::snprintf(line, sizeof(line), "%-26s %s   %3d%%%s",
                                  ap.ssid.c_str(),
                                  ap.secure ? (is_cn ? "[加密]" : "[Lock]") : (is_cn ? "[开放]" : "[Open]"),
                                  pct, st_tag);

                    const bool selected = (wifi_ap_idx_ == i);
                    ImGui::PushStyleColor(ImGuiCol_Text, sc);
                    if (ImGui::Selectable(line, selected, ImGuiSelectableFlags_None, ImVec2(0, 24.0f)))
                    {
                        wifi_ap_idx_ = i;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::EndChild();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button(is_cn ? "重新扫描" : "Rescan", ImVec2(120, 0)))
                {
                    wifi_scan_requested_ = false;
                    wifi_aps_.clear();
                }
                ImGui::SameLine();
                bool has_ap = !wifi_aps_.empty();
                if (!has_ap)
                    ImGui::BeginDisabled();
                if (ImGui::Button(is_cn ? "下一步" : "Next", ImVec2(120, 0)))
                {
                    std::snprintf(wifi_client_ssid_, sizeof(wifi_client_ssid_), "%s",
                                  wifi_aps_[wifi_ap_idx_].ssid.c_str());
                    wifi_step_ = 2;
                    wifi_pwd_focus_ = 1;
                }
                if (!has_ap)
                    ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(is_cn ? "上一步" : "Back", ImVec2(120, 0)))
                {
                    wifi_step_ = 0;
                    wifi_ifaces_.clear();
                }
                ImGui::SameLine();
                if (ImGui::Button(is_cn ? "取消" : "Cancel", ImVec2(100, 0)))
                {
                    wifi_step_ = 0;
                    ImGui::CloseCurrentPopup();
                }
            }

            // ---- 步骤 2：输密码 ----
            else if (wifi_step_ == 2)
            {
                ImGui::Text(is_cn ? "目标热点：%s" : "Target AP: %s", wifi_client_ssid_);
                ImGui::Spacing();
                ImGui::TextUnformatted(is_cn ? "请输入密码：" : "Enter Password:");
                ImGui::SetNextItemWidth(360);
                if (wifi_pwd_focus_ == 1)
                {
                    ImGui::SetKeyboardFocusHere();
                    wifi_pwd_focus_ = 0;
                }
                ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue;
                if (!wifi_show_password_)
                {
                    input_flags |= ImGuiInputTextFlags_Password;
                }
                bool enter = ImGui::InputText("##password", wifi_client_password_,
                                              sizeof(wifi_client_password_), input_flags);
                ImGui::SameLine();
                ImGui::Checkbox(is_cn ? "显示密码" : "Show", &wifi_show_password_);
                ImGui::TextDisabled("%s", is_cn
                    ? "（开放热点留空即可直接连接）"
                    : "(leave empty for open network)");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button(is_cn ? "连接" : "Connect", ImVec2(130, 0)) || enter)
                {
                    wifi_has_result_ = false;
                    wifi_result_msg_.clear();
                    wifi_step_ = 3;
                    wifi_.StartConnect(wifi_client_ssid_, wifi_client_password_, wifi_.Iface());
                }
                ImGui::SameLine();
                if (ImGui::Button(is_cn ? "上一步" : "Back", ImVec2(130, 0)))
                    wifi_step_ = 1;
                ImGui::SameLine();
                if (ImGui::Button(is_cn ? "取消" : "Cancel", ImVec2(100, 0)))
                {
                    wifi_step_ = 0;
                    ImGui::CloseCurrentPopup();
                }
            }

            // ---- 步骤 3：连接中 / 结果 ----
            else
            {
                if (wifi_connecting_ || (!wifi_has_result_ && wifi_result_msg_.empty()))
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s",
                        is_cn ? "正在连接..." : "Connecting...");
                    ImGui::Spacing();
                    ImGui::Text(is_cn ? "正在连接至：%s" : "Connecting to: %s", wifi_client_ssid_);
                    ImGui::TextDisabled("%s", is_cn
                        ? "正在通知 ConnMan 连接热点并等待 DHCP，最多 30 秒"
                        : "Asking ConnMan to connect and waiting for DHCP (up to 30s)");
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (ImGui::Button(is_cn ? "取消连接" : "Cancel Connection", ImVec2(140, 0)))
                    {
                        wifi_connecting_ = false;
                        wifi_has_result_ = false;
                        wifi_step_ = 1;
                    }
                }
                else if (wifi_result_ok_)
                {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s",
                        is_cn ? "连接成功！" : "Connected successfully!");
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", wifi_result_msg_.c_str());
                    ImGui::Spacing();
                    if (wifi_live_status_.signal_dbm != 0)
                    {
                        ImGui::Text("%s %d dBm", is_cn ? "信号强度：" : "Signal:", wifi_live_status_.signal_dbm);
                    }
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (ImGui::Button(is_cn ? "完成并应用" : "Done", ImVec2(140, 0)))
                    {
                        wifi_step_ = 0;
                        wifi_ifaces_.clear();
                        state_.ForceNetworkModeNotifyOnce();
                        state_.SetNetworkModeIndex(static_cast<int>(MenuState::NetworkMode::ConnectAp));
                        ImGui::CloseCurrentPopup();
                    }
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                        is_cn ? "连接失败" : "Connection failed");
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", wifi_result_msg_.c_str());
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (ImGui::Button(is_cn ? "重试密码" : "Retry Password", ImVec2(130, 0)))
                    {
                        wifi_step_ = 2;
                        wifi_has_result_ = false;
                        wifi_pwd_focus_ = 1;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(is_cn ? "重选热点" : "Choose AP", ImVec2(130, 0)))
                    {
                        wifi_step_ = 1;
                        wifi_has_result_ = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(is_cn ? "关闭" : "Close", ImVec2(100, 0)))
                    {
                        wifi_step_ = 0;
                        wifi_ifaces_.clear();
                        ImGui::CloseCurrentPopup();
                    }
                }
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
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                ImGui::CloseCurrentPopup();
            }
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
    ImGui::PopStyleColor(14);
    ImGui::PopStyleVar(5);
    if (active_category_ < 0)
        settings_open_amount_ = 0.0f;
    settings_visible_last_ = rendered_category_this_frame >= 0;
    last_rendered_category_ = rendered_category_this_frame;
}
