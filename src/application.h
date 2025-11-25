#pragma once

#include "menu_renderer.h"
#include "menu_state.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <libinput.h>
#include <libudev.h>

#include <chrono>
#include <memory>
#include <string>

class Application {
public:
    Application();
    ~Application();

    bool Initialize(const std::string &font_path = "");
    void Run();
    void Shutdown();

private:
    struct FbContext {
        int fd = -1;
        int width = 0;
        int height = 0;
    };

    bool InitFramebuffer(FbContext &fb);
    bool InitEgl(const FbContext &fb);
    bool InitInput();
    void ProcessInput(bool &running);
    void HandleLibinputEvent(struct libinput_event *event, bool &running);
    void UpdateDeltaTime();

    FbContext fb_{};
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLConfig egl_config_ = nullptr;
    struct libinput *li_ctx_ = nullptr;
    struct udev *udev_ctx_ = nullptr;
    bool running_ = false;
    bool initialized_ = false;
    std::chrono::steady_clock::time_point last_frame_time_{};

    std::unique_ptr<MenuState> menu_state_;
    std::unique_ptr<MenuRenderer> renderer_;
};
