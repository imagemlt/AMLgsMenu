#include "wifi_controller.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace
{

constexpr const char kConnmanConf[] = "/storage/.config/connman_main.conf";
constexpr const char kWfbConf[] = "/storage/digitalfpv/wfb.conf";
constexpr const char kOpenipcConf[] = "/storage/openipc_sta.conf";
constexpr const char kSysNet[] = "/sys/class/net";

// ---------------- 基础工具 ----------------

std::string Trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> Split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == sep)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// 执行命令并返回 stdout
std::string Run(const std::string &cmd)
{
    std::string out;
    FILE *p = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p)
        return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p))
        out += buf;
    pclose(p);
    return out;
}

void RunVoid(const std::string &cmd)
{
    std::system((cmd + " >/dev/null 2>&1").c_str());
}

std::string ReadFileStr(const std::string &path)
{
    std::ifstream f(path);
    if (!f)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string ReadFirstLine(const std::string &path)
{
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    return Trim(line);
}

// 读 key=value 形式的配置
std::string ReadKeyValue(const std::string &path, const std::string &key)
{
    std::ifstream f(path);
    if (!f)
        return {};
    std::string line;
    while (std::getline(f, line))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        if (Trim(line.substr(0, eq)) == key)
            return Trim(line.substr(eq + 1));
    }
    return {};
}

// ---------------- 网卡信息 ----------------

bool IsWifiIface(const std::string &iface)
{
    return access((std::string(kSysNet) + "/" + iface + "/wireless").c_str(), F_OK) == 0;
}

std::string DriverOf(const std::string &iface)
{
    char buf[512];
    std::string link = std::string(kSysNet) + "/" + iface + "/device/driver";
    ssize_t n = readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    std::string s(buf);
    size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

std::string MacOf(const std::string &iface)
{
    return ReadFirstLine(std::string(kSysNet) + "/" + iface + "/address");
}

bool IsUp(const std::string &iface)
{
    return ReadFirstLine(std::string(kSysNet) + "/" + iface + "/operstate") == "up";
}

// WFB 虚拟卡不参与 CA 连接
bool IsReservedIface(const std::string &iface)
{
    return iface.rfind("wlan_wfb", 0) == 0;
}

std::vector<std::string> AllWifiIfaces()
{
    std::vector<std::string> out;
    DIR *d = opendir(kSysNet);
    if (!d)
        return out;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr)
    {
        std::string n = e->d_name;
        if (n == "." || n == "..")
            continue;
        if (IsWifiIface(n))
            out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// 按 phy 支持的频率判断频段能力
std::string BandsOf(const std::string &iface)
{
    std::string phy = ReadFirstLine(std::string(kSysNet) + "/" + iface + "/phy80211/name");
    if (phy.empty())
        return "?";
    std::string info = Run("iw phy " + phy + " info");
    bool b24 = info.find("24") != std::string::npos && info.find(" MHz") != std::string::npos &&
               (info.find("2412 MHz") != std::string::npos || info.find("2437 MHz") != std::string::npos ||
                info.find("2462 MHz") != std::string::npos);
    bool b5 = info.find("5180 MHz") != std::string::npos || info.find("5745 MHz") != std::string::npos ||
              info.find("5825 MHz") != std::string::npos;
    if (b24 && b5)
        return "2.4G+5G";
    if (b5)
        return "5G";
    if (b24)
        return "2.4G";
    return "?";
}

// 从 `iw dev <if> link` 里取 SSID / 信号
std::string SsidOf(const std::string &iface)
{
    std::string link = Run("iw dev " + iface + " link");
    std::istringstream iss(link);
    std::string line;
    while (std::getline(iss, line))
    {
        size_t pos = line.find("SSID:");
        if (pos != std::string::npos)
            return Trim(line.substr(pos + 5));
    }
    return {};
}

// 拿某个接口的 IPv4 地址（getifaddrs，避免再起进程）
std::string Ipv4Of(const std::string &iface)
{
    struct ifaddrs *ifa = nullptr;
    if (getifaddrs(&ifa) != 0)
        return {};
    std::string out;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next)
    {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (iface != p->ifa_name)
            continue;
        char buf[INET_ADDRSTRLEN] = {0};
        auto *sin = reinterpret_cast<struct sockaddr_in *>(p->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
        {
            out = buf;
            break;
        }
    }
    freeifaddrs(ifa);
    return out;
}

// ---------------- ConnMan 交互 ----------------

struct ServiceProps
{
    std::string name;
    std::string state;
    int strength = 0;
    bool secure = false;
    bool found = false;
};

ServiceProps QueryService(const std::string &path)
{
    ServiceProps sp;
    std::string out = Run("connmanctl services " + path);
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key == "Name")
            sp.name = val;
        else if (key == "State")
            sp.state = val;
        else if (key == "Strength")
            sp.strength = std::atoi(val.c_str());
        else if (key == "Security")
            sp.secure = (val.find("none") == std::string::npos && !val.empty());
        sp.found = true;
    }
    return sp;
}

