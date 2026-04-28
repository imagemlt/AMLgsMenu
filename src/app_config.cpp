#include "app_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>

namespace
{
constexpr const char kDefaultStorageConfigPath[] = "/storage/digitalfpv/wfb.conf";
constexpr const char kFlashConfigPath[] = "/flash/wfb.conf";
constexpr const char kWifiClientStoragePath[] = "/storage/.config/connman_main.conf";

std::string TrimCopy(const std::string &text)
{
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
    {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string IniValue(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value)
    {
        if (c == '\n' || c == '\r')
        {
            out.push_back(' ');
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

int FindChannelIndex(const MenuState &state, int channel_val)
{
    const auto &chs = state.Channels();
    for (size_t i = 0; i < chs.size(); ++i)
    {
        if (chs[i] == channel_val)
            return static_cast<int>(i);
    }
    return -1;
}

int FindPowerIndex(const MenuState &state, int power_val)
{
    const auto &pwr = state.PowerLevels();
    for (size_t i = 0; i < pwr.size(); ++i)
    {
        if (pwr[i] == power_val)
            return static_cast<int>(i);
    }
    return -1;
}

int FindMcsIndex(const MenuState &state, int mcs_val)
{
    const auto &mcs = state.McsLevels();
    for (size_t i = 0; i < mcs.size(); ++i)
    {
        if (mcs[i] == mcs_val)
            return static_cast<int>(i);
    }
    return -1;
}

int FindGroundModeIndex(const MenuState &state, const std::string &label)
{
    const auto &modes = state.GroundModes();
    for (size_t i = 0; i < modes.size(); ++i)
    {
        if (modes[i].label == label)
            return static_cast<int>(i);
    }
    return -1;
}
} // namespace

// Convert SSID string to hex representation (used for ConnMan service filename)
static std::string SSIDToHex(const std::string &ssid)
{
    std::ostringstream oss;
    for (unsigned char c : ssid)
    {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return oss.str();
}

bool AppConfig::EnsureWritable() const
{
    if (config_path_.empty())
    {
        return false;
    }
    namespace fs = std::filesystem;
    fs::path target(config_path_);
    std::error_code ec;
    fs::path parent = target.parent_path();
    if (!parent.empty() && !fs::exists(parent, ec))
    {
        if (!fs::create_directories(parent, ec))
        {
            std::fprintf(stderr, "[AMLgsMenu] Failed to create directory '%s': %s\n",
                         parent.string().c_str(), ec.message().c_str());
            return false;
        }
    }
    if (fs::exists(target, ec))
    {
        return true;
    }

    if (config_path_ != kDefaultStorageConfigPath)
    {
        return true;
    }

    std::ifstream src(kFlashConfigPath, std::ios::binary);
    if (!src.is_open())
    {
        std::fprintf(stderr, "[AMLgsMenu] Source config '%s' not found\n", kFlashConfigPath);
        return false;
    }
    std::ofstream dst(config_path_, std::ios::binary | std::ios::trunc);
    if (!dst.is_open())
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to open '%s' for writing\n", config_path_.c_str());
        return false;
    }
    dst << src.rdbuf();
    if (!dst.good())
    {
        std::fprintf(stderr, "[AMLgsMenu] Failed to copy config to '%s'\n", config_path_.c_str());
        return false;
    }
    return true;
}

bool AppConfig::LoadInto(MenuState &state, std::string &ssh_password)
{
    config_kv_.clear();

    std::ifstream file(config_path_);
    if (!file.is_open())
    {
        return false;
    }
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t\r\n") + 1);
        config_kv_[key] = val;
    }

    LoadWifiClientConfig();

    bool channel_loaded = false;
    auto it_ch = config_kv_.find("channel");
    if (it_ch != config_kv_.end())
    {
        int ch = std::stoi(it_ch->second);
        int idx = FindChannelIndex(state, ch);
        if (idx >= 0)
        {
            state.SetChannelIndex(idx);
            channel_loaded = true;
        }
    }
    if (!channel_loaded)
    {
        int default_channel = 161;
        int idx = FindChannelIndex(state, default_channel);
        if (idx < 0 && !state.Channels().empty())
            idx = FindChannelIndex(state, state.Channels()[0]);
        if (idx >= 0 && !state.Channels().empty())
        {
            state.SetChannelIndex(idx);
            config_kv_["channel"] = std::to_string(state.Channels()[idx]);
        }
    }

    bool bandwidth_loaded = false;
    auto it_bw = config_kv_.find("bandwidth");
    if (it_bw != config_kv_.end())
    {
        int bw = std::stoi(it_bw->second);
        if (bw == 10)
        {
            state.SetBandwidthIndex(0);
            bandwidth_loaded = true;
        }
        else if (bw == 20)
        {
            state.SetBandwidthIndex(1);
            bandwidth_loaded = true;
        }
        else if (bw == 40)
        {
            state.SetBandwidthIndex(2);
            bandwidth_loaded = true;
        }
    }
    if (!bandwidth_loaded)
    {
        state.SetBandwidthIndex(1);
        config_kv_["bandwidth"] = "20";
    }

    bool power_loaded = false;
    auto it_power = config_kv_.find("driver_txpower_override");
    if (it_power != config_kv_.end())
    {
        int p = std::stoi(it_power->second);
        int idx = FindPowerIndex(state, p);
        if (idx >= 0)
        {
            state.SetGroundPowerIndex(idx);
            power_loaded = true;
        }
    }
    if (!power_loaded)
    {
        int default_power = 5;
        int idx = FindPowerIndex(state, default_power);
        if (idx < 0)
            idx = 0;
        if (idx >= 0 && !state.PowerLevels().empty())
        {
            state.SetGroundPowerIndex(idx);
            config_kv_["driver_txpower_override"] = std::to_string(state.PowerLevels()[idx]);
        }
    }

    const auto &mcs_levels = state.McsLevels();
    if (!mcs_levels.empty())
    {
        int default_mcs = 2;
        int idx = FindMcsIndex(state, default_mcs);
        if (idx < 0)
            idx = 0;
        if (idx >= 0 && idx < static_cast<int>(mcs_levels.size()))
        {
            state.SetSkyMcsIndex(idx);
        }
    }

    bool ground_mode_loaded = false;
    auto it_res = config_kv_.find("ground_res");
    if (it_res == config_kv_.end())
    {
        it_res = config_kv_.find("groud_res");
    }
    if (it_res != config_kv_.end())
    {
        std::string label = it_res->second;
        while (!label.empty() && (label.back() == '*' || label.back() == ' ' || label.back() == '\t'))
            label.pop_back();
        int idx = FindGroundModeIndex(state, label);
        if (idx >= 0)
        {
            state.SetGroundModeIndex(idx);
            const auto &modes = state.GroundModes();
            if (idx < static_cast<int>(modes.size()) && modes[idx].refresh > 60)
            {
                state.SetGroundModePersisted(modes[idx].label, true);
            }
            ground_mode_loaded = true;
        }
    }
    if (!ground_mode_loaded)
    {
        std::string fallback = "1080p60hz";
        int idx = FindGroundModeIndex(state, fallback);
        if (idx < 0)
            idx = 0;
        const auto &modes = state.GroundModes();
        if (idx >= 0 && idx < static_cast<int>(modes.size()))
        {
            state.SetGroundModeIndex(idx);
            config_kv_["ground_res"] = modes[idx].label;
        }
    }

    auto it_lang = config_kv_.find("lang");
    if (it_lang != config_kv_.end())
    {
        std::string v = it_lang->second;
        for (auto &c : v)
            c = static_cast<char>(tolower(c));
        if (v == "en")
            state.SetLanguage(MenuState::Language::EN);
        else if (v == "cn")
            state.SetLanguage(MenuState::Language::CN);
    }
    auto it_sshpass = config_kv_.find("ssh_pass");
    if (it_sshpass != config_kv_.end())
    {
        ssh_password = it_sshpass->second;
    }
    auto it_fw = config_kv_.find("firmware");
    if (it_fw != config_kv_.end())
    {
        std::string v = it_fw->second;
        for (auto &c : v)
            c = static_cast<char>(tolower(c));
        if (v == "official")
        {
            state.SetFirmwareType(MenuState::FirmwareType::Official);
        }
        else
        {
            state.SetFirmwareType(MenuState::FirmwareType::CCEdition);
        }
    }
    auto it_sound = config_kv_.find("sound");
    if (it_sound != config_kv_.end())
    {
        std::string v = it_sound->second;
        for (auto &c : v)
            c = static_cast<char>(tolower(c));
        bool enabled = (v == "1" || v == "true" || v == "yes");
        state.SetSoundEnabled(enabled);
    }
    else
    {
        config_kv_["sound"] = "0";
        state.SetSoundEnabled(false);
    }

    auto it_alink = config_kv_.find("alink");
    if (it_alink != config_kv_.end())
    {
        std::string v = it_alink->second;
        for (auto &c : v)
            c = static_cast<char>(tolower(c));
        bool enabled = (v == "1" || v == "true" || v == "yes");
        state.SetAdaptiveLinkEnabled(enabled);
    }
    else
    {
        config_kv_["alink"] = "0";
        state.SetAdaptiveLinkEnabled(false);
    }

    const auto &volume_levels = state.VolumeLevels();
    if (!volume_levels.empty())
    {
        int volume_value = volume_levels.back();
        auto it_vol = config_kv_.find("sound_volume");
        if (it_vol != config_kv_.end())
        {
            try
            {
                volume_value = std::stoi(it_vol->second);
            }
            catch (const std::exception &)
            {
                volume_value = volume_levels.back();
            }
        }
        else
        {
            config_kv_["sound_volume"] = std::to_string(volume_levels.back());
        }
        int idx = 0;
        for (int i = 0; i < static_cast<int>(volume_levels.size()); ++i)
        {
            if (volume_levels[i] == volume_value)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0 || idx >= static_cast<int>(volume_levels.size()))
        {
            idx = static_cast<int>(volume_levels.size()) - 1;
        }
        state.SetSoundVolumeIndex(idx);
        config_kv_["sound_volume"] = std::to_string(volume_levels[idx]);
    }

    const auto &buffer_levels = state.BufferLevels();
    if (!buffer_levels.empty())
    {
        int buffer_value = 1;
        auto it_buf = config_kv_.find("buf_level");
        if (it_buf != config_kv_.end())
        {
            try
            {
                buffer_value = std::stoi(it_buf->second);
            }
            catch (const std::exception &)
            {
                buffer_value = 1;
            }
        }
        int idx = 0;
        for (int i = 0; i < static_cast<int>(buffer_levels.size()); ++i)
        {
            if (buffer_levels[i] == buffer_value)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0 || idx >= static_cast<int>(buffer_levels.size()))
        {
            idx = 0;
        }
        state.SetBufferLevelIndex(idx);
        config_kv_["buf_level"] = std::to_string(buffer_levels[idx]);
    }

    const auto &hdmi_attrs = state.HdmiAttrs();
    if (!hdmi_attrs.empty())
    {
        std::string value = "rgb";
        auto it_hdmi = config_kv_.find("hdmi_attr");
        if (it_hdmi != config_kv_.end())
        {
            value = it_hdmi->second;
        }
        int idx = 0;
        for (int i = 0; i < static_cast<int>(hdmi_attrs.size()); ++i)
        {
            if (hdmi_attrs[i] == value)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0 || idx >= static_cast<int>(hdmi_attrs.size()))
        {
            idx = 2 < static_cast<int>(hdmi_attrs.size()) ? 2 : 0;
            value = hdmi_attrs[idx];
        }
        state.SetHdmiAttrIndex(idx);
        config_kv_["hdmi_attr"] = value;
    }

    auto it_ap_mode = config_kv_.find("ap_mode");
    if (it_ap_mode != config_kv_.end())
    {
        int mode = 0;
        try
        {
            mode = std::stoi(it_ap_mode->second);
        }
        catch (const std::exception &)
        {
            mode = 0;
        }
        state.SetNetworkModeIndex(mode == 1 ? 1 : 0);
    }
    else
    {
        config_kv_["ap_mode"] = "0";
        state.SetNetworkModeIndex(0);
    }

    auto it_bad = config_kv_.find("bad_frame");
    if (it_bad != config_kv_.end())
    {
        int val = 176;
        try
        {
            val = std::stoi(it_bad->second);
        }
        catch (const std::exception &)
        {
            val = 176;
        }
        state.SetBadFrameIndex(val == 0 ? 0 : 1);
    }
    else
    {
        config_kv_["bad_frame"] = "176";
        state.SetBadFrameIndex(1);
    }

    return true;
}

void AppConfig::SaveValue(const std::string &key, const std::string &value)
{
    config_kv_[key] = value;
    const std::string tmp = config_path_ + ".tmp" + std::to_string(std::rand());

    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open())
            return;

        for (const auto &kv : config_kv_)
            file << kv.first << "=" << kv.second << "\n";
    }
    std::rename(tmp.c_str(), config_path_.c_str());
}

void AppConfig::SaveAll() const
{
    const std::string tmp = config_path_ + ".tmp";

    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open())
            return;

