cmake_minimum_required(VERSION 3.16)
project(AMLgsMenu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options for GLES builds
option(AML_ENABLE_GLES "Use OpenGL ES backends" ON)

add_executable(AMLgsMenu
    src/application.cpp
    src/main.cpp
    src/command_executor.cpp
    src/mavlink_receiver.cpp
    src/menu_renderer.cpp
    src/menu_state.cpp
    src/video_mode.cpp
    src/udp_command_client.cpp
)

if(AML_ENABLE_GLES)
    # OpenGL ES 2 is common on Amlogic platforms
    find_path(GLES2_INCLUDE_DIR NAMES GLES2/gl2.h)
    find_library(GLESv2_LIBRARY NAMES GLESv2)
    if(NOT GLESv2_LIBRARY)
        message(FATAL_ERROR "GLESv2 not found")
    endif()
    if(GLES2_INCLUDE_DIR)
        target_include_directories(AMLgsMenu PRIVATE ${GLES2_INCLUDE_DIR})
    endif()
    target_link_libraries(AMLgsMenu PRIVATE ${GLESv2_LIBRARY})
    target_compile_definitions(AMLgsMenu PRIVATE IMGUI_IMPL_OPENGL_ES2)
endif()

# ImGui is expected to be added as a subdirectory or provided by the toolchain
# Define IMGUI_ROOT to point to your local checkout if needed
set(IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH "Dear ImGui root directory")
set(MAVLINK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mavlink" CACHE PATH "MAVLink c_library_v2 root directory")

if (NOT EXISTS "${IMGUI_ROOT}/imgui.h")
    message(FATAL_ERROR "ImGui not found at ${IMGUI_ROOT}. Initialize submodule with `git submodule update --init --recursive` or set IMGUI_ROOT.")
endif()
if (NOT EXISTS "${MAVLINK_ROOT}/common/mavlink.h")
    message(FATAL_ERROR "MAVLink headers not found at ${MAVLINK_ROOT}. Initialize submodule with `git submodule update --init --recursive` or set MAVLINK_ROOT.")
endif()

set(IMGUI_SRC
    ${IMGUI_ROOT}/imgui.cpp
    ${IMGUI_ROOT}/imgui_demo.cpp
    ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp
)

target_sources(AMLgsMenu PRIVATE ${IMGUI_SRC})
target_include_directories(AMLgsMenu PRIVATE ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${MAVLINK_ROOT})

# Link against EGL if available for fbdev pipelines
find_library(EGL_LIBRARY EGL)
if(EGL_LIBRARY)
    target_link_libraries(AMLgsMenu PRIVATE ${EGL_LIBRARY})
endif()

find_path(LIBINPUT_INCLUDE_DIR libinput.h)
find_library(LIBINPUT_LIBRARY NAMES input libinput)
find_library(UDEV_LIBRARY NAMES udev)
if(NOT LIBINPUT_LIBRARY OR NOT UDEV_LIBRARY)
    message(FATAL_ERROR "libinput/udev not found")
endif()
target_include_directories(AMLgsMenu PRIVATE ${LIBINPUT_INCLUDE_DIR})
target_link_libraries(AMLgsMenu PRIVATE ${LIBINPUT_LIBRARY} ${UDEV_LIBRARY})

# PNG for icon loading
find_path(PNG_INCLUDE_DIR NAMES png.h)
find_library(PNG_LIBRARY NAMES png16 png)
if(NOT PNG_LIBRARY OR NOT PNG_INCLUDE_DIR)
    message(FATAL_ERROR "libpng not found")
endif()
find_library(ZLIB_LIBRARY NAMES z)
if(NOT ZLIB_LIBRARY)
    message(FATAL_ERROR "zlib not found (needed by libpng)")
endif()
target_include_directories(AMLgsMenu PRIVATE ${PNG_INCLUDE_DIR})
target_link_libraries(AMLgsMenu PRIVATE ${PNG_LIBRARY} ${ZLIB_LIBRARY})

# Basic warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(AMLgsMenu PRIVATE -Wall -Wextra -Wpedantic)
endif()