// 列出 ConnMan 里所有 wifi service 的 path
std::vector<std::string> ListWifiServices()
{
    std::vector<std::string> out;
    std::string raw = Run("connmanctl services");
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line))
    {
        // 每行末段是 service 路径，形如 wifi_xxx_managed_psk
        size_t p = line.find_last_of(" \t");
        std::string last = (p == std::string::npos) ? Trim(line) : Trim(line.substr(p + 1));
        if (last.rfind("wifi_", 0) == 0)
            out.push_back(last);
    }
    return out;
}

// 在 ConnMan 的 service 列表里按 SSID 找路径
std::string FindServiceBySsid(const std::string &ssid)
{
    for (const auto &p : ListWifiServices())
    {
        ServiceProps sp = QueryService(p);
        if (sp.name == ssid)
            return p;
    }
    return {};
}

// 当前黑名单
std::string ReadBlacklist()
{
    std::ifstream f(kConnmanConf);
    if (!f)
        return {};
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("NetworkInterfaceBlacklist=", 0) == 0)
            return Trim(line.substr(25));
    }
    return {};
}

// 把 iface 从黑名单里去掉（不写则已存在）
bool UnblacklistIface(const std::string &iface)
{
    std::string cur = ReadBlacklist();
    if (cur.empty())
        return false;
    auto items = Split(cur, ',');
    bool hit = false;
    std::vector<std::string> keep;
    for (auto &it : items)
    {
        std::string t = Trim(it);
        if (t.empty())
            continue;
        if (t == iface)
            hit = true;
        else
            keep.push_back(t);
    }
    if (!hit)
        return false;

    std::string joined;
    for (size_t i = 0; i < keep.size(); ++i)
    {
        if (i)
            joined += ",";
        joined += keep[i];
    }

    // 原地替换该行，保留其它配置
    std::ifstream in(kConnmanConf);
    if (!in)
        return false;
    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(in, line))
    {
        if (!replaced && line.rfind("NetworkInterfaceBlacklist=", 0) == 0)
        {
            lines.push_back("NetworkInterfaceBlacklist=" + joined);
            replaced = true;
        }
        else
            lines.push_back(line);
    }
    in.close();

    std::ofstream out(kConnmanConf, std::ios::trunc);
    if (!out)
        return false;
    for (auto &l : lines)
        out << l << "\n";
    return true;
}

// 写 [service_AMLgsMenu] 段（ConnMan 官方格式，config.c 要求 service_ 前缀）
bool WriteServiceConfig(const std::string &ssid, const std::string &psk)
{
    std::ifstream in(kConnmanConf);
    std::vector<std::string> lines;
    std::string line;
    bool skipping = false;
    while (std::getline(in, line))
    {
        if (line.rfind("[service_AMLgsMenu]", 0) == 0)
        {
            skipping = true;
            continue;
        }
        if (skipping && line.rfind("[", 0) == 0)
            skipping = false;
        if (!skipping)
            lines.push_back(line);
    }
    in.close();

    // 去掉尾部空行，保持整洁
    while (!lines.empty() && Trim(lines.back()).empty())
        lines.pop_back();

    lines.push_back("[service_AMLgsMenu]");
    lines.push_back("Type = wifi");
    lines.push_back("Name = " + ssid);
    lines.push_back("IPv4 = dhcp");
    lines.push_back("AutoConnect = true");
    lines.push_back("Favorite = true");
    if (!psk.empty())
    {
        lines.push_back("Security = psk");
        lines.push_back("Passphrase = " + psk);
    }

    std::ofstream out(kConnmanConf, std::ios::trunc);
    if (!out)
        return false;
    for (auto &l : lines)
        out << l << "\n";
    return true;
}

