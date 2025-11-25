#pragma once

#include "video_mode.h"

#include <array>
#include <string>
#include <vector>

class MenuState {
public:
    MenuState(std::vector<VideoMode> sky_modes, std::vector<VideoMode> ground_modes);

    const std::vector<int> &Channels() const { return channels_; }
    const std::array<const char *, 3> &Bandwidths() const { return bandwidths_; }
    const std::vector<VideoMode> &SkyModes() const { return sky_modes_; }
    const std::vector<VideoMode> &GroundModes() const { return ground_modes_; }
    const std::vector<int> &Bitrates() const { return bitrates_; }
    const std::vector<int> &PowerLevels() const { return power_levels_; }
    bool MenuVisible() const { return menu_visible_; }

    int ChannelIndex() const { return channel_index_; }
    int BandwidthIndex() const { return bandwidth_index_; }
    int SkyModeIndex() const { return sky_mode_index_; }
    int GroundModeIndex() const { return ground_mode_index_; }
    int BitrateIndex() const { return bitrate_index_; }
    int SkyPowerIndex() const { return sky_power_index_; }
    int GroundPowerIndex() const { return ground_power_index_; }
    bool Recording() const { return recording_; }
    bool ShouldExit() const { return should_exit_; }

    void SetChannelIndex(int index) { channel_index_ = index; }
    void SetBandwidthIndex(int index) { bandwidth_index_ = index; }
    void SetSkyModeIndex(int index) { sky_mode_index_ = index; }
    void SetGroundModeIndex(int index) { ground_mode_index_ = index; }
    void SetBitrateIndex(int index) { bitrate_index_ = index; }
    void SetSkyPowerIndex(int index) { sky_power_index_ = index; }
    void SetGroundPowerIndex(int index) { ground_power_index_ = index; }
    void ToggleMenuVisibility() { menu_visible_ = !menu_visible_; }

    void ToggleRecording() { recording_ = !recording_; }
    void RequestExit() { should_exit_ = true; }

private:
    static std::vector<int> BuildRange(int start, int end);

    std::vector<int> channels_;
    std::vector<int> bitrates_;
    std::vector<int> power_levels_;
    std::vector<VideoMode> sky_modes_;
    std::vector<VideoMode> ground_modes_;
    std::array<const char *, 3> bandwidths_{{"10 MHz", "20 MHz", "40 MHz"}};

    int channel_index_ = 0;
    int bandwidth_index_ = 0;
    int sky_mode_index_ = 0;
    int ground_mode_index_ = 0;
    int bitrate_index_ = 0;
    int sky_power_index_ = 0;
    int ground_power_index_ = 0;
    bool menu_visible_ = false;
    bool recording_ = false;
    bool should_exit_ = false;
};

