# AMLgsMenu

[简体中文说明](README_zhCN.md)

Transparent OSD and configuration UI for AML-based fbdev + GLES targets. Uses EGL/OpenGL ES2 + libinput; ImGui is bundled as a submodule. Background is fully transparent except the menu panel. Embedded terminal functionality is adapted from the MIT-licensed [ImGui-Terminal](https://github.com/nealmick/ImGui-Terminal) project—thanks to its author for the advanced terminal emulator.

## Features
- OSD mock (or MAVLink) data: horizon, signals (dBm), flight mode, GPS + home distance, video bitrate/resolution/refresh, battery, sky/ground temps. Ground antenna readings now parse the latest `RX_ANT` entries from the `wifibroadcast` journal so the OSD mirrors realtime signal variation; if the service is unavailable the existing display simply keeps the last known value. When no MAVLink is received (and not in mock), only a small “WAITING” tag shows in the top-left.
- Channel list aligned to Digi: 2.4G 1–13 + 5G {32,36,40,44,48,52,56,60,64,68,96,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165,169,173,177}. Bandwidth 10/20/40 MHz.
- Sky modes aligned to Digi: 1280x720@120, 1920x1080@90, 1920x1080@60, 2240x1260@60, 3200x1800@30, 3840x2160@20. Ground modes auto-read from `/sys/class/amhdmitx/amhdmitx0/modes` with fallback.
- Command-line: `-t <font.ttf>` custom UI font (recommend bold CJK font), `-T <font.ttf>` custom terminal font, `-m 1` forces mock data; default binds MAVLink UDP 0.0.0.0:14450.
- UDP config push (fire-and-forget) to 127.0.0.1:14650/14651: channel, bandwidth, sky mode (size/fps, restart majestic), bitrate (Mbps→kbps), sky power (p*50 mBm). Ground power/channel also apply to local monitor interfaces via `iw` (with HT20/HT40+ suffix).
- Icons default path `/storage/digitalfpv/icons/` (PNG, e.g., 48x48). Text is white with black outline, menu opaque; OSD fully transparent behind.
- Custom text overlays: add an `[osd]` section in `command.cfg` with entries like `label = 120|80|/storage/digitalfpv/scripts/osd_cpu.sh`; the binary runs each command every 2s, captures the first line, and renders it at `(x,y)` relative to the top-left corner.

## Build
Deps: C++17, CMake 3.16+, EGL/OpenGL ES2, libinput/udev, libpng/zlib; ImGui submodule under `third_party/imgui`.

CoreELEC cross example (from user toolchain):
```bash
export TOOLCHAIN=/home/docker/CoreELEC/build.CoreELEC-Amlogic-ng.arm-21/toolchain
export SYSROOT=${TOOLCHAIN}/armv8a-libreelec-linux-gnueabihf/sysroot
export CXX=${TOOLCHAIN}/bin/armv8a-libreelec-linux-gnueabihf-g++
export CC=${TOOLCHAIN}/bin/armv8a-libreelec-linux-gnueabihf-gcc
cmake -S . -B build-ng \
  -DCMAKE_C_COMPILER=${CC} \
  -DCMAKE_CXX_COMPILER=${CXX} \
  -DCMAKE_SYSROOT=${SYSROOT} \
  -DIMGUI_ROOT=$(pwd)/third_party/imgui \
  -DAML_ENABLE_GLES=ON
cmake --build build-ng
```

## Run
```bash
./AMLgsMenu -t /path/to/font.ttf      # optional UI font
./AMLgsMenu -T /path/to/font.ttf      # optional terminal font
./AMLgsMenu -m 1                      # force mock
./AMLgsMenu -c /flash/command.cfg     # override command template config (default /flash/command.cfg)
./AMLgsMenu -f /flash/wfb.conf        # override wfb.conf path (default /flash/wfb.conf)
./AMLgsMenu -h | --help               # show usage summary
```
Right-click or gamepad X toggles the menu; controller navigation enabled.

## MAVLink
- Receiver binds 0.0.0.0:14450 UDP; first message logs once. Flight mode hidden if unknown. Mock mode bypasses receiver.

## Icons & fonts
- Default icon lookup: `/storage/digitalfpv/icons/`. Use ~48x48 transparent PNGs.
- Use a bold CJK-compatible font for clarity; pass via `-t`. Terminal still follows `-T` (falls back to the UI font when unset).
- Custom overlays: extend `command.cfg` with
  ```ini
  [osd]
  cpu = 80|60|sh -c 'printf \"CPU %.1f%%\" $(awk \'{print $1}\' /tmp/cpu)'
  temp = 80|90|cat /storage/digitalfpv/temp.txt
  ```
  Coordinates are in screen pixels from the top-left origin.

## Input tips
- Mouse: right-click toggles the menu, left-click/wheel behave like a desktop app.
- Keyboard: Alt toggles the menu; arrows move focus; Enter confirms combos/buttons; Ctrl+C quits the app. When the terminal is open, keyboard events are captured by the terminal (menu navigation falls back to mouse).
- Gamepad (Xbox layout): X toggles the menu; D-Pad moves; A confirms selection.
- For the embedded terminal, best experience is with both keyboard and mouse attached; avoid relying on the controller while the terminal window is active.

## Safety / license
- Licensed under GPL-3.0-only (see LICENSE). No warranty; commercial/illegal use is forbidden and entirely at user’s own risk.
