#include "menu_state.h"

MenuState::MenuState(std::vector<VideoMode> sky_modes, std::vector<VideoMode> ground_modes)
    : channels_({// 2.4 GHz
                 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                 // 5 GHz (Digi list)
                 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 96,
                 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
                 149, 153, 157, 161, 165, 169, 173, 177}),
      bitrates_(BuildRange(1, 50)),
      power_levels_(BuildRange(1, 60)),
      sky_modes_(std::move(sky_modes)),
      ground_modes_(std::move(ground_modes))
{
}

std::vector<int> MenuState::BuildRange(int start, int end)
{
    std::vector<int> values;
    for (int i = start; i <= end; ++i)
    {
        values.push_back(i);
    }
    return values;
}

void MenuState::NotifyChange(SettingType type) const
{
    if (on_change_callback_)
    {
        on_change_callback_(type);
    }
}

// Override setters to only notify on actual changes
void MenuState::SetChannelIndex(int index)
{
    if (channel_index_ == index)
    {
        return;
    }
    channel_index_ = index;
    NotifyChange(SettingType::Channel);
}

void MenuState::SetBandwidthIndex(int index)
{
    if (bandwidth_index_ == index)
    {
        return;
    }
    bandwidth_index_ = index;
    NotifyChange(SettingType::Bandwidth);
}

void MenuState::SetSkyModeIndex(int index)
{
    if (sky_mode_index_ == index)
    {
        return;
    }
    sky_mode_index_ = index;
    NotifyChange(SettingType::SkyMode);
}

void MenuState::SetGroundModeIndex(int index)
{
    const bool force_notify = force_ground_mode_notify_once_;
    if (!force_notify && ground_mode_index_ == index)
    {
        return;
    }
    ground_mode_index_ = index;
    force_ground_mode_notify_once_ = false;
    NotifyChange(SettingType::GroundMode);
}

void MenuState::SetBitrateIndex(int index)
{
    if (bitrate_index_ == index)
    {
        return;
    }
    bitrate_index_ = index;
    NotifyChange(SettingType::Bitrate);
}

void MenuState::SetSkyPowerIndex(int index)
{
    if (sky_power_index_ == index)
    {
        return;
    }
    sky_power_index_ = index;
    NotifyChange(SettingType::SkyPower);
}

void MenuState::SetGroundPowerIndex(int index)
{
    if (ground_power_index_ == index)
    {
        return;
    }
    ground_power_index_ = index;
    NotifyChange(SettingType::GroundPower);
}

void MenuState::SetLanguage(Language lang)
{
    if (language_ == lang)
    {
        return;
    }
    language_ = lang;
    NotifyChange(SettingType::Language);
}

void MenuState::SetFirmwareType(FirmwareType type)
{
    if (firmware_type_ == type)
    {
        return;
    }
    firmware_type_ = type;
    NotifyChange(SettingType::Firmware);
}

void MenuState::ToggleRecording()
{
    bool new_value = !recording_;
    if (new_value == recording_)
    {
        return;
    }
    recording_ = new_value;
    NotifyChange(SettingType::Recording);
}

void MenuState::RequestGroundModeSkipSaveOnce()
{
    ground_mode_skip_save_once_ = true;
}

bool MenuState::ConsumeGroundModeSkipSaveOnce()
{
    bool flag = ground_mode_skip_save_once_;
    ground_mode_skip_save_once_ = false;
    return flag;
}

void MenuState::RequestGroundModeForceSaveOnce()
{
    ground_mode_force_save_once_ = true;
}

bool MenuState::ConsumeGroundModeForceSaveOnce()
{
    bool flag = ground_mode_force_save_once_;
    ground_mode_force_save_once_ = false;
    return flag;
}

void MenuState::ForceGroundModeNotifyOnce()
{
    force_ground_mode_notify_once_ = true;
}

bool MenuState::IsGroundModePersisted(const std::string &label) const
{
    return persisted_ground_modes_.count(label) > 0;
}

void MenuState::SetGroundModePersisted(const std::string &label, bool persisted)
{
    if (persisted)
    {
        persisted_ground_modes_.insert(label);
    }
    else
    {
        persisted_ground_modes_.erase(label);
    }
}
