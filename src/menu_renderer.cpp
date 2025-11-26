#include "menu_renderer.h"

#include "imgui.h"
#include "video_mode.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>
#include <png.h>
#include <GLES2/gl2.h>

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
    static float last_fps_time = -1.0f;
    static int cached_fps = 0;
    const float now = static_cast<float>(ImGui::GetTime());
    if (last_fps_time < 0.0f || (now - last_fps_time) >= 1.0f) {
        cached_fps = GetOutputFps();
        last_fps_time = now;
    }
    int fps = cached_fps;
    data.video_refresh_hz = fps > 0 ? fps : (mode.refresh ? mode.refresh : 60);
    data.bitrate_mbps = std::max(1.0f, 6.0f + 2.0f * std::sin(t * 0.4f));

    data.cell_voltage = 3.8f + 0.12f * std::sin(t * 0.6f);
    data.pack_voltage = data.cell_voltage * 4.0f + 0.4f * std::cos(t * 0.3f);

    data.sky_temp_c = 45.0f + 5.0f * std::sin(t * 0.22f);
    data.ground_temp_c = ReadTemperatureC();
    data.roll_deg = 10.0f * std::sin(t * 0.6f);
    data.pitch_deg = 5.0f * std::cos(t * 0.5f);

    return data;
}

MenuRenderer::MenuRenderer(MenuState &state, bool use_mock, std::function<TelemetryData()> provider)
    : state_(state), use_mock_(use_mock), telemetry_provider_(std::move(provider)) {
    int w = 0, h = 0;
    const char *icon_base = "/storage/digitalfpv/icons/";
    LoadIcon(std::string(icon_base + std::string("antenna.png")).c_str(), icon_antenna_, w, h);
    LoadIcon(std::string(icon_base + std::string("battery_per.png")).c_str(), icon_batt_cell_, w, h);
    LoadIcon(std::string(icon_base + std::string("battery_all.png")).c_str(), icon_batt_pack_, w, h);
    LoadIcon(std::string(icon_base + std::string("gps.png")).c_str(), icon_gps_, w, h);
    LoadIcon(std::string(icon_base + std::string("monitor.png")).c_str(), icon_monitor_, w, h);
    LoadIcon(std::string(icon_base + std::string("temp_air.png")).c_str(), icon_temp_air_, w, h);
    LoadIcon(std::string(icon_base + std::string("temp_ground.png")).c_str(), icon_temp_ground_, w, h);
}
bool MenuRenderer::LoadIcon(const char *path, ImTextureID &out_id, int &out_w, int &out_h) {
    FILE *fp = std::fopen(path, "rb");
    if (!fp) return false;
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) {
        std::fclose(fp);
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        std::fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        std::fclose(fp);
        return false;
    }
    png_init_io(png_ptr, fp);
    png_read_info(png_ptr, info_ptr);
    png_uint_32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    png_read_update_info(png_ptr, info_ptr);
    std::vector<png_byte> data;
    data.resize(png_get_rowbytes(png_ptr, info_ptr) * height);
    std::vector<png_bytep> row_ptrs(height);
    for (png_uint_32 y = 0; y < height; ++y) {
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
MenuRenderer::~MenuRenderer() {
    GLuint tex_ids[5]{};
    int count = 0;
    auto collect = [&](ImTextureID id) {
        if (id) {
            tex_ids[count++] = static_cast<GLuint>(id);
        }
    };
    collect(icon_antenna_);
    collect(icon_batt_cell_);
    collect(icon_batt_pack_);
    collect(icon_gps_);
    collect(icon_monitor_);
    if (count > 0) {
        glDeleteTextures(count, tex_ids);
    }
}

void MenuRenderer::Render(bool &running_flag) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    auto now_tp = std::chrono::steady_clock::now();
    bool need_refresh = (last_osd_update_time_ < 0.0f ||
                         last_osd_tp_.time_since_epoch().count() == 0 ||
                         std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_osd_tp_).count() >= 100);
    if (need_refresh) {
        TelemetryData new_data = cached_telemetry_;
        if (use_mock_) {
            new_data = BuildMockTelemetry(state_);
        } else if (telemetry_provider_) {
            new_data = telemetry_provider_();

            static auto last_ground_sample = std::chrono::steady_clock::time_point{};
            auto ms_ground = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_ground_sample).count();
            const bool update_ground = (last_ground_sample.time_since_epoch().count() == 0 || ms_ground >= 1000);
            if (!update_ground) {
                new_data.ground_signal_a = cached_telemetry_.ground_signal_a;
                new_data.ground_signal_b = cached_telemetry_.ground_signal_b;
                new_data.ground_temp_c = cached_telemetry_.ground_temp_c;
                new_data.video_refresh_hz = cached_telemetry_.video_refresh_hz;
                new_data.video_resolution = cached_telemetry_.video_resolution;
                new_data.bitrate_mbps = cached_telemetry_.bitrate_mbps;
            } else {
                last_ground_sample = now_tp;
            }
        }
        cached_telemetry_ = new_data;
        last_osd_update_time_ = static_cast<float>(ImGui::GetTime());
        last_osd_tp_ = now_tp;
    }

    DrawOsd(viewport, cached_telemetry_);

    ImGuiIO &io = ImGui::GetIO();
    io.MouseDrawCursor = state_.MenuVisible();
    if (state_.MenuVisible()) {
        DrawMenu(viewport, running_flag);
    }
}