// ---------------- 后台任务的共享状态 ----------------

std::mutex g_scan_mu;
std::vector<WifiAp> g_scan_result;
std::atomic<bool> g_scanning{false};
std::atomic<bool> g_scan_fresh{false};

std::mutex g_conn_mu;
std::atomic<bool> g_conn_running{false};
std::atomic<bool> g_conn_done{false};
std::atomic<bool> g_conn_ok{false};
std::string g_conn_msg;

// 等到某个接口拿到 IPv4（最多 timeout_s 秒）
bool WaitForIp(const std::string &iface, int timeout_s)
{
    for (int i = 0; i < timeout_s; ++i)
    {
        if (!Ipv4Of(iface).empty())
            return true;
        sleep(1);
    }
    return !Ipv4Of(iface).empty();
}

} // namespace

// ================= 公共接口 =================

std::vector<WifiIface> WifiController::ListIfaces()
{
    std::vector<WifiIface> out;
    for (const auto &n : AllWifiIfaces())
    {
        if (IsReservedIface(n))
            continue;
        WifiIface it;
        it.name = n;
        it.driver = DriverOf(n);
        it.mac = MacOf(n);
        it.bands = BandsOf(n);
        it.up = IsUp(n);
        it.ssid = SsidOf(n);
        out.push_back(it);
    }
    return out;
}

std::string WifiController::ApModeValue()
{
    std::string v = ReadKeyValue(kWfbConf, "ap_mode");
    if (v.empty())
        v = ReadKeyValue("/flash/wfb.conf", "ap_mode");
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return v;
}

bool WifiController::IsWfbMode()
{
    const std::string v = ApModeValue();
    // 与 /opt/bin/wifibroadcast 的判定保持一致
    if (v == "ap" || v == "true" || v == "1" || v == "yes" || v == "on")
        return false; // STA/AP 模式
    // wfb/false/0/no/off，以及读不到配置时，都保守按 WFB 处理
    return true;
}

std::string WifiController::ConfiguredSsid()
{
    // 优先读 [service_AMLgsMenu] 段的 Name
    std::ifstream f(kConnmanConf);
    if (f)
    {
        std::string line;
        bool in_section = false;
        while (std::getline(f, line))
        {
            if (line.rfind("[service_AMLgsMenu]", 0) == 0)
            {
                in_section = true;
                continue;
            }
            if (in_section && line.rfind("[", 0) == 0)
                break;
            if (in_section)
            {
                size_t eq = line.find('=');
                if (eq != std::string::npos && Trim(line.substr(0, eq)) == "Name")
                    return Trim(line.substr(eq + 1));
            }
        }
    }
    // 退回旧的 supplicant 配置
    std::string raw = ReadFileStr(kOpenipcConf);
    size_t p = raw.find("ssid=\"");
    if (p != std::string::npos)
    {
        size_t s = p + 6;
        size_t e = raw.find('"', s);
        if (e != std::string::npos)
            return raw.substr(s, e - s);
    }
    return {};
}

