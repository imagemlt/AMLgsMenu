#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class CommandTemplates {
public:
    CommandTemplates();
    bool LoadFromFile(const std::string &path);
    std::string Render(const std::string &section, const std::string &key,
                       const std::unordered_map<std::string, std::string> &params) const;

private:
    void InitDefaults();
    static std::string ReplaceVars(std::string templ, const std::unordered_map<std::string, std::string> &params);

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> commands_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> defaults_;
};
