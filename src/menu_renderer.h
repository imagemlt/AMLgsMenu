#pragma once

#include "menu_state.h"

#include "imgui.h"

class MenuRenderer {
public:
    explicit MenuRenderer(MenuState &state);

    void Render(bool &running_flag);

private:
    struct TelemetryData;

    void DrawOsd(const ImGuiViewport *viewport, const TelemetryData &data) const;
    void DrawMenu(const ImGuiViewport *viewport, bool &running_flag);

    MenuState &state_;
};
