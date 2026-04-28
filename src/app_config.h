#pragma once

#include "menu_state.h"

#include <string>
#include <unordered_map>

class AppConfig
{
public:
    void SetConfigPath(const std::string &path) { config_path_ = path; }
    const std::string &ConfigPath() const { return config_path_; }

    bool EnsureWritable() const;
    bool LoadInto(MenuState &state, std::string &ssh_password);
    void SaveValue(const std::string &key, const std::string &value);
    void SaveAll() const;
    bool UpdateWifiClientConfig(const std::string &ssid, const std::string &password);

    std::unordered_map<std::string, std::string> &MutableValues() { return config_kv_; }
    const std::unordered_map<std::string, std::string> &Values() const { return config_kv_; }
    const std::string &WifiClientSsid() const { return wifi_client_ssid_; }
    const std::string &WifiClientPassword() const { return wifi_client_password_; }

private:
    bool LoadWifiClientConfig();
    bool WriteWifiClientConfig(const std::string &ssid, const std::string &password) const;

    std::unordered_map<std::string, std::string> config_kv_;
    std::string config_path_ = "/storage/digitalfpv/wfb.conf";
    std::string wifi_client_ssid_;
    std::string wifi_client_password_;
};
