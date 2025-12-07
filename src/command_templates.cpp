#include "command_templates.h"

#include <fstream>
#include <iostream>

namespace {
std::string Trim(const std::string &s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
} // namespace

CommandTemplates::CommandTemplates() {
    InitDefaults();
}

bool CommandTemplates::LoadFromFile(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[AMLgsMenu] command.cfg not found: " << path << "\n";
        return false;
    }
    std::string line;
    std::string section;
    while (std::getline(file, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = Trim(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }
        auto pos = trimmed.find('=');
        if (pos == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, pos));
        std::string value = Trim(trimmed.substr(pos + 1));
        if (section.empty()) continue;
        commands_[section][key] = value;
    }
    return true;
}

std::string CommandTemplates::Render(const std::string &section, const std::string &key,
                                     const std::unordered_map<std::string, std::string> &params) const {
    const auto sec = commands_.find(section);
    const auto def_sec = defaults_.find(section);
    const auto lookup = [key](const std::unordered_map<std::string, std::string> &map) -> const std::string * {
        auto it = map.find(key);
        return it != map.end() ? &it->second : nullptr;
    };
    const std::string *templ = nullptr;
    if (sec != commands_.end()) templ = lookup(sec->second);
    if (!templ && def_sec != defaults_.end()) templ = lookup(def_sec->second);
    if (!templ) return {};
    return ReplaceVars(*templ, params);
}

std::string CommandTemplates::ReplaceVars(std::string templ,
                                          const std::unordered_map<std::string, std::string> &params) {
    for (const auto &[key, value] : params) {
        std::string placeholder = "${" + key + "}";

        size_t pos = 0;
        while ((pos = templ.find(placeholder, pos)) != std::string::npos) {
            templ.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    }
    return templ;
}

void CommandTemplates::InitDefaults() {
    defaults_["remote"]["channel"] =
        "sed -i 's/channel=.*$/channel=${CHANNEL}/' /etc/wfb.conf && iwconfig wlan0 channel ${CHANNEL}";
    defaults_["remote"]["bandwidth"] =
        "sed -i 's/bandwidth=.*$/bandwidth=${BANDWIDTH}/' /etc/wfb.conf";
    defaults_["remote"]["sky_mode"] =
        "cli -s .video0.size ${WIDTH}x${HEIGHT} && cli -s .video0.fps ${FPS} && killall -1 majestic";
    defaults_["remote"]["bitrate"] =
        "cli -s .video0.bitrate ${BITRATE_KBPS} && curl -s 'http://localhost/api/v1/set?video0.bitrate=${BITRATE_KBPS}'";
    defaults_["remote"]["sky_power"] =
        "sed -i 's/driver_txpower_override=.*$/driver_txpower_override=${POWER}/' /etc/wfb.conf && iw dev wlan0 set txpower fixed ${TXPOWER}";
    defaults_["remote_query"]["channel"] =
        "awk -F= '/^channel=/{print $2; exit}' /etc/wfb.conf";
    defaults_["remote_query"]["bandwidth"] =
        "awk -F= '/^bandwidth=/{print $2; exit}' /etc/wfb.conf";
    defaults_["remote_query"]["sky_power"] =
        "awk -F= '/^driver_txpower_override=/{print $2; exit}' /etc/wfb.conf";
    defaults_["remote_query"]["bitrate"] =
        "cli -g .video0.bitrate";
    defaults_["remote_query"]["sky_size"] =
        "cli -g .video0.size";
    defaults_["remote_query"]["sky_fps"] =
        "cli -g .video0.fps";

    defaults_["local"]["monitor_channel"] =
        "sh -c 'for dev in $(iw dev 2>/dev/null | awk '\\''/Interface/ {iface=$2} /type[[:space:]]+monitor/ {print iface}'\\''); "
        "do iw dev $dev set channel ${CHANNEL}${BW_SUFFIX}; done'";
    defaults_["local"]["monitor_power"] =
        "sh -c 'for dev in $(iw dev 2>/dev/null | awk '\\''/Interface/ {iface=$2} /type[[:space:]]+monitor/ {print iface}'\\''); "
        "do iw dev $dev set txpower fixed ${TXPOWER}; done'";
}
