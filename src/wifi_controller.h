#pragma once

#include <functional>
#include <string>
#include <vector>

// 一张可用于连 AP 的无线网卡
struct WifiIface
{
    std::string name;       // wlan1
    std::string driver;     // rtl88x2cs
    std::string mac;
    std::string bands;      // 2.4G / 5G / 2.4G+5G
    bool up = false;
    std::string ssid;       // 已连接时非空
};

// ConnMan 里的一个 wifi service（= 一个 AP）
// 与 CoreELEC 官方一致：列表来自 ConnMan 的 Manager.GetServices，
// 不自己跑 iw scan（iw scan 会打断当前关联）。
struct WifiAp
{
    std::string ssid;
    std::string service_path;  // wifi_<mac>_<ssid_hex>_managed_psk
    int signal_dbm = 0;        // ConnMan 的 Strength（0-100）
    bool secure = false;
    std::string state;         // idle / ready / online / failure
};

enum class WifiState
{
    Disconnected = 0,
    Scanning,
    Connecting,
    Connected,
    Failed,
};

struct WifiStatus
{
    WifiState state = WifiState::Disconnected;
    std::string ssid;
    int signal_dbm = 0;
    std::string ip;
};

// Wi-Fi 操作层（全部内置，不依赖外部脚本）。
//
// 这台 CoreELEC 的 wifi 由 ConnMan 承担：
//   · kodi 21 源码里没有任何 wifi 代码；
//   · CoreELEC 的 connman 用 --enable-wifi 编译，后端是 wpa_supplicant -u；
//   · 系统里没有 libdbus，所以只能通过 connmanctl / iw 命令行操作
//     （与本项目 command_executor 的 std::system 风格一致）。
//
// 所有耗时操作在后台线程跑，UI 每帧 Poll 取结果，不阻塞渲染。
class WifiController
{
public:
    // ---- 网卡列表（同步，很快）----
    static std::vector<WifiIface> ListIfaces();

    // ---- 扫描（后台）----
    void StartScan(const std::string &iface);
    // 返回 true 表示取到了新结果（取走后清空）
    bool PollScan(std::vector<WifiAp> *out, bool *scanning);

    // ---- 连接（后台）----
    void StartConnect(const std::string &ssid, const std::string &psk, const std::string &iface);
    // 返回 true 表示连接流程已结束（成功或失败）
    bool PollConnect(bool *ok, std::string *message, bool *running);

    // ---- 连接（同步，供切换网络模式等非 UI 路径使用）----
    static bool ConnectSync(const std::string &ssid, const std::string &psk,
                            const std::string &iface, std::string *message = nullptr);

    // ---- 状态（轻量，可直接同步查）----
    static WifiStatus QueryStatus(const std::string &iface);

    // ---- 断开 ----
    static void Disconnect(const std::string &iface);

    // ---- 工作模式 ----
    // WFB 模式下网卡要给 wifibroadcast 当 monitor，绝不能连 AP。
    // 判定与 /opt/bin/wifibroadcast 一致：
    //   ap/true/1/yes/on   -> STA/AP 模式
    //   wfb/false/0/no/off -> WFB 模式
    static bool IsWfbMode();
    // 读不到配置时返回空串
    static std::string ApModeValue();
    // 从配置里取要连的 SSID（[service_AMLgsMenu] Name，其次 openipc_sta.conf）
    static std::string ConfiguredSsid();

    // 开机自动连接：延迟 delay_s 秒后在后台执行一次。
    // WFB 模式、或已有 192.168.144.x 地址时自动跳过。
    static void AutoConnectOnBoot(int delay_s = 20);

    // 当前正在操作的网卡（UI 选中项）
    void SetIface(const std::string &iface) { iface_ = iface; }
    const std::string &Iface() const { return iface_; }

private:
    std::string iface_;
};
