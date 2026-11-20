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
        "([ -f /etc/wfb.conf ] && sh -c 'tmp=/tmp/wfb_conf.$$.tmp; cp /etc/wfb.conf \"$tmp\" && sed -i \"s/^channel=.*/channel=$1/\" \"$tmp\" && cat \"$tmp\" > /etc/wfb.conf && rm -f \"$tmp\"' sh \"${CHANNEL}\" || wifibroadcast cli -s .wireless.channel \"${CHANNEL}\") && iwconfig wlan0 channel ${CHANNEL}";
    defaults_["remote"]["bandwidth"] =
        "([ -f /etc/wfb.conf ] && sh -c 'tmp=/tmp/wfb_conf.$$.tmp; cp /etc/wfb.conf \"$tmp\" && sed -i \"s/^bandwidth=.*/bandwidth=$1/\" \"$tmp\" && cat \"$tmp\" > /etc/wfb.conf && rm -f \"$tmp\"' sh \"${BANDWIDTH}\" || wifibroadcast cli -s .wireless.width ${BANDWIDTH} )";
    defaults_["remote"]["sky_mode"] =
        "if [ -f /customer/config/camera.conf ]; then sed -i -e 's/^video_width=.*/video_width=${WIDTH}/' "
        "-e 's/^video_height=.*/video_height=${HEIGHT}/' -e 's/^video_framerate=.*/video_framerate=${FPS}/' /customer/config/camera.conf "
        "&& (if [ -x /etc/init.d/S95camera ]; then /etc/init.d/S95camera reload; "
        "elif [ -x /customer/S95camera ]; then /customer/S95camera reload; else echo 'S95camera reload script not found' >&2; exit 1; fi); "
        "else cli -s .video0.size ${WIDTH}x${HEIGHT} && cli -s .video0.fps ${FPS} && killall -1 majestic; fi";
    defaults_["remote"]["bitrate"] =
        "(if [ -f /customer/config/camera.conf ]; then sed -i 's/^video_bitrate=.*/video_bitrate=${BITRATE_KBPS}/' /customer/config/camera.conf; "
        "else cli -s .video0.bitrate ${BITRATE_KBPS}; fi) && curl -s 'http://localhost/api/v1/set?video0.bitrate=${BITRATE_KBPS}'";
    defaults_["remote"]["sky_power"] =
        "([ -f /etc/wfb.conf ] && sh -c 'tmp=/tmp/wfb_conf.$$.tmp; cp /etc/wfb.conf \"$tmp\" && sed -i \"s/^driver_txpower_override=.*/driver_txpower_override=$1/\" \"$tmp\" && cat \"$tmp\" > /etc/wfb.conf && rm -f \"$tmp\"' sh \"${POWER}\" || wifibroadcast cli -s .wireless.txpower ${POWER} ) && iw dev wlan0 set txpower fixed ${TXPOWER}";
    defaults_["remote"]["sky_mcs"] =
        "([ -f /etc/wfb.conf ] && sh -c 'tmp=/tmp/wfb_conf.$$.tmp; cp /etc/wfb.conf \"$tmp\" && sed -i \"s/^mcs_index=.*/mcs_index=$1/\" \"$tmp\" && cat \"$tmp\" > /etc/wfb.conf && rm -f \"$tmp\" && . /etc/wfb.conf && wfb_tx_cmd 8000 set_radio -B $bandwidth -M $mcs_index -S $stbc -L $ldpc -G $guard_interval' sh \"${MCS}\" || wifibroadcast cli -s .broadcast.mcs_index ${MCS} )";
    defaults_["remote"]["ack_time"] =
        "sh -c 'found=0; failed=0; for path in /proc/net/*/*/ack_timeout; do [ -f \"$path\" ] || continue; found=1; if echo \"$1\" > \"$path\"; then echo \"[AMLgsMenu] Sky ACK time $1 applied: $path\"; else echo \"[AMLgsMenu] Sky ACK time write failed: $path\" >&2; failed=1; fi; done; if [ \"$found\" = 0 ]; then echo \"[AMLgsMenu] No sky ack_timeout found under /proc/net\" >&2; exit 1; fi; exit \"$failed\"' sh \"${ACK_TIME}\"";
    defaults_["remote"]["request_idr"] =
        "curl -s http://localhost/request/idr >/dev/null";
    defaults_["remote_query"]["channel"] =
        "awk -F= '/^channel=/{print $2; exit}' /etc/wfb.conf";
    defaults_["remote_query"]["bandwidth"] =
        "awk -F= '/^bandwidth=/{print $2; exit}' /etc/wfb.conf";
    defaults_["remote_query"]["sky_power"] =
        "awk -F= '/^driver_txpower_override=/{print $2; exit}' /etc/wfb.conf";
    defaults_["remote_query"]["sky_mcs"] =
        "awk -F= '/^mcs_index=/{print $2; exit}' /etc/wfb.conf";
    defaults_["remote_query"]["bitrate"] =
        "if [ -f /customer/config/camera.conf ]; then sed -n 's/^video_bitrate=//p' /customer/config/camera.conf; else cli -g .video0.bitrate; fi";
    defaults_["remote_query"]["sky_size"] =
        "if [ -f /customer/config/camera.conf ]; then awk -F= '$1==\"video_width\"{w=$2+0} $1==\"video_height\"{h=$2+0} "
        "END{if(w>0 && h>0) print w \"x\" h}' /customer/config/camera.conf; else cli -g .video0.size; fi";
    defaults_["remote_query"]["sky_fps"] =
        "if [ -f /customer/config/camera.conf ]; then sed -n 's/^video_framerate=//p' /customer/config/camera.conf; else cli -g .video0.fps; fi";
    defaults_["remote_query"]["camera_config"] =
        "if [ -f /customer/config/camera.conf ]; then echo camera_conf; fi";

    defaults_["local"]["monitor_channel"] =
        "sh -c 'for dev in $(iw dev 2>/dev/null | awk '\\''/Interface/ {iface=$2} /type[[:space:]]+monitor/ {print iface}'\\''); "
        "do iw dev $dev set channel ${CHANNEL}${BW_SUFFIX}; done'";
    defaults_["local"]["monitor_power"] =
        "sh -c 'cfg=/storage/digitalfpv/wfb.conf; [ -f \"$cfg\" ] || cfg=/flash/wfb.conf; [ -f \"$cfg\" ] && . \"$cfg\"; "
        "if [ \"${ap_mode:-0}\" = \"1\" ]; then dev=; for candidate in $(iw dev 2>/dev/null | awk '\\''/Interface/ {print $2}'\\''); do "
        "if iw dev \"$candidate\" link 2>/dev/null | grep -q '\\''^Connected to '\\''; then dev=$candidate; break; fi; done; "
        "[ -n \"$dev\" ] && iw dev \"$dev\" set txpower fixed ${TXPOWER}; "
        "else for dev in $(iw dev 2>/dev/null | awk '\\''/Interface/ {iface=$2} /type[[:space:]]+monitor/ {print iface}'\\''); "
        "do iw dev \"$dev\" set txpower fixed ${TXPOWER}; done; fi'";
    defaults_["local"]["ack_time"] =
        "sh -c 'found=0; failed=0; for path in /proc/net/*/*/ack_timeout; do [ -f \"$path\" ] || continue; found=1; if echo ${ACK_TIME} > \"$path\"; then echo \"[AMLgsMenu] ACK time ${ACK_TIME} applied: $path\"; else echo \"[AMLgsMenu] ACK time write failed: $path\" >&2; failed=1; fi; done; if [ \"$found\" = 0 ]; then echo \"[AMLgsMenu] No ack_timeout found under /proc/net\" >&2; exit 1; fi; exit \"$failed\"'";
    defaults_["local"]["bad_frame_drop"] =
        "echo 0 > /sys/module/amvdec_h265/parameters/error_handle_policy && "
        "echo 0 > /sys/module/amvdec_h265/parameters/hacked_lowlatency && "
        "echo 2 > /sys/module/amvdec_h265/parameters/nal_skip_policy && "
        "echo 1,1,1,1,1,1,1,1,1 > /sys/module/amvdec_h265/parameters/ref_frame_mark_flag";
    defaults_["local"]["bad_frame_ignore"] =
        "echo 432 > /sys/module/amvdec_h265/parameters/error_handle_policy && "
        "echo 1 > /sys/module/amvdec_h265/parameters/hacked_lowlatency && "
        "echo 0 > /sys/module/amvdec_h265/parameters/nal_skip_policy && "
        "echo 0 > /sys/module/amvdec_h265/parameters/ref_frame_mark_flag";
}
