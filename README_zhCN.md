# AMLgsMenu

针对仅提供 OpenGL ES + fbdev 的 Amlogic 平台的无线链路配置/OSD 原型。应用使用 SDL2 负责输入和窗口，Dear ImGui 的 GLES2 后端绘制透明叠加层。底层视频由专用 video 层呈现，本程序的背景保持透明避免遮挡。

## 特性
- 透明叠加，菜单居中占屏幕约 1/4 区域，可随时显示/隐藏。
- 下拉选择：信道(34–179)、频宽(10/20/40 MHz)、码率(1–50 Mbps)、天空/地面功率(1–60)。
- 分辨率/刷新率：天空端使用默认列表，地面端自动读取 `/sys/class/amhdmitx/amhdmitx0/modes`，无模式时回落到默认值。
- 录制开/关按钮与退出按钮；右键或手柄 **X** 键唤出/关闭菜单。
- OSD mock 数据：中心水平线、信号强度、飞行模式、GPS+离家距离、视频码率/分辨率/刷新率、电池/温度信息，图标位置已预留占位（当前为绘制方块）。

## 运行
在构建目录：
```bash
./aml_gs_menu          # 默认字体
./aml_gs_menu -t /path/to/font.ttf   # 指定字体
```
窗口全屏，右键或手柄 **X** 键切换菜单显示，其余区域透明以显示底层视频。

## 构建
要求：CMake 3.16+、C++17 编译器、SDL2、GLES2（及 EGL 如果平台提供）。
ImGui 已作为子模块放在 `third_party/imgui`，可通过 `IMGUI_ROOT` 自定义。
```bash
cmake -S . -B build -DAML_ENABLE_GLES=ON
cmake --build build
```

## 模拟遥测/OSD 数据
- 所有演示数据集中在 `src/menu_renderer.cpp` 的 `BuildMockTelemetry`；渲染逻辑在 `DrawOsd`。用真实数据时，替换/移除 `BuildMockTelemetry` 并传入实际遥测。
- 水平线、信号/温度/电池/GPS/视频位置和透明度可在 `DrawOsd` 调整。
- 图标当前为占位方块。如需 PNG，加载成 ImGui 纹理，改 `draw_icon` 为 `AddImage`。

## 接入真实 MAVLink 的建议
1. **解析端**：独立线程或非阻塞循环读取 MAVLink，解析所需字段（模式、RSSI、GPS、温度、电池、码率等），填充共享的 `TelemetryData` 结构（可参考 `menu_renderer.cpp` 字段）。
2. **线程安全**：用互斥或无锁交换，让渲染线程获取最新 `TelemetryData`，替换 `BuildMockTelemetry(state_)` 为你的数据提供函数。
3. **飞行模式文本**：根据 MAVLink base mode/custom mode 映射字符串，写入 `TelemetryData::flight_mode`。
4. **图标**：加载 PNG 为纹理，`draw_icon` 改为 `AddImage` 绘制；保持 `icon_size`/`icon_gap` 以对齐文本。

## 将菜单配置应用到系统
- 目前菜单只更新 `MenuState`。需要下发时，可在选项回调或确认按钮中调用业务接口，或在 `MenuState` 添加回调：
  - 信道/频宽/功率/码率：调用射频配置接口。
  - 分辨率/刷新率：对接视频链路配置或重启/重协商命令。
  - 录制开关：在 `ToggleRecording` 后触发实际录制控制。
  - 确认/关闭：目前仅隐藏菜单，可在确认时集中提交当前选择。

## 其它
- 背景透明：请求 RGBA 缓冲并清屏 alpha 为 0；菜单/OSD 背景仅 0.2–0.25 透明度，底层视频可见。
- 若 Amlogic 驱动不支持 alpha，可考虑自定义合成路径或使用专用图层。
- ImGui 依赖：已作为子模块，缺失时执行 `git submodule update --init --recursive`。
