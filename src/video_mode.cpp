#include "video_mode.h"

#include <fstream>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <chrono>

namespace {
VideoMode MakeModeFromLegacy(int height, int refresh, const std::string &label) {
    VideoMode m{};
    m.label = label;
    m.height = height;
    m.refresh = refresh;
    switch (height) {
    case 2160: m.width = 3840; break;
    case 1080: m.width = 1920; break;
    case 720: m.width = 1280; break;
    case 576: m.width = 720; break;
    case 480: m.width = 720; break;
    default: m.width = 0; break;
    }
    return m;
}
} // namespace

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
        // trim trailing '*' and whitespace
        while (!line.empty() && (line.back() == '*' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }

        VideoMode mode{};
        mode.label = line;

        // Patterns: "1920x1080p60hz" or "1080p60hz"
        if (line.find('x') != std::string::npos) {
            auto xpos = line.find('x');
            auto ppos = line.find('p', xpos);
            auto hzpos = line.find("hz", ppos);
            if (xpos != std::string::npos && ppos != std::string::npos) {
                mode.width = std::stoi(line.substr(0, xpos));
                mode.height = std::stoi(line.substr(xpos + 1, ppos - xpos - 1));
                if (hzpos != std::string::npos) {
                    mode.refresh = std::stoi(line.substr(ppos + 1, hzpos - ppos - 1));
                }
            }
        } else {
            auto ppos = line.find('p');
            auto hzpos = line.find("hz", ppos);
            if (ppos != std::string::npos) {
                int h = std::stoi(line.substr(0, ppos));
                int r = (hzpos != std::string::npos) ? std::stoi(line.substr(ppos + 1, hzpos - ppos - 1)) : 0;
                mode = MakeModeFromLegacy(h, r, line);
            }
        }

        modes.push_back(mode);
    }

    return modes;
}

int GetOutputFps(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return 0;
    }
    std::string line;
    if (!std::getline(file, line)) {
        return 0;
    }
    std::stringstream ss(line);
    std::string token;
    auto parse_val = [](const std::string &s) -> int {
        if (s.empty()) return 0;
        char *end = nullptr;
        long v = std::strtol(s.c_str(), &end, 0);
        if (end == s.c_str()) return 0;
        return static_cast<int>(v);
    };
    int output = 0;
    int input = 0;
    while (ss >> token) {
        auto pos = token.find("output_fps:");
        if (pos != std::string::npos) {
            output = parse_val(token.substr(pos + std::strlen("output_fps:")));
        }
        pos = token.find("input_fps:");
        if (pos != std::string::npos) {
            input = parse_val(token.substr(pos + std::strlen("input_fps:")));
        }
    }
    if (output > 0) return output;
    return input;
}

float ReadTemperatureC(const std::string &path) {
    static auto last_read = std::chrono::steady_clock::time_point{};
    static float cached = 0.0f;

    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_read).count();
    const bool need_read = (last_read.time_since_epoch().count() == 0 || ms >= 1000);
    if (!need_read) {
        return cached;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return cached;
    }
    int milli = 0;
    file >> milli;
    cached = static_cast<float>(milli) / 1000.0f;
    last_read = now;
    return cached;
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
