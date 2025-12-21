#pragma once

#include "video_mode.h"

#include <array>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

class MenuState
{
public:
    MenuState(std::vector<VideoMode> sky_modes, std::vector<VideoMode> ground_modes);

    enum class SettingType
    {
        Channel,
        Bandwidth,
        SkyMode,
        GroundMode,
        Bitrate,
        SkyPower,
        GroundPower,
        Recording,
        Language,
        Firmware,
    };

    enum class Language
    {
        CN = 0,
        EN = 1,
    };

    enum class FirmwareType
    {
        CCEdition = 0,
        Official = 1,
    };

    using SettingChangedCallback = std::function<void(SettingType)>;
    using ChangeVisibilityCallback = std::function<void(bool)>;

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
    Language GetLanguage() const { return language_; }
    FirmwareType GetFirmwareType() const { return firmware_type_; }
    bool Recording() const { return recording_; }
    bool ShouldExit() const { return should_exit_; }

    void SetChannelIndex(int index);
    void SetBandwidthIndex(int index);
    void SetSkyModeIndex(int index);
    void SetGroundModeIndex(int index);
    void SetBitrateIndex(int index);
    void SetSkyPowerIndex(int index);
    void SetGroundPowerIndex(int index);
    void SetLanguage(Language lang);
    void SetFirmwareType(FirmwareType type);
    void ToggleMenuVisibility()
    {
        menu_visible_ = !menu_visible_;
        // if (!menu_visible_)
        //     on_change_visibility_callback_(menu_visible_);
    }
    void SetMenuVisible(bool visible) { menu_visible_ = visible; }

    void ToggleRecording();
    void RequestExit() { should_exit_ = true; }
    void SetOnChangeCallback(SettingChangedCallback cb) { on_change_callback_ = std::move(cb); }
    // void SetVisibilityChangeCallback(ChangeVisibilityCallback cb) { on_change_visibility_callback_ = std::move(cb); }
    void RequestGroundModeSkipSaveOnce();
    bool ConsumeGroundModeSkipSaveOnce();
    void RequestGroundModeForceSaveOnce();
    bool ConsumeGroundModeForceSaveOnce();
    void ForceGroundModeNotifyOnce();
    bool ExperimentalGroundPersisted() const { return experimental_ground_persisted_; }
    void SetExperimentalGroundPersisted(bool value) { experimental_ground_persisted_ = value; }
    bool IsGroundModePersisted(const std::string &label) const;
    void SetGroundModePersisted(const std::string &label, bool persisted);

private:
    static std::vector<int> BuildRange(int start, int end);
    void NotifyChange(SettingType type) const;

    std::vector<int> channels_;
    std::vector<int> bitrates_;
    std::vector<int> power_levels_;
    std::vector<VideoMode> sky_modes_;
    std::vector<VideoMode> ground_modes_;
    std::array<const char *, 3> bandwidths_{{"10 MHz", "20 MHz", "40 MHz"}};

    SettingChangedCallback on_change_callback_;
    ChangeVisibilityCallback on_change_visibility_callback_;

    int channel_index_ = 0;
    int bandwidth_index_ = 0;
    int sky_mode_index_ = 0;
    int ground_mode_index_ = 0;
    int bitrate_index_ = 0;
    int sky_power_index_ = 0;
    int ground_power_index_ = 0;
    Language language_ = Language::CN;
    // shared_ptr<Application> application_;
    FirmwareType firmware_type_ = FirmwareType::CCEdition;
    bool menu_visible_ = false;
    bool recording_ = false;
    bool should_exit_ = false;
    bool ground_mode_skip_save_once_ = false;
    bool ground_mode_force_save_once_ = false;
    bool force_ground_mode_notify_once_ = false;
    bool experimental_ground_persisted_ = false;
    std::unordered_set<std::string> persisted_ground_modes_;
};