void WifiController::StartScan(const std::string &iface)
{
    if (iface.empty() || g_scanning.exchange(true))
        return;

    std::thread([iface]() {
        std::vector<WifiAp> list;

        // 与 CoreELEC 官方一致：列表来自 ConnMan 的 service 集合。
        // 不自己跑 iw scan —— 已关联状态下 iw scan 会超时并让接口短暂离开信道。
        // connmanctl scan 在 1.43 上返回 NotImplemented（wifi driver 无 scan 字段），
        // 属正常，忽略即可；ConnMan 后台扫描一直在积累结果。
        if (WifiController::IsWfbMode())
        {
            // WFB 模式下网卡被 wifibroadcast 占用，不触发任何扫描动作
            std::lock_guard<std::mutex> lk(g_scan_mu);
            g_scan_result.clear();
            g_scan_fresh.store(true);
            g_scanning.store(false);
            return;
        }

        RunVoid("connmanctl scan wifi");

        for (const auto &path : ListWifiServices())
        {
            ServiceProps sp = QueryService(path);
            if (!sp.found || sp.name.empty())
                continue;
            WifiAp ap;
            ap.ssid = sp.name;
            ap.service_path = path;
            ap.signal_dbm = sp.strength;
            ap.secure = sp.secure;
            ap.state = sp.state;
            // 同一 SSID 只留信号最强的一条
            auto it = std::find_if(list.begin(), list.end(),
                                   [&](const WifiAp &a) { return a.ssid == ap.ssid; });
            if (it == list.end())
                list.push_back(ap);
            else if (ap.signal_dbm > it->signal_dbm)
                *it = ap;
        }

        std::sort(list.begin(), list.end(),
                  [](const WifiAp &a, const WifiAp &b) { return a.signal_dbm > b.signal_dbm; });

        {
            std::lock_guard<std::mutex> lk(g_scan_mu);
            g_scan_result = std::move(list);
        }
        g_scan_fresh.store(true);
        g_scanning.store(false);
    }).detach();
}

bool WifiController::PollScan(std::vector<WifiAp> *out, bool *scanning)
{
    if (scanning)
        *scanning = g_scanning.load();
    if (!g_scan_fresh.exchange(false))
        return false;
    std::lock_guard<std::mutex> lk(g_scan_mu);
    if (out)
        *out = g_scan_result;
    return true;
}

bool WifiController::ConnectSync(const std::string &ssid, const std::string &psk,
                                 const std::string &iface, std::string *message)
{
    auto fail = [&](const std::string &why) {
        if (message)
            *message = why;
        return false;
    };

    if (ssid.empty())
        return fail("SSID 为空");

    // WFB 模式下绝不连 AP（会抢走网卡、破坏 WFB 链路）
    if (IsWfbMode())
        return fail("当前是 WFB 模式，不连接热点（避免抢占网卡）");

    // 目标网卡
    std::string target = iface;
    if (target.empty() || !IsWifiIface(target))
    {
        // 优先 88x2cs/88x2cu（双频），否则第一张非 WFB 卡
        for (const auto &n : AllWifiIfaces())
        {
            if (IsReservedIface(n))
                continue;
            std::string drv = DriverOf(n);
            if (drv == "rtl88x2cs" || drv == "88x2cs" || drv == "rtl88x2cu" || drv == "88x2cu")
            {
                target = n;
                break;
            }
        }
        if (target.empty())
        {
            for (const auto &n : AllWifiIfaces())
            {
                if (IsReservedIface(n))
                    continue;
                target = n;
                break;
            }
        }
    }
    if (target.empty())
        return fail("没有可用的无线网卡");

    // 死接口先拉起来
    if (!IsUp(target))
        RunVoid("ip link set " + target + " up");

    // ---- 第 1 优先：Service.Connect（ConnMan 官方路径，无副作用）----
    std::string svc = FindServiceBySsid(ssid);
    if (!svc.empty())
    {
        ServiceProps sp = QueryService(svc);
        if (sp.state == "ready" || sp.state == "online")
        {
            if (message)
                *message = "已连接 " + ssid;
            return true;
        }

        bool has_psk = sp.secure;
        if (has_psk || psk.empty())
        {
            RunVoid("connmanctl connect " + svc);
            if (WaitForIp(target, 25))
            {
                if (message)
                    *message = "已连接 " + ssid + "  " + Ipv4Of(target);
                return true;
            }
            // 连不上（密码错/AP 拒绝/超时）：不写配置，避免覆盖当前可用的 AP
            return fail("连接失败：检查密码是否正确、AP 是否在线");
        }
    }

    // ---- 第 2 优先：写 service_ 段注册这个 AP（新 AP / 首次带密码）----
    bool blacklist_changed = UnblacklistIface(target);
    if (!WriteServiceConfig(ssid, psk))
        return fail("写入 ConnMan 配置失败");

    RunVoid("systemctl restart connman");
    if (blacklist_changed)
        sleep(8);
    else
        sleep(8);

    // 等它把 service 建出来再连
    svc.clear();
    for (int i = 0; i < 30; ++i)
    {
        svc = FindServiceBySsid(ssid);
        if (!svc.empty())
            break;
        sleep(1);
    }
    if (svc.empty())
        return fail("ConnMan 未发现该热点（AP 不在线？该网卡频段不覆盖？）");

    RunVoid("connmanctl connect " + svc);
    if (WaitForIp(target, 25))
    {
        if (message)
            *message = "已连接 " + ssid + "  " + Ipv4Of(target);
        return true;
    }
    return fail("已发起连接但未获取到 IP");
}