        for (const auto &kv : config_kv_)
            file << kv.first << "=" << kv.second << "\n";
    }
    std::rename(tmp.c_str(), config_path_.c_str());
}

bool AppConfig::LoadWifiClientConfig()
{
    wifi_client_ssid_.clear();
    wifi_client_password_.clear();

    std::ifstream file(kWifiClientStoragePath);
    if (!file.is_open())
    {
        return false;
    }

    std::string line;
    bool in_section = false;
    while (std::getline(file, line))
    {
        std::string trimmed = TrimCopy(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        if (!in_section)
        {
            if (trimmed == "[service_AMLgsMenu]")
            {
                in_section = true;
            }
            continue;
        }
        else
        {
            // Stop when another section begins
            if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']')
            {
                break;
            }
            auto pos = trimmed.find('=');
            if (pos == std::string::npos)
                continue;
            const std::string key = TrimCopy(trimmed.substr(0, pos));
            const std::string value = TrimCopy(trimmed.substr(pos + 1));
            if (key == "Name")
                wifi_client_ssid_ = value;
            else if (key == "Passphrase")
                wifi_client_password_ = value;
        }
    }

    return !wifi_client_ssid_.empty();
}

bool AppConfig::WriteWifiClientConfig(const std::string &ssid, const std::string &password) const
{
    namespace fs = std::filesystem;

    const std::string safe_ssid = IniValue(TrimCopy(ssid));
    const std::string safe_password = IniValue(password);
    if (safe_ssid.empty())
    {
        return false;
    }

    // Build the new service block
    std::ostringstream new_block;
    new_block << "[service_AMLgsMenu]\n";
    new_block << "Type = wifi\n";
    new_block << "Name = " << safe_ssid << "\n";
    new_block << "IPv4 = dhcp\n";

    new_block << "AutoConnect = true\n";
    new_block << "Favorite = true\n"; // allow auto‑connect on boot
    if (!safe_password.empty())
    {
        new_block << "Security = psk\n";
        new_block << "Passphrase = " << safe_password << "\n";
    }
    new_block << "\n";

    // Prepare temporary file path
    const std::string tmp_path = std::string(kWifiClientStoragePath) + ".tmp" + std::to_string(std::rand());

    // Read existing file and keep lines not belonging to our service section
    std::vector<std::string> kept;
    {
        std::ifstream src(kWifiClientStoragePath);
        if (src)
        {
            std::string line;
            bool in_section = false;
            while (std::getline(src, line))
            {
                std::string trimmed = TrimCopy(line);
                if (!in_section)
                {
                    if (trimmed == "[service_AMLgsMenu]")
                    {
                        in_section = true;
                        continue; // skip old section header
                    }
                    else
                    {
                        kept.push_back(line);
                    }
                }
                else
                {
                    // Inside old section, stop when another section begins
                    if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']')
                    {
                        in_section = false;
                        kept.push_back(line); // keep the next section header
                    }
                    // otherwise skip line
                }
            }
        }
    }

    // Ensure target directory exists
    fs::path path(kWifiClientStoragePath);
    fs::path parent = path.parent_path();
    if (!parent.empty())
    {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec)
        {
            std::fprintf(stderr, "[AMLgsMenu] Failed to create '%s': %s\n",
                         parent.string().c_str(), ec.message().c_str());
            return false;
        }
    }

    // Write merged content to temporary file
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out)
            return false;
        for (const auto &l : kept)
            out << l << "\n";
        out << new_block.str();
        if (!out.good())
        {
            std::fprintf(stderr, "[AMLgsMenu] Failed to write WiFi client config '%s'\n",
                         tmp_path.c_str());
            return false;
        }
    }

    // Write ConnMan service file for automatic connection (persistent cache)
    {
        const std::string connman_dir = "/storage/.cache/connman";
        std::error_code ec;
        fs::create_directories(connman_dir, ec);
        if (ec) {
            std::fprintf(stderr, "[AMLgsMenu] Failed to create ConnMan cache dir '%s': %s\n",
                         connman_dir.c_str(), ec.message().c_str());
            // continue anyway – if the dir cannot be created the Wi‑Fi config will be lost on reboot
        }
        const std::string connman_path = connman_dir + "/wifi.config";
        std::ofstream connout(connman_path, std::ios::trunc);
        if (connout) {
            connout << "[service_AMLgsMenu]\n";
            connout << "Type = wifi\n";
            connout << "Name = " << safe_ssid << "\n";
            connout << "IPv4 = dhcp\n";
            connout << "AutoConnect = true\n";
            connout << "Favorite = true\n";
            if (!safe_password.empty()) {
                connout << "Security = psk\n";
                connout << "Passphrase = " << safe_password << "\n";
            }
        } else {
            std::fprintf(stderr, "[AMLgsMenu] Failed to write ConnMan cache file '%s'\n", connman_path.c_str());
        }
    }

    // Atomically replace the original file
    std::rename(tmp_path.c_str(), kWifiClientStoragePath);
    return true;
}

bool AppConfig::UpdateWifiClientConfig(const std::string &ssid, const std::string &password)
{
    const std::string trimmed_ssid = TrimCopy(ssid);
    if (trimmed_ssid.empty())
    {
        std::fprintf(stderr, "[AMLgsMenu] WiFi client SSID is empty\n");
        return false;
    }
    if (!WriteWifiClientConfig(trimmed_ssid, password))
    {
        return false;
    }
    wifi_client_ssid_ = trimmed_ssid;
    wifi_client_password_ = password;
    return true;
}
