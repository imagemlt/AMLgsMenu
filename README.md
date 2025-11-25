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
# 指定字体 (可选)
./aml_gs_menu -t /path/to/font.ttf
```

The application opens a full-screen window. Toggle the menu with a right-click or the controller **X** button; once open, it renders the transparent configuration UI in the center. Interact with keyboard, mouse, or controller inputs supported by SDL2.

## Telemetry / OSD mock data
- OSD 所有演示数据集中在 `src/menu_renderer.cpp` 的 `BuildMockTelemetry`，渲染在 `DrawOsd` 中。替换成真实数据时，修改/移除 `BuildMockTelemetry` 并传入实际遥测即可。
- 水平线、信号/温度/电池/GPS/视频等元素全部在 `DrawOsd`，布局和透明度可在此处调节。

## 接入真实 MAVLink 的建议
1) **解析端**：在独立线程或非阻塞循环读取 MAVLink，解析需要的字段（姿态/模式、RSSI、GPS、温度、电池、电台码率等），填充一个共享的 `TelemetryData` 结构（可复制 `menu_renderer.cpp` 中的字段定义）。
2) **线程安全**：用简单的互斥锁或无锁交换，将最新 TelemetryData 提供给渲染线程，替换 `BuildMockTelemetry(state_)` 调用为获取真实数据。
3) **飞行模式文本**：根据 MAVLink 的 base mode/custom mode 映射到字符串，直接传给 `TelemetryData::flight_mode`。
4) **图标**：当前是绘制占位方块，若要用 PNG，加载为 ImGui 纹理并将 `draw_icon` 替换为 `AddImage`。

## 将菜单配置应用到系统的建议
- 目前菜单仅更新内存中的 `MenuState`。要下发配置，可在 `DrawMenu` 的按钮/选择回调里调用你的业务层接口，或在 `MenuState` 里新增回调/观察者：
  - 频道/频宽/功率/码率：调用射频配置接口。
  - 分辨率/刷新率：对接视频链路设置或发出重启/重协商命令。
  - 录象开关：在 `ToggleRecording` 调用后触发实际录制控制。
  - 确认/关闭：目前只是隐藏菜单，可在确认时集中提交当前选择。

## Notes
- The prototype focuses on UI rendering; selecting values does not yet apply system-level configuration until you connect the hooks described above.
- On platforms without HDMI mode exposure, the ground resolution list falls back to the bundled defaults.
- Development currently tracks the `main` branch; pull the latest `main` tip before building to ensure code and build files are in sync.
