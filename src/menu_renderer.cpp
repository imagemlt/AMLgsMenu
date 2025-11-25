#include "menu_renderer.h"

#include "imgui.h"
#include "video_mode.h"

#include <sstream>

MenuRenderer::MenuRenderer(MenuState &state) : state_(state) {}

void MenuRenderer::Render(bool &running_flag) {
    if (!state_.MenuVisible()) {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
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
        if (ImGui::BeginCombo("\u5929\u7a7a\u7aef\u5206\u8fa8\u7387/\u5237\u65b0\u7387", FormatVideoModeLabel(sky_modes[state_.SkyModeIndex()]).c_str())) {
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
        if (ImGui::BeginCombo("\u5730\u9762\u7aef\u5206\u8fa8\u7387/\u5237\u65b0\u7387", FormatVideoModeLabel(ground_modes[state_.GroundModeIndex()]).c_str())) {
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

        if (ImGui::Button("\u9000\u51fa")) {
            state_.RequestExit();
            running_flag = false;
        }
    }

    ImGui::End();
}

