#include "display_platform.h"

#include <EGL/eglext.h>
#include <EGL/fbdev_window.h>

#include <cstdio>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

DisplayPlatform::~DisplayPlatform()
{
    Shutdown();
}

bool DisplayPlatform::Initialize()
{
    fb_fd_ = open("/dev/fb0", O_RDWR);
    if (fb_fd_ < 0)
    {
        std::perror("[AMLgsMenu] open(/dev/fb0)");
        return false;
    }

    fb_var_screeninfo vinfo{};
    if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vinfo) != 0)
    {
        std::perror("[AMLgsMenu] ioctl(FBIOGET_VSCREENINFO)");
        return false;
    }
    width_ = static_cast<int>(vinfo.xres);
    height_ = static_cast<int>(vinfo.yres);

    EGLint major = 0;
    EGLint minor = 0;
    EGLint num_configs = 0;
    EGLint attr[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE};

    egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display_ == EGL_NO_DISPLAY)
    {
        std::fprintf(stderr, "[AMLgsMenu] eglGetDisplay failed\n");
        return false;
    }
    if (!eglInitialize(egl_display_, &major, &minor))
    {
        std::fprintf(stderr, "[AMLgsMenu] eglInitialize failed\n");
        return false;
    }
    if (!eglChooseConfig(egl_display_, attr, &egl_config_, 1, &num_configs) || num_configs == 0)
    {
        std::fprintf(stderr, "[AMLgsMenu] eglChooseConfig failed\n");
        return false;
    }

    EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx_attr);
    if (egl_context_ == EGL_NO_CONTEXT)
    {
        std::fprintf(stderr, "[AMLgsMenu] eglCreateContext failed\n");
        return false;
    }

    native_window_.width = static_cast<unsigned short>(width_);
    native_window_.height = static_cast<unsigned short>(height_);
    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, &native_window_, nullptr);
    if (egl_surface_ == EGL_NO_SURFACE)
    {
        std::fprintf(stderr, "[AMLgsMenu] eglCreateWindowSurface failed\n");
        return false;
    }
    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_))
    {
        std::fprintf(stderr, "[AMLgsMenu] eglMakeCurrent failed\n");
        return false;
    }
    eglSwapInterval(egl_display_, 0);
    return true;
}

void DisplayPlatform::Shutdown()
{
    if (egl_display_ != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (egl_context_ != EGL_NO_CONTEXT)
    {
        eglDestroyContext(egl_display_, egl_context_);
        egl_context_ = EGL_NO_CONTEXT;
    }
    if (egl_surface_ != EGL_NO_SURFACE)
    {
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }
    if (egl_display_ != EGL_NO_DISPLAY)
    {
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }
    egl_config_ = nullptr;

    if (fb_fd_ >= 0)
    {
        close(fb_fd_);
        fb_fd_ = -1;
    }
    width_ = 0;
    height_ = 0;
}
