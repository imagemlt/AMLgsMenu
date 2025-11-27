# AMLgsMenu

透明 OSD 与配置菜单，面向仅有 GLES + fbdev 的 Amlogic 平台。使用 EGL/GLES2 + libinput，ImGui 为子模块；背景全透明，菜单区域不透明。

## 特性
- OSD（MAVLink 或 mock）：地平线、信号强度（dBm）、飞行模式、GPS+离家距离、视频码率/分辨率/刷新率、电池、天空/地面温度。未收到 MAVLink（且非 mock）时左上角小字 “WAITING”，其余元素不显示飞行模式。
- 信道：2.4G 1–13，5G {32,36,40,44,48,52,56,60,64,68,96,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165,169,173,177}；频宽 10/20/40 MHz。
- 天空分辨率：1280x720@120，1920x1080@90，1920x1080@60，2240x1260@60，3200x1800@30，3840x2160@20。地面分辨率自动读取 `/sys/class/amhdmitx/amhdmitx0/modes`，无则回退默认。
- 命令行：`-t 字体.ttf`（推荐粗体全字库），`-m 1` 强制 mock；默认 MAVLink 监听 0.0.0.0:14450 UDP。
- UDP 配置下发（单向）到 127.0.0.1:14650/14651：信道、频宽、天空分辨率/帧率（重启 majestic）、码率（Mbps→kbps）、天空功率（p*50 mBm）。本地 monitor 网卡同步信道/功率，带 HT20/HT40+ 后缀，无 monitor 时跳过。
- 图标默认路径 `/storage/digitalfpv/icons/`（建议透明 48x48 PNG）。文字白色黑描边；菜单不透明，OSD 纯透明背景。

## 运行
```bash
./AMLgsMenu                # 默认字体
./AMLgsMenu -t 字体.ttf    # 指定字体
./AMLgsMenu -m 1           # 强制 mock
./AMLgsMenu -f /flash/wfb.conf # 指定 wfb.conf 路径（默认 /flash/wfb.conf）
```
右键或手柄 X 键切换菜单；支持鼠标/键盘/手柄导航。

## 构建
依赖：CMake 3.16+、C++17、EGL/GLES2、libinput/udev、libpng/zlib；ImGui 子模块在 `third_party/imgui`。

CoreELEC 交叉示例（按当前工具链）：
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

## MAVLink
- 默认绑定 0.0.0.0:14450；收到首帧打印一次日志；未知飞行模式不显示。
- Mock 模式跳过接收器，始终有数据。

## 配置下发
- UDP（不等待回执）推送到 127.0.0.1:14650/14651：
  - 信道：`sed -i 's/channel=.../' /etc/wfb.conf && iwconfig wlan0 channel ...`
  - 频宽：`sed -i 's/bandwidth=.../' /etc/wfb.conf`
  - 天空分辨率/帧率：`cli -s .video0.size WxH && cli -s .video0.fps R && killall -1 majestic`
  - 码率：`cli -s .video0.bitrate <kbps> && curl -s 'http://localhost/api/v1/set?video0.bitrate=<kbps>'`
  - 天空功率：`sed -i 's/driver_txpower_override=.../' /etc/wfb.conf && iw dev wlan0 set txpower fixed <p*50>`
- 本地 monitor 网卡：遍历 monitor 接口并 `iw dev <dev> set channel <ch> HT20|HT40+`、`iw dev <dev> set txpower fixed <p*50>`。

## 图标与字体
- 图标路径：`/storage/digitalfpv/icons/`，建议透明 48x48。
- 推荐粗体、多语言字体防止发虚，用 `-t` 指定。

## 许可证与免责
- 许可证：GPL-3.0-only（见 LICENSE）。禁止非法/违规用途，后果自负，与作者无关。
