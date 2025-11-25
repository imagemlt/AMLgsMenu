#include "application.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "video_mode.h"

#include <SDL_opengles2.h>

Application::Application() = default;
Application::~Application() { Shutdown(); }

bool Application::Initialize(const std::string &font_path) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_Log("Failed to init SDL2: %s", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    window_ = SDL_CreateWindow(
        "AML GS Menu",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window_) {
        SDL_Log("Failed to create SDL window: %s", SDL_GetError());
        Shutdown();
        return false;
    }

    gl_context_ = CreateGLContext(window_);
    if (!gl_context_) {
        SDL_Log("Failed to create GL context: %s", SDL_GetError());
        Shutdown();
        return false;
    }

    SDL_GL_MakeCurrent(window_, gl_context_);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    ImGui::StyleColorsDark();

    if (!font_path.empty()) {
        ImFont *font = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 18.0f);
        if (!font) {
            SDL_Log("Failed to load font at '%s', fallback to default font", font_path.c_str());
        }
    }

    ImGui_ImplSDL2_InitForOpenGL(window_, gl_context_);
    ImGui_ImplOpenGL3_Init("#version 100");

    auto sky_modes = DefaultSkyModes();
    auto ground_modes = LoadHdmiModes("/sys/class/amhdmitx/amhdmitx0/modes");
    if (ground_modes.empty()) {
        ground_modes = sky_modes;
    }

    menu_state_ = std::make_unique<MenuState>(sky_modes, ground_modes);
    renderer_ = std::make_unique<MenuRenderer>(*menu_state_);

    running_ = true;
    initialized_ = true;
    return true;
}

void Application::Run() {
    ImGuiIO &io = ImGui::GetIO();

    while (running_ && !menu_state_->ShouldExit()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_RIGHT) {
                menu_state_->ToggleMenuVisibility();
            }

            if (event.type == SDL_CONTROLLERBUTTONDOWN &&
                event.cbutton.button == SDL_CONTROLLER_BUTTON_X) {
                menu_state_->ToggleMenuVisibility();
            }

            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running_ = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        renderer_->Render(running_);

        ImGui::Render();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window_);
    }
}

void Application::Shutdown() {
    if (!initialized_) {
        return;
    }

    initialized_ = false;
    running_ = false;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (gl_context_) {
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
    }

    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_Quit();
}

SDL_GLContext Application::CreateGLContext(SDL_Window *window) {
    return SDL_GL_CreateContext(window);
}
