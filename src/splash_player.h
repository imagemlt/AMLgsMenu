#pragma once

#include <GLES2/gl2.h>

#include <chrono>
#include <vector>

class SplashPlayer
{
public:
    ~SplashPlayer();

    void Init();
    void RenderOverlay();
    void Shutdown();

private:
    bool DecompressFrame(int index, std::vector<unsigned char> &buffer) const;

    bool active_ = false;
    std::vector<GLuint> textures_;
    std::vector<int> frame_offsets_;
    int total_duration_ms_ = 0;
    std::chrono::steady_clock::time_point start_time_{};
};
