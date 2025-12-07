# AMLgsMenu

透明 OSD 与配置菜单，面向仅有 GLES + fbdev 的 Amlogic 平台。使用 EGL/GLES2 + libinput，ImGui 为子模块；背景全透明，菜单区域不透明。嵌入式终端借鉴自 MIT 许可的 [ImGui-Terminal](https://github.com/nealmick/ImGui-Terminal) 项目，感谢原作者提供的成熟终端模拟器。

## 特性
- OSD（MAVLink 或 mock）：地平线、信号强度（dBm）、飞行模式、GPS+离家距离、视频码率/分辨率/刷新率、电池、天空/地面温度。地面天线信号直接从 `wifibroadcast` 服务的 `RX_ANT` 日志中解析，保持与实际天线状态一致，服务缺失时不会崩溃而是沿用上一次值。未收到 MAVLink（且非 mock）时左上角小字 “WAITING”，其余元素不显示飞行模式。
- 信道：2.4G 1–13，5G {32,36,40,44,48,52,56,60,64,68,96,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165,169,173,177}；频宽 10/20/40 MHz。
- 天空分辨率：1280x720@120，1920x1080@90，1920x1080@60，2240x1260@60，3200x1800@30，3840x2160@20。地面分辨率自动读取 `/sys/class/amhdmitx/amhdmitx0/modes`，无则回退默认。
- 命令行：`-t 字体.ttf`（推荐粗体全字库）用于 UI，`-T 字体.ttf` 可选指定终端字体，`-m 1` 强制 mock；默认 MAVLink 监听 0.0.0.0:14450 UDP。
- UDP 配置下发（单向）到 127.0.0.1:14650/14651：信道、频宽、天空分辨率/帧率（重启 majestic）、码率（Mbps→kbps）、天空功率（p*50 mBm）。本地 monitor 网卡同步信道/功率，带 HT20/HT40+ 后缀，无 monitor 时跳过。
- 图标默认路径 `/storage/digitalfpv/icons/`（建议透明 48x48 PNG）。文字白色黑描边；菜单不透明，OSD 纯透明背景。
- 固件传输模式可选：**CC 固件（UDP）** 与 **官方固件（SSH）**。两者共用 `command.cfg`，菜单切换后自动切换到底层传输；SSH 连接 `root@10.5.0.10`（密码 `12345`）。
- 自定义 OSD 文本（格式示例见文末“图标与字体”小节）。

## 运行
```bash
./AMLgsMenu                     # 默认字体
./AMLgsMenu -t 字体.ttf         # 指定 UI 字体
./AMLgsMenu -T 字体.ttf         # 指定终端字体（默认使用 UI 字体）
./AMLgsMenu -m 1                # 强制 mock
./AMLgsMenu -c /flash/command.cfg # 指定 command.cfg 路径（默认 /flash/command.cfg）
./AMLgsMenu -f /flash/wfb.conf    # 指定 wfb.conf 路径（默认 /flash/wfb.conf）
./AMLgsMenu -h | --help           # 查看帮助
```
右键或手柄 X 键切换菜单；支持鼠标/键盘/手柄导航。

## 构建
依赖：CMake 3.16+、C++17、EGL/GLES2、libinput/udev、libpng/zlib、libssh；ImGui 子模块在 `third_party/imgui`。

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

## 固件模式
- “固件模式”下拉可切换 **CC 固件（UDP）** 与 **官方固件（SSH）**。UDP 模式使用本地 127.0.0.1:14650/14651；官方模式会对 `root@10.5.0.10`（密码 `12345`）发起短连接 SSH，并在同一个 `command.cfg` 模板上执行命令/查询。
- 选择会写入 `/flash/wfb.conf` 的 `firmware=cc|official`，也可以手动编辑该键值。切换后程序会重新拉取一次天空端状态，使菜单显示同步。
- 编译/部署前请确保系统包含 libssh。

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
- 推荐粗体、多语言字体防止发虚，用 `-t` 指定，终端可通过 `-T` 继续使用不同字体。
- 自定义 OSD 例子:
  ```ini
  [osd]
  cpu = 100|60|/storage/digitalfpv/scripts/osd_cpu.sh
  temp = 100|90|printf \"TMP %.1fC\" $(cat /sys/class/thermal/thermal_zone0/temp)
  ```
  坐标以屏幕左上角为原点。

## 操作提示
- 鼠标：右键开关菜单，左键/滚轮操作控件。
- 键盘：Alt 开关菜单，方向键移动焦点，Enter 进入/确认选项，Ctrl+C 退出程序。终端窗口打开时键盘输入优先给终端，菜单操作建议改用鼠标。
- 手柄（Xbox 布局）：X 开关菜单，方向键/摇杆移动，A 确认。
- 终端模式建议同时连接键盘和鼠标，避免依赖手柄时焦点被终端占用。

## 许可证与免责
- 许可证：GPL-3.0-only（见 LICENSE）。禁止非法/违规用途，后果自负，与作者无关。
