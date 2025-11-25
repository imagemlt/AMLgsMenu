#include "menu_state.h"

MenuState::MenuState(std::vector<VideoMode> sky_modes, std::vector<VideoMode> ground_modes)
    : channels_(BuildRange(34, 179)),
      bitrates_(BuildRange(1, 50)),
      power_levels_(BuildRange(1, 60)),
      sky_modes_(std::move(sky_modes)),
      ground_modes_(std::move(ground_modes)) {}

std::vector<int> MenuState::BuildRange(int start, int end) {
    std::vector<int> values;
    for (int i = start; i <= end; ++i) {
        values.push_back(i);
    }
    return values;
}