void MenuRenderer::DrawOsd(const ImGuiViewport *viewport, const TelemetryData &data) const {
    ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
    const bool is_cn = state_.GetLanguage() == MenuState::Language::CN;
    const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f);

    const float icon_size = 18.0f * 1.5f;
    const float icon_gap = 6.0f * 1.5f;
    const ImU32 text_outline = IM_COL32(0, 0, 0, 255);                  // solid black edge
    const ImU32 text_fill = IM_COL32(235, 245, 255, 255);               // cool light tone for visibility

    auto draw_icon = [&](ImVec2 pos, ImTextureID tex) {
        if (tex) {
            draw_list->AddImage(tex, pos, ImVec2(pos.x + icon_size, pos.y + icon_size));
        } else {
            ImU32 fill = IM_COL32(80, 120, 200, 180);
            ImU32 border = IM_COL32(180, 210, 255, 220);
            draw_list->AddRectFilled(pos, ImVec2(pos.x + icon_size, pos.y + icon_size), fill, 3.0f);
            draw_list->AddRect(pos, ImVec2(pos.x + icon_size, pos.y + icon_size), border, 3.0f, 0, 1.5f);
        }
    };

    auto draw_horizon = [&](float roll_deg, float pitch_deg) {
        const float line_half_len = viewport->Size.x * 0.25f * 0.66f; // lengthen a bit vs previous
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
    }

    auto draw_centered_text = [&](ImVec2 pos, const std::string &text, ImU32 color, ImTextureID tex) {
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        ImVec2 icon_pos(pos.x - size.x * 0.5f - icon_size - icon_gap, pos.y);
        draw_icon(icon_pos, tex);
        ImVec2 text_pos(icon_pos.x + icon_size + icon_gap, pos.y);
        // Shadow
        draw_list->AddText(ImVec2(text_pos.x + 1.2f, text_pos.y + 1.2f), text_outline, text.c_str());
        draw_list->AddText(text_pos, color, text.c_str());
    };

    auto draw_centered_text_no_icon = [&](ImVec2 pos, const std::string &text, ImU32 color) {
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        ImVec2 text_pos(pos.x - size.x * 0.5f, pos.y);
        draw_list->AddText(ImVec2(text_pos.x + 1.2f, text_pos.y + 1.2f), text_outline, text.c_str());
        draw_list->AddText(text_pos, color, text.c_str());
    };

    if (data.has_rc_signal || data.ground_signal_a != 0.0f || data.ground_signal_b != 0.0f) {
        std::ostringstream signal;
        if (is_cn) {
            signal << "\u5730\u9762A: " << static_cast<int>(data.ground_signal_a) << " dBm  |  "
                   << "\u5730\u9762B: " << static_cast<int>(data.ground_signal_b) << " dBm";
            if (data.has_rc_signal) {
                signal << "  |  RC: " << static_cast<int>(data.rc_signal) << " dBm";
            }
        } else {
            signal << "GND A: " << static_cast<int>(data.ground_signal_a) << " dBm  |  "
                   << "GND B: " << static_cast<int>(data.ground_signal_b) << " dBm";
            if (data.has_rc_signal) {
                signal << "  |  RC: " << static_cast<int>(data.rc_signal) << " dBm";
            }
        }
        draw_centered_text(ImVec2(center.x, viewport->Pos.y + viewport->Size.y * 0.05f),
                           signal.str(), text_fill, icon_antenna_);
    }

    if (data.has_flight_mode) {
        ImFont *font = ImGui::GetFont();
        float base = ImGui::GetFontSize();
        float mode_size = base * 1.5f; // enlarge flight mode text
        ImU32 mode_fill = IM_COL32(170, 220, 255, 255);   // soft cyan for readability
        ImU32 mode_outline = IM_COL32(10, 26, 42, 240);   // deep navy outline
        std::string label = data.flight_mode;
        ImVec2 size = ImGui::CalcTextSize(label.c_str());
        ImVec2 pos(center.x - size.x * 0.5f, center.y - viewport->Size.y * 0.25f);
        ImDrawList *dl = draw_list;
        dl->AddText(font, mode_size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), mode_outline, label.c_str());
        dl->AddText(font, mode_size, pos, mode_fill, label.c_str());
    }

    ImGuiWindowFlags overlay_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;

    auto icon_text_line = [&](const char *text, ImTextureID tex) {
        ImVec2 start = ImGui::GetCursorScreenPos();
        draw_icon(start, tex);
        ImGui::Dummy(ImVec2(icon_size + icon_gap, icon_size));
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(start.x + icon_size + icon_gap, start.y));
        ImGui::TextUnformatted(text);
    };

    if (data.has_gps) {
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 16.0f,
                                       viewport->Pos.y + viewport->Size.y - 140.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("OSD_GPS", nullptr, overlay_flags)) {
            ImGui::PushStyleColor(ImGuiCol_Text, text_fill);
            char gps_buf[128];
            if (is_cn) {
                snprintf(gps_buf, sizeof(gps_buf), "GPS: %.5f, %.5f, %.1fm",
                         data.latitude, data.longitude, data.altitude_m);
            } else {
                snprintf(gps_buf, sizeof(gps_buf), "GPS: %.5f, %.5f, %.1fm",
                         data.latitude, data.longitude, data.altitude_m);
            }
            icon_text_line(gps_buf, icon_gps_);
            char home_buf[64];
            if (is_cn) {
                snprintf(home_buf, sizeof(home_buf), "\u79bb\u5bb6\u8ddd\u79bb: %.1fm", data.home_distance_m);
            } else {
                snprintf(home_buf, sizeof(home_buf), "Home Dist: %.1fm", data.home_distance_m);
            }
            icon_text_line(home_buf, icon_gps_);
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - 16.0f,
                                   viewport->Pos.y + viewport->Size.y - 48.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("OSD_VIDEO", nullptr, overlay_flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, text_fill);
        char video_buf[128];
        if (is_cn) {
            snprintf(video_buf, sizeof(video_buf), "\u89c6\u9891: %.1f Mbps %s @ %dHz",
                     data.bitrate_mbps, data.video_resolution.c_str(), data.video_refresh_hz);
        } else {
            snprintf(video_buf, sizeof(video_buf), "Video: %.1f Mbps %s @ %dHz",
                     data.bitrate_mbps, data.video_resolution.c_str(), data.video_refresh_hz);
        }
        icon_text_line(video_buf, icon_monitor_);
        ImGui::PopStyleColor();
    }
    ImGui::End();

    if (data.has_battery) {
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 16.0f, center.y - 24.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("OSD_BATT", nullptr, overlay_flags)) {
            ImGui::PushStyleColor(ImGuiCol_Text, text_fill);
            char cell_buf[32];
            if (is_cn) {
                snprintf(cell_buf, sizeof(cell_buf), "\u5355\u8282: %.2fV", data.cell_voltage);
            } else {
                snprintf(cell_buf, sizeof(cell_buf), "Cell: %.2fV", data.cell_voltage);
            }
            icon_text_line(cell_buf, icon_batt_cell_);
            char pack_buf[32];
            if (is_cn) {
                snprintf(pack_buf, sizeof(pack_buf), "\u603b\u7535: %.2fV", data.pack_voltage);
            } else {
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
    if (ImGui::Begin("OSD_TEMP", nullptr, overlay_flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, text_fill);
        char sky_buf[32];
        snprintf(sky_buf, sizeof(sky_buf), is_cn ? "\u5929\u7a7a\u7aef\u6e29\u5ea6: %.1f\u2103" : "Air Temp: %.1fC", data.sky_temp_c);
        if (data.has_sky_temp) {
            icon_text_line(sky_buf, icon_temp_air_);
        }
        char ground_buf[32];
        snprintf(ground_buf, sizeof(ground_buf), is_cn ? "\u5730\u9762\u7a7a\u7aef\u6e29\u5ea6: %.1f\u2103" : "Ground Temp: %.1fC", data.ground_temp_c);
        icon_text_line(ground_buf, icon_temp_ground_);
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

void MenuRenderer::DrawMenu(const ImGuiViewport *viewport, bool &running_flag) {
    const ImVec2 menu_size = ImVec2(viewport->Size.x * 0.5f, viewport->Size.y * 0.45f);
    const ImVec2 menu_pos = ImVec2(viewport->Pos.x + viewport->Size.x * 0.25f,
                                   viewport->Pos.y + viewport->Size.y * 0.30f);
    const bool is_cn = state_.GetLanguage() == MenuState::Language::CN;
    bool kodi_popup_requested = false;

    ImGui::SetNextWindowBgAlpha(0.9f); // make menu opaque
    ImGui::SetNextWindowPos(menu_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(menu_size, ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 24, 32, 238));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(80, 200, 190, 255));       // teal border
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(34, 42, 54, 240));        // controls bg
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(60, 110, 125, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(78, 140, 155, 255));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 140, 170, 240));       // default buttons
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 170, 200, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(50, 120, 150, 255));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("GS Control Menu", nullptr, flags)) {
        ImGui::TextUnformatted(is_cn ? "\u65e0\u7ebf\u94fe\u8def\u914d\u7f6e" : "Wireless Link Settings");
        ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 10)); // slightly taller rows
        if (ImGui::BeginTable("menu_table", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("L1", ImGuiTableColumnFlags_WidthStretch, 0.22f);
            ImGui::TableSetupColumn("C1", ImGuiTableColumnFlags_WidthStretch, 0.28f);
            ImGui::TableSetupColumn("L2", ImGuiTableColumnFlags_WidthStretch, 0.22f);
            ImGui::TableSetupColumn("C2", ImGuiTableColumnFlags_WidthStretch, 0.28f);

            auto row_pair = [](const char *l1, const std::function<void()> &c1,
                               const char *l2, const std::function<void()> &c2) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(l1);
                ImGui::TableSetColumnIndex(1);
                c1();
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(l2);
                ImGui::TableSetColumnIndex(3);
                c2();
            };

            const auto &channels = state_.Channels();
            const auto &bandwidths = state_.Bandwidths();
            row_pair(is_cn ? "\u4fe1\u9053" : "Channel", [&] {
                         if (ImGui::BeginCombo("##channel", std::to_string(channels[state_.ChannelIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(channels.size()); ++i) {
                                 bool selected = (state_.ChannelIndex() == i);
                                 if (ImGui::Selectable(std::to_string(channels[i]).c_str(), selected)) {
                                     state_.SetChannelIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         }
                     },
                     is_cn ? "\u9891\u5bbd" : "Bandwidth", [&] {
                         if (ImGui::BeginCombo("##bandwidth", bandwidths[state_.BandwidthIndex()])) {
                             for (int i = 0; i < static_cast<int>(bandwidths.size()); ++i) {
                                 bool selected = (state_.BandwidthIndex() == i);
                                 if (ImGui::Selectable(bandwidths[i], selected)) {
                                     state_.SetBandwidthIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         }
                     });

            const auto &sky_modes = state_.SkyModes();
            const auto &ground_modes = state_.GroundModes();
            row_pair(is_cn ? "\u5929\u7a7a\u7aef\u5206\u8fa8\u7387/\u5237\u65b0\u7387" : "Air Res/Refresh", [&] {
                         if (!sky_modes.empty() && ImGui::BeginCombo("##sky_mode", FormatVideoModeLabel(sky_modes[state_.SkyModeIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(sky_modes.size()); ++i) {
                                 bool selected = (state_.SkyModeIndex() == i);
                                 if (ImGui::Selectable(FormatVideoModeLabel(sky_modes[i]).c_str(), selected)) {
                                     state_.SetSkyModeIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         }
                     },
                     is_cn ? "\u5730\u9762\u7aef\u5206\u8fa8\u7387/\u5237\u65b0\u7387" : "Ground Res/Refresh", [&] {
                         if (!ground_modes.empty() && ImGui::BeginCombo("##ground_mode", FormatVideoModeLabel(ground_modes[state_.GroundModeIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(ground_modes.size()); ++i) {
                                 bool selected = (state_.GroundModeIndex() == i);
                                 if (ImGui::Selectable(FormatVideoModeLabel(ground_modes[i]).c_str(), selected)) {
                                     state_.SetGroundModeIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         }
                     });

            const auto &bitrates = state_.Bitrates();
            const auto &powers = state_.PowerLevels();
            row_pair(is_cn ? "\u7801\u7387(Mbps)" : "Bitrate (Mbps)", [&] {
                         if (ImGui::BeginCombo("##bitrate", std::to_string(bitrates[state_.BitrateIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(bitrates.size()); ++i) {
                                 bool selected = (state_.BitrateIndex() == i);
                                 if (ImGui::Selectable(std::to_string(bitrates[i]).c_str(), selected)) {
                                     state_.SetBitrateIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         }
                     },
                     is_cn ? "\u5929\u7a7a\u7aef\u53d1\u5c04\u529f\u7387" : "Air TX Power", [&] {
                         if (ImGui::BeginCombo("##sky_power", std::to_string(powers[state_.SkyPowerIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(powers.size()); ++i) {
                                 bool selected = (state_.SkyPowerIndex() == i);
                                 if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected)) {
                                     state_.SetSkyPowerIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         }
                     });

            row_pair(is_cn ? "\u5730\u9762\u7aef\u53d1\u5c04\u529f\u7387" : "Ground TX Power", [&] {
                         if (ImGui::BeginCombo("##ground_power", std::to_string(powers[state_.GroundPowerIndex()]).c_str())) {
                             for (int i = 0; i < static_cast<int>(powers.size()); ++i) {
                                 bool selected = (state_.GroundPowerIndex() == i);
                                 if (ImGui::Selectable(std::to_string(powers[i]).c_str(), selected)) {
                                     state_.SetGroundPowerIndex(i);
                                 }
                                 if (selected) ImGui::SetItemDefaultFocus();
                             }
                             ImGui::EndCombo();
                         }
                     },
                     is_cn ? "\u5f55\u50cf\u63a7\u5236" : "Recording", [&] {
                         if (ImGui::Button(state_.Recording() ? (is_cn ? "\u505c\u6b62\u5f55\u50cf" : "Stop Recording")
                                                              : (is_cn ? "\u5f00\u542f\u5f55\u50cf" : "Start Recording"),
                                           ImVec2(-1, 0))) {
                             state_.ToggleRecording();
                         }
                     });

            row_pair(is_cn ? "\u8bed\u8a00" : "Language", [&] {
                         const char *label = state_.GetLanguage() == MenuState::Language::CN ? "\u4e2d\u6587" : "English";
                         if (ImGui::BeginCombo("##lang", label)) {
                             if (ImGui::Selectable("\u4e2d\u6587", state_.GetLanguage() == MenuState::Language::CN)) {
                                 state_.SetLanguage(MenuState::Language::CN);
                             }
                             if (ImGui::Selectable("English", state_.GetLanguage() == MenuState::Language::EN)) {
                                 state_.SetLanguage(MenuState::Language::EN);
                             }
                             ImGui::EndCombo();
                         }
                     },
                     is_cn ? "\u6253\u5f00 KODI" : "Open KODI", [&] {
                         if (ImGui::Button(is_cn ? "\u6253\u5f00 KODI" : "Open KODI", ImVec2(-1, 0))) {
                             kodi_popup_requested = true;
                         }
                     });

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(" ");
            ImGui::TableSetColumnIndex(3);
            ImVec2 btn_sz(-1, 0);
            if (ImGui::Button(is_cn ? "\u786e\u8ba4" : "OK", btn_sz)) {
                state_.ToggleMenuVisibility();
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(" ");
            ImGui::TableSetColumnIndex(3);
            ImGui::Dummy(ImVec2(0, 6)); // small vertical gap
            if (ImGui::Button(is_cn ? "\u5173\u95ed" : "Close", btn_sz)) {
                state_.ToggleMenuVisibility();
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        if (kodi_popup_requested) {
            ImGui::OpenPopup("confirm_kodi");
        }
        ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                       viewport->Pos.y + viewport->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("confirm_kodi", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            const char *msg = is_cn ? "\u6253\u5f00 KODI \u5c06\u5173\u95ed\u56fe\u4f20\u7a0b\u5e8f\uff0c\u662f\u5426\u7ee7\u7eed\uff1f"
                                    : "Opening KODI will close the video link process. Continue?";
            ImGui::TextWrapped("%s", msg);
            ImGui::Spacing();
            if (ImGui::Button(is_cn ? "\u53d6\u6d88" : "Cancel", ImVec2(140, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(is_cn ? "\u786e\u8ba4" : "Confirm", ImVec2(140, 0))) {
                std::system("bash -lc 'killall -9 AMLDigitalFPV || true; systemctl restart kodi'"); // restart kodi and exit
                running_flag = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

    }

    ImGui::End();
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(4);
}
