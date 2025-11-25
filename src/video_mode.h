#pragma once

#include <string>
#include <vector>

struct VideoMode {
    std::string label;
    int width{};
    int height{};
    int refresh{};
};

std::vector<VideoMode> LoadHdmiModes(const std::string &path);
std::vector<VideoMode> DefaultSkyModes();
std::string FormatVideoModeLabel(const VideoMode &mode);

