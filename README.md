# AMLgsMenu

A prototype wireless link configuration menu for Amlogic platforms that only expose OpenGL ES and fbdev. The application uses SDL2 for input and windowing, and Dear ImGui with the OpenGL ES 2.0 backend to render a transparent overlay menu centered on the display.

## Features
- Transparent, fixed-position overlay occupying roughly the center quarter of the screen that can be toggled at runtime.
- Drop-down selections for channel (34–179), bandwidth (10/20/40 MHz), bitrate (1–50 Mbps), and power levels (1–60) for both sky and ground sides.
- Resolution/refresh selection with defaults for the sky endpoint and automatic discovery of HDMI modes for the ground endpoint via `/sys/class/amhdmitx/amhdmitx0/modes`, with sensible fallbacks.
- Toggle button to start/stop recording and an exit button to close the program.

## UI layout and styling
- Starts hidden; a right-click or controller **X** button reveals the menu so the display remains unobstructed until needed.
- The menu window is centered on the screen and sized to half the viewport in each dimension, occupying the middle quarter of the display.
- A lightly darkened, 25% opacity background keeps the underlying video visible while isolating controls for readability.
- Simple vertical flow: wireless link title and separator followed by drop-downs for channel, bandwidth, sky and ground resolution/refresh, bitrate, and power controls, plus recording and exit buttons.
- Uses the default ImGui dark theme for consistent padding, typography, and focus highlighting across mouse, keyboard, and controller input.

## Prerequisites
- A toolchain with CMake 3.16+ and a C++17-capable compiler.
- SDL2 development headers and libraries.
- OpenGL ES 2.0 libraries (and EGL if available on the target platform).
- Dear ImGui sources available locally; set `IMGUI_ROOT` to point to the checkout or place it under `third_party/imgui`.

## Building
```bash
cmake -S . -B build \
  -DIMGUI_ROOT=/path/to/imgui \
  -DAML_ENABLE_GLES=ON
cmake --build build
```

## Running
From the build directory:
```bash
./aml_gs_menu
```

The application opens a full-screen window. Toggle the menu with a right-click or the controller **X** button; once open, it renders the transparent configuration UI in the center. Interact with keyboard, mouse, or controller inputs supported by SDL2.

## Notes
- The prototype focuses on UI rendering; selecting values does not yet apply system-level configuration.
- On platforms without HDMI mode exposure, the ground resolution list falls back to the bundled defaults.
