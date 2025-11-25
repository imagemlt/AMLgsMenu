cmake_minimum_required(VERSION 3.16)
project(AMLgsMenu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options for GLES builds
option(AML_ENABLE_GLES "Use OpenGL ES backends" ON)

add_executable(aml_gs_menu
    src/application.cpp
    src/main.cpp
    src/menu_renderer.cpp
    src/menu_state.cpp
    src/video_mode.cpp
)

find_package(SDL2 REQUIRED)
target_link_libraries(aml_gs_menu PRIVATE SDL2::SDL2)

if(AML_ENABLE_GLES)
    # OpenGL ES 2 is common on Amlogic platforms
    find_package(OpenGL REQUIRED)
    target_link_libraries(aml_gs_menu PRIVATE OpenGL::GLES2)
    target_compile_definitions(aml_gs_menu PRIVATE IMGUI_IMPL_OPENGL_ES2)
endif()

# ImGui is expected to be added as a subdirectory or provided by the toolchain
# Define IMGUI_ROOT to point to your local checkout if needed
set(IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH "Dear ImGui root directory")

if (NOT EXISTS "${IMGUI_ROOT}/imgui.h")
    message(FATAL_ERROR "ImGui not found at ${IMGUI_ROOT}. Initialize submodule with `git submodule update --init --recursive` or set IMGUI_ROOT.")
endif()

set(IMGUI_SRC
    ${IMGUI_ROOT}/imgui.cpp
    ${IMGUI_ROOT}/imgui_demo.cpp
    ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp
)

target_sources(aml_gs_menu PRIVATE ${IMGUI_SRC})
target_include_directories(aml_gs_menu PRIVATE ${IMGUI_ROOT} ${IMGUI_ROOT}/backends)

# Link against EGL if available for fbdev pipelines
find_library(EGL_LIBRARY EGL)
if(EGL_LIBRARY)
    target_link_libraries(aml_gs_menu PRIVATE ${EGL_LIBRARY})
endif()

# Basic warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aml_gs_menu PRIVATE -Wall -Wextra -Wpedantic)
endif()
