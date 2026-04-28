#pragma once

#include <EGL/egl.h>
#include <EGL/fbdev_window.h>

class DisplayPlatform
{
public:
    DisplayPlatform() = default;
    ~DisplayPlatform();

    bool Initialize();
    void Shutdown();

    int width() const { return width_; }
    int height() const { return height_; }
    int framebuffer_fd() const { return fb_fd_; }
    EGLDisplay display() const { return egl_display_; }
    EGLSurface surface() const { return egl_surface_; }
    EGLContext context() const { return egl_context_; }
    EGLConfig config() const { return egl_config_; }

private:
    int fb_fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLConfig egl_config_ = nullptr;
    fbdev_window native_window_{};
};
