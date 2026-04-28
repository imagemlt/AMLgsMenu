#include "splash_player.h"

#include "imgui.h"
#include "splash_data.h"

#include <zlib.h>

namespace
{
constexpr int kSplashHoldMs = 1500;
}

SplashPlayer::~SplashPlayer()
{
    Shutdown();
}

void SplashPlayer::Init()
{
    Shutdown();
    if (kSplashFrameCount <= 0)
    {
        return;
    }
    textures_.resize(kSplashFrameCount, 0);
    glGenTextures(kSplashFrameCount, textures_.data());
    std::vector<unsigned char> decoded;
    for (int i = 0; i < kSplashFrameCount; ++i)
    {
        if (!DecompressFrame(i, decoded))
        {
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, textures_[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kSplashWidth, kSplashHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded.data());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    frame_offsets_.clear();
    total_duration_ms_ = 0;
    const int delay_len = static_cast<int>(sizeof(kSplashFrameDelayMs) / sizeof(kSplashFrameDelayMs[0]));
    for (int i = 0; i < kSplashFrameCount; ++i)
    {
        frame_offsets_.push_back(total_duration_ms_);
        int delay_ms = (i < delay_len) ? kSplashFrameDelayMs[i] : (delay_len > 0 ? kSplashFrameDelayMs[delay_len - 1] : 0);
        total_duration_ms_ += delay_ms;
    }
    start_time_ = std::chrono::steady_clock::now();
    active_ = true;
}

void SplashPlayer::RenderOverlay()
{
    if (!active_ || textures_.empty())
    {
        return;
    }
    if (total_duration_ms_ <= 0)
    {
        Shutdown();
        return;
    }
    auto now = std::chrono::steady_clock::now();
    int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count());
    const int splash_end_with_hold = total_duration_ms_ + kSplashHoldMs;
    if (elapsed >= splash_end_with_hold)
    {
        Shutdown();
        return;
    }
    float alpha = 1.0f;
    int frame = kSplashFrameCount - 1;
    if (elapsed < total_duration_ms_)
    {
        const int delay_len = static_cast<int>(sizeof(kSplashFrameDelayMs) / sizeof(kSplashFrameDelayMs[0]));
        for (int i = 0; i < kSplashFrameCount; ++i)
        {
            int start = (i < static_cast<int>(frame_offsets_.size())) ? frame_offsets_[i] : 0;
            int delay = (i < delay_len) ? kSplashFrameDelayMs[i] : (delay_len > 0 ? kSplashFrameDelayMs[delay_len - 1] : 0);
            if (delay <= 0)
            {
                delay = 1;
            }
            if (elapsed < start + delay)
            {
                frame = i;
                break;
            }
        }
    }
    else
    {
        const int hold_elapsed = elapsed - total_duration_ms_;
        if (kSplashHoldMs > 0)
        {
            alpha = 1.0f - static_cast<float>(hold_elapsed) / static_cast<float>(kSplashHoldMs);
            if (alpha < 0.0f)
            {
                alpha = 0.0f;
            }
        }
    }
    if (frame < 0 || frame >= static_cast<int>(textures_.size()))
    {
        return;
    }
    GLuint tex = textures_[frame];
    if (tex == 0)
    {
        return;
    }
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float x = viewport->Pos.x + (viewport->Size.x - static_cast<float>(kSplashWidth)) * 0.5f;
    const float y = viewport->Pos.y + (viewport->Size.y - static_cast<float>(kSplashHeight)) * 0.5f;
    ImVec2 min(x, y);
    ImVec2 max(x + static_cast<float>(kSplashWidth), y + static_cast<float>(kSplashHeight));
    ImGui::GetBackgroundDrawList()->AddImage(
        (ImTextureID)(intptr_t)(tex), min, max, ImVec2(0, 0), ImVec2(1, 1),
        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
}

void SplashPlayer::Shutdown()
{
    if (!textures_.empty())
    {
        glDeleteTextures(static_cast<GLsizei>(textures_.size()), textures_.data());
    }
    textures_.clear();
    frame_offsets_.clear();
    total_duration_ms_ = 0;
    active_ = false;
    start_time_ = {};
}

bool SplashPlayer::DecompressFrame(int index, std::vector<unsigned char> &buffer) const
{
    if (index < 0 || index >= kSplashFrameCount)
    {
        return false;
    }
    const auto &rec = kSplashFrameRecords[index];
    if (rec.uncompressed_length == 0)
    {
        return false;
    }
    buffer.resize(rec.uncompressed_length);
    uLongf dest_len = rec.uncompressed_length;
    const unsigned char *src = kSplashCompressedData + rec.offset;
    int ret = uncompress(buffer.data(), &dest_len, src, rec.length);
    if (ret != Z_OK || dest_len != rec.uncompressed_length)
    {
        return false;
    }
    return true;
}
