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
        SkyMcs,
        SkyPower,
        GroundPower,
        SoundEnable,
        SoundVolume,
        BufferLevel,
        HdmiAttr,
        BadFramePolicy,
        AdaptiveLink,
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
    const std::vector<int> &McsLevels() const { return mcs_levels_; }
    const std::vector<int> &VolumeLevels() const { return volume_levels_; }
    const std::vector<int> &BufferLevels() const { return buffer_levels_; }
    const std::vector<std::string> &HdmiAttrs() const { return hdmi_attrs_; }
    bool MenuVisible() const { return menu_visible_; }

    int ChannelIndex() const { return channel_index_; }
    int BandwidthIndex() const { return bandwidth_index_; }
    int SkyModeIndex() const { return sky_mode_index_; }
    int GroundModeIndex() const { return ground_mode_index_; }
    int BitrateIndex() const { return bitrate_index_; }
    int SkyPowerIndex() const { return sky_power_index_; }
    int SkyMcsIndex() const { return sky_mcs_index_; }
    int GroundPowerIndex() const { return ground_power_index_; }
    int SoundVolumeIndex() const { return sound_volume_index_; }
    int BufferLevelIndex() const { return buffer_level_index_; }
    int HdmiAttrIndex() const { return hdmi_attr_index_; }
    int BadFrameIndex() const { return bad_frame_index_; }
    bool AdaptiveLinkEnabled() const { return adaptive_link_enabled_; }
    Language GetLanguage() const { return language_; }
    FirmwareType GetFirmwareType() const { return firmware_type_; }
    bool Recording() const { return recording_; }
    bool SoundEnabled() const { return sound_enabled_; }
    bool ShouldExit() const { return should_exit_; }
    void SetNotifyEnabled(bool enabled) { notify_enabled_ = enabled; }
    bool NotifyEnabled() const { return notify_enabled_; }

    void SetChannelIndex(int index);
    void SetBandwidthIndex(int index);
    void SetSkyModeIndex(int index);
    void SetGroundModeIndex(int index);
    void SetBitrateIndex(int index);
    void SetSkyMcsIndex(int index);
    void SetSkyPowerIndex(int index);
    void SetGroundPowerIndex(int index);
    void SetSoundEnabled(bool enabled);
    void SetSoundVolumeIndex(int index);
    void SetBufferLevelIndex(int index);
    void SetHdmiAttrIndex(int index);
    void SetBadFrameIndex(int index);
    void SetAdaptiveLinkEnabled(bool enabled);
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
    void RequestHdmiAttrSkipSaveOnce();
    bool ConsumeHdmiAttrSkipSaveOnce();
    void RequestHdmiAttrForceSaveOnce();
    bool ConsumeHdmiAttrForceSaveOnce();
    void ForceHdmiAttrNotifyOnce();
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
    std::vector<int> mcs_levels_;
    std::vector<int> volume_levels_;
    std::vector<int> buffer_levels_;
    std::vector<std::string> hdmi_attrs_;
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
    int sky_mcs_index_ = 2;
    int ground_power_index_ = 0;
    int sound_volume_index_ = 9;
    int buffer_level_index_ = 0;
    int hdmi_attr_index_ = 0;
    int bad_frame_index_ = 1;
    bool adaptive_link_enabled_ = false;
    Language language_ = Language::CN;
    // shared_ptr<Application> application_;
    FirmwareType firmware_type_ = FirmwareType::CCEdition;
    bool menu_visible_ = false;
    bool recording_ = false;
    bool sound_enabled_ = false;
    bool should_exit_ = false;
    bool ground_mode_skip_save_once_ = false;
    bool ground_mode_force_save_once_ = false;
    bool force_ground_mode_notify_once_ = false;
    bool hdmi_attr_skip_save_once_ = false;
    bool hdmi_attr_force_save_once_ = false;
    bool force_hdmi_attr_notify_once_ = false;
    bool experimental_ground_persisted_ = false;
    std::unordered_set<std::string> persisted_ground_modes_;
    bool notify_enabled_ = true;
};
