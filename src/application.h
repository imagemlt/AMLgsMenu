#pragma once

#include "menu_renderer.h"
#include "menu_state.h"

#include <SDL.h>
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
    SDL_GLContext CreateGLContext(SDL_Window *window);

    SDL_Window *window_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;
    bool running_ = false;
    bool initialized_ = false;

    std::unique_ptr<MenuState> menu_state_;
    std::unique_ptr<MenuRenderer> renderer_;
};
