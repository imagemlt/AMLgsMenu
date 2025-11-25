#include "video_mode.h"

#include <fstream>
#include <sstream>

std::vector<VideoMode> LoadHdmiModes(const std::string &path) {
    std::vector<VideoMode> modes;
    std::ifstream file(path);
    if (!file.is_open()) {
        return modes;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }

        VideoMode mode{};
        mode.label = line;

        std::stringstream ss(line);
        int value = 0;
        char p = 0;
        char hz[3] = {0};
        if (ss >> value >> p >> mode.refresh >> hz) {
            mode.height = value;
            mode.width = (value == 2160) ? 3840 : (value == 1080 ? 1920 : 1280);
        }

        modes.push_back(mode);
    }

    return modes;
}

std::vector<VideoMode> DefaultSkyModes() {
    return {
        {"1920x1080 @ 60Hz", 1920, 1080, 60},
        {"1920x1080 @ 30Hz", 1920, 1080, 30},
        {"1280x720 @ 60Hz", 1280, 720, 60},
        {"3840x2160 @ 30Hz", 3840, 2160, 30},
    };
}

std::string FormatVideoModeLabel(const VideoMode &mode) {
    std::ostringstream os;
    os << mode.label;
    if (mode.width && mode.height && mode.refresh) {
        os << " (" << mode.width << "x" << mode.height << "@" << mode.refresh << "Hz)";
    }
    return os.str();
}

