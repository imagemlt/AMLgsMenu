#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class CommandTemplates {
public:
    struct CustomOsdCommand {
        std::string key;
        float x = 0.0f;
        float y = 0.0f;
        std::string command;
    };

    CommandTemplates();
    bool LoadFromFile(const std::string &path);
    std::string Render(const std::string &section, const std::string &key,
                       const std::unordered_map<std::string, std::string> &params) const;
    const std::vector<CustomOsdCommand> &CustomOsdEntries() const { return custom_osd_; }

private:
    void InitDefaults();
    static std::string ReplaceVars(std::string templ, const std::unordered_map<std::string, std::string> &params);
    static std::vector<std::string> Split(const std::string &value, char delim);

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> commands_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> defaults_;
    std::vector<CustomOsdCommand> custom_osd_;
};
