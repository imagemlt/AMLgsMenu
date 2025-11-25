#pragma once

#include "menu_state.h"

class MenuRenderer {
public:
    explicit MenuRenderer(MenuState &state);

    void Render(bool &running_flag);

private:
    MenuState &state_;
};

