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

void MenuState::NotifyChange(SettingType type) const {
    if (on_change_callback_) {
        on_change_callback_(type);
    }
}

// Override setters to only notify on actual changes
void MenuState::SetChannelIndex(int index) {
    if (channel_index_ == index) {
        return;
    }
    channel_index_ = index;
    NotifyChange(SettingType::Channel);
}

void MenuState::SetBandwidthIndex(int index) {
    if (bandwidth_index_ == index) {
        return;
    }
    bandwidth_index_ = index;
    NotifyChange(SettingType::Bandwidth);
}

void MenuState::SetSkyModeIndex(int index) {
    if (sky_mode_index_ == index) {
        return;
    }
    sky_mode_index_ = index;
    NotifyChange(SettingType::SkyMode);
}

void MenuState::SetGroundModeIndex(int index) {
    if (ground_mode_index_ == index) {
        return;
    }
    ground_mode_index_ = index;
    NotifyChange(SettingType::GroundMode);
}

void MenuState::SetBitrateIndex(int index) {
    if (bitrate_index_ == index) {
        return;
    }
    bitrate_index_ = index;
    NotifyChange(SettingType::Bitrate);
}

void MenuState::SetSkyPowerIndex(int index) {
    if (sky_power_index_ == index) {
        return;
    }
    sky_power_index_ = index;
    NotifyChange(SettingType::SkyPower);
}

void MenuState::SetGroundPowerIndex(int index) {
    if (ground_power_index_ == index) {
        return;
    }
    ground_power_index_ = index;
    NotifyChange(SettingType::GroundPower);
}

void MenuState::ToggleRecording() {
    bool new_value = !recording_;
    if (new_value == recording_) {
        return;
    }
    recording_ = new_value;
    NotifyChange(SettingType::Recording);
}