void WifiController::StartConnect(const std::string &ssid, const std::string &psk,
                                  const std::string &iface)
{
    if (ssid.empty() || g_conn_running.exchange(true))
        return;
    g_conn_done.store(false);
    g_conn_ok.store(false);
    std::thread([ssid, psk, iface]() {
        std::string msg;
        bool ok = WifiController::ConnectSync(ssid, psk, iface, &msg);
        {
            std::lock_guard<std::mutex> lk(g_conn_mu);
            g_conn_msg = msg;
        }
        g_conn_ok.store(ok);
        g_conn_done.store(true);
        g_conn_running.store(false);
    }).detach();
}

bool WifiController::PollConnect(bool *ok, std::string *message, bool *running)
{
    if (running)
        *running = g_conn_running.load();
    if (!g_conn_done.exchange(false))
        return false;
    if (ok)
        *ok = g_conn_ok.load();
    if (message)
    {
        std::lock_guard<std::mutex> lk(g_conn_mu);
        *message = g_conn_msg;
    }
    return true;
}

WifiStatus WifiController::QueryStatus(const std::string &iface)
{
    WifiStatus st;
    if (iface.empty() || !IsWifiIface(iface))
        return st;

    std::string link = Run("iw dev " + iface + " link");
    if (link.find("Connected to") == std::string::npos)
        return st;

    st.state = WifiState::Connected;
    std::istringstream iss(link);
    std::string line;
    while (std::getline(iss, line))
    {
        size_t p = line.find("SSID:");
        if (p != std::string::npos)
            st.ssid = Trim(line.substr(p + 5));
        p = line.find("signal:");
        if (p != std::string::npos)
        {
            std::string v = Trim(line.substr(p + 7));
            st.signal_dbm = std::atoi(v.c_str());
        }
    }
    st.ip = Ipv4Of(iface);
    return st;
}

void WifiController::AutoConnectOnBoot(int delay_s)
{
    std::thread([delay_s]() {
        if (delay_s > 0)
            sleep(static_cast<unsigned>(delay_s));

        if (IsWfbMode())
        {
            std::fprintf(stdout, "[wifi] WFB mode, skip boot auto-connect\n");
            return;
        }
        // 已经连上就不动（避免打断正常链路）
        for (const auto &n : AllWifiIfaces())
        {
            std::string ip = Ipv4Of(n);
            if (ip.rfind("192.168.144.", 0) == 0)
            {
                std::fprintf(stdout, "[wifi] already connected on %s (%s)\n",
                             n.c_str(), ip.c_str());
                return;
            }
        }
        const std::string ssid = ConfiguredSsid();
        if (ssid.empty())
        {
            std::fprintf(stdout, "[wifi] no configured SSID, skip boot auto-connect\n");
            return;
        }
        std::string msg;
        const bool ok = ConnectSync(ssid, "", "", &msg);
        std::fprintf(stdout, "[wifi] boot auto-connect %s: %s (%s)\n",
                     ok ? "ok" : "failed", ssid.c_str(), msg.c_str());
    }).detach();
}
void WifiController::Disconnect(const std::string &iface)
{
    if (iface.empty() || !IsWifiIface(iface))
        return;
    std::string ssid = SsidOf(iface);
    if (ssid.empty())
        return;
    std::string svc = FindServiceBySsid(ssid);
    if (!svc.empty())
        RunVoid("connmanctl disconnect " + svc);
}
