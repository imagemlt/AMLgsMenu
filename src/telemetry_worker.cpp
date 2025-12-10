#include "telemetry_worker.h"

#include "video_mode.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>
#include <set>
#include <unordered_map>
#include <cstdio>
#include <cmath>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <poll.h>

namespace {
constexpr std::chrono::seconds kLoopSleep{1};
constexpr std::chrono::seconds kSignalInterval{2};
constexpr std::chrono::seconds kTempInterval{1};
constexpr std::chrono::seconds kFpsInterval{1};
constexpr std::chrono::seconds kHidBatteryInterval{2};
constexpr uint16_t kCemianVendorId = 0x2019;
constexpr uint16_t kCemianProductId = 0x056D;
constexpr size_t kCemianBatteryIndex = 6;
constexpr size_t kCemianReportLength = 8;

std::optional<uint16_t> ReadSysfsHex(const std::filesystem::path &path)
{
    std::ifstream file(path);
    if (!file)
        return std::nullopt;
    std::string token;
    file >> token;
    if (token.empty())
        return std::nullopt;
    try
    {
        return static_cast<uint16_t>(std::stoul(token, nullptr, 16));
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}

bool MatchVendorProduct(const std::string &hid_path, uint16_t vid, uint16_t pid)
{
    namespace fs = std::filesystem;
    const fs::path node = fs::path(hid_path).filename();
    fs::path base("/sys/class/hidraw");
    base /= node;
    base /= "device";
    auto sys_vid = ReadSysfsHex(base / "idVendor");
    auto sys_pid = ReadSysfsHex(base / "idProduct");
    if (!sys_vid || !sys_pid)
        return false;
    return *sys_vid == vid && *sys_pid == pid;
}

std::string Trim(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    if (start > 0)
        s.erase(0, start);
    return s;
}

std::optional<std::string> ReadSingleLine(const std::filesystem::path &path)
{
    std::ifstream file(path);
    if (!file)
        return std::nullopt;
    std::string line;
    std::getline(file, line);
    return Trim(std::move(line));
}

bool LooksLikeHidSupply(const std::string &name)
{
    static const char *kHints[] = {"hid", "gamepad", "controller", "joystick", "pad", "mouse"};
    for (const char *hint : kHints)
    {
        if (name.find(hint) != std::string::npos)
            return true;
    }
    return false;
}

std::optional<float> QueryHidBatteryPercent()
{
    namespace fs = std::filesystem;
    const fs::path base("/sys/class/power_supply");
    std::error_code ec;
    if (!fs::exists(base, ec) || !fs::is_directory(base, ec))
        return std::nullopt;

    std::optional<float> fallback;
    for (const auto &entry : fs::directory_iterator(base, ec))
    {
        if (!entry.is_directory())
            continue;
        const std::string name = entry.path().filename().string();
        auto type = ReadSingleLine(entry.path() / "type");
        if (!type || *type != "Battery")
            continue;

        auto scope = ReadSingleLine(entry.path() / "scope");
        if (scope && scope->find("System") != std::string::npos)
            continue;

        auto present = ReadSingleLine(entry.path() / "present");
        if (present && *present == "0")
            continue;

        auto capacity = ReadSingleLine(entry.path() / "capacity");
        if (!capacity)
            continue;

        float pct = 0.0f;
        try
        {
            pct = std::stof(*capacity);
        }
        catch (const std::exception &)
        {
            continue;
        }
        pct = std::clamp(pct, 0.0f, 100.0f);

        if (LooksLikeHidSupply(name))
            return pct;
        if (!fallback)
            fallback = pct;
    }
    return fallback;
}

struct HidBatteryField
{
    uint8_t report_id = 0;
    uint32_t bit_offset = 0;
    uint8_t bit_size = 0;
    bool is_feature = false;
    uint32_t report_bits = 0;
};

class HidBatteryMonitor
{
public:
    HidBatteryMonitor() = default;
    ~HidBatteryMonitor()
    {
        for (auto &dev : devices_)
        {
            if (dev.fd >= 0)
                close(dev.fd);
        }
    }

    float Poll()
    {
        const auto now = std::chrono::steady_clock::now();
        if (last_scan_.time_since_epoch().count() == 0 ||
            (now - last_scan_) >= std::chrono::seconds(5))
        {
            Rescan();
            last_scan_ = now;
        }

        float best = -1.0f;
        for (auto it = devices_.begin(); it != devices_.end();)
        {
            float value = ReadDevice(*it);
            if (it->fd < 0)
            {
                it = devices_.erase(it);
                continue;
            }
            if (value >= 0.0f)
                best = value;
            ++it;
        }
        return best;
    }

private:
    struct Device
    {
        int fd = -1;
        std::string path;
        HidBatteryField field;
        size_t payload_bytes = 0;
        bool logged_once = false;
        float last_value = -1.0f;
        bool manual = false;
        size_t manual_index = 0;
        size_t manual_report_len = 0;
    };

    void Rescan()
    {
        namespace fs = std::filesystem;
        std::set<std::string> present;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator("/dev", ec))
        {
            if (!entry.is_character_file(ec))
                continue;
            const std::string name = entry.path().filename().string();
            if (name.rfind("hidraw", 0) != 0)
                continue;
            std::string path = entry.path().string();
            std::fprintf(stderr, "[Telemetry] found candidate %s\n", path.c_str());
            present.insert(path);
            bool known = false;
            for (const auto &dev : devices_)
            {
                if (dev.path == path)
                {
                    known = true;
                    break;
                }
            }
            if (!known)
                InitDevice(path);
        }

        devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                      [&](Device &dev) {
                                          if (present.count(dev.path) == 0)
                                          {
                                              if (dev.fd >= 0)
                                                  close(dev.fd);
                                              dev.fd = -1;
                                              return true;
                                          }
                                          return false;
                                      }),
                       devices_.end());
    }

    static bool IsBatteryUsage(uint32_t usage)
    {
        uint16_t page = static_cast<uint16_t>(usage >> 16);
        uint16_t code = static_cast<uint16_t>(usage & 0xFFFF);
        if (page == 0x06 && code == 0x20)
        {
            std::fprintf(stderr, "[Telemetry] matched battery usage GD 0x%04x\n", code);
            return true;
        }
        if (page == 0x84 && (code == 0x68 || code == 0x20))
        {
            std::fprintf(stderr, "[Telemetry] matched battery usage PD 0x%04x\n", code);
            return true;
        }
        return false;
    }

    static std::optional<HidBatteryField> ParseField(int fd)
    {
        int desc_size = 0;
        if (ioctl(fd, HIDIOCGRDESCSIZE, &desc_size) < 0 || desc_size <= 0)
            return std::nullopt;
        hidraw_report_descriptor desc{};
        desc.size = std::min(desc_size, HID_MAX_DESCRIPTOR_SIZE);
        if (ioctl(fd, HIDIOCGRDESC, &desc) < 0)
            return std::nullopt;
        std::fprintf(stderr, "[Telemetry] descriptor size %d (clamped %u)\n", desc_size, desc.size);

        std::unordered_map<uint8_t, std::pair<uint32_t, uint32_t>> report_bits;
        uint16_t usage_page = 0;
        uint32_t report_size = 0;
        uint32_t report_count = 0;
        uint8_t report_id = 0;
        std::vector<uint32_t> usages;
        std::optional<uint32_t> usage_min;
        std::optional<uint32_t> usage_max;
        bool pending_battery_usage = false;

        size_t i = 0;
        while (i < desc.size)
        {
            uint8_t b = desc.value[i++];
            uint8_t item_size = b & 0x3;
            if (item_size == 3)
                item_size = 4;
            if (i + item_size > desc.size)
                break;
            uint8_t type = (b >> 2) & 0x3;
            uint8_t tag = (b >> 4) & 0xF;
            uint32_t value = 0;
            for (uint8_t n = 0; n < item_size; ++n)
            {
                value |= static_cast<uint32_t>(desc.value[i++]) << (8 * n);
            }

            if (type == 1) // Global
            {
                switch (tag)
                {
                case 0: usage_page = static_cast<uint16_t>(value); break;
                case 7: report_size = value; break;
                case 8: report_count = value; break;
                case 10: report_id = static_cast<uint8_t>(value); break;
                default: break;
                }
            }
            else if (type == 2) // Local
            {
                switch (tag)
                {
                case 0: // Usage
                {
                    uint32_t usage = (static_cast<uint32_t>(usage_page) << 16) | (value & 0xFFFF);
                    if (usage_page == 0x84)
                        std::fprintf(stderr, "[Telemetry] saw usage page84 code=0x%04x\n", value & 0xFFFF);
                    if (IsBatteryUsage(usage))
                        pending_battery_usage = true;
                    usages.push_back(usage);
                    break;
                }
                case 1:
                    usage_min = value & 0xFFFF;
                    break;
                case 2:
                    usage_max = value & 0xFFFF;
                    if (usage_min && usage_max && usage_max >= usage_min)
                    {
                        for (uint32_t u = *usage_min; u <= *usage_max && usages.size() < 32; ++u)
                        {
                            usages.push_back((static_cast<uint32_t>(usage_page) << 16) | u);
                        }
                    }
                    usage_min.reset();
                    usage_max.reset();
                    break;
                default:
                    break;
                }
            }
            else if (type == 0) // Main
            {
                bool is_input = (tag == 8);
                bool is_feature = (tag == 11);
                if ((is_input || is_feature) && report_size > 0 && report_count > 0)
                {
                    auto &state = report_bits[report_id];
                    uint32_t offset = is_feature ? state.second : state.first;
                    uint32_t total_bits = report_size * report_count;
                    std::fprintf(stderr,
                                 "[Telemetry] main item report_id=%u type=%s size=%u count=%u usages=%zu pending=%d offset=%u\n",
                                 report_id, is_feature ? "feature" : "input", report_size, report_count,
                                 usages.size(), pending_battery_usage ? 1 : 0, offset);
                    if (usages.empty() && usage_min && usage_max && usage_max >= usage_min)
                    {
                        for (uint32_t u = *usage_min; u <= *usage_max && usages.size() < 32; ++u)
                        {
                            usages.push_back((static_cast<uint32_t>(usage_page) << 16) | u);
                        }
                    }
                    for (size_t idx = 0; idx < report_count; ++idx)
                    {
                        bool usage_is_batt = false;
                        if (idx < usages.size())
                        {
                            std::fprintf(stderr, "[Telemetry] checking usage 0x%08x (page=0x%04x code=0x%04x)\n",
                                         usages[idx], static_cast<uint16_t>(usages[idx] >> 16),
                                         static_cast<uint16_t>(usages[idx] & 0xFFFF));
                            usage_is_batt = IsBatteryUsage(usages[idx]);
                            if (usage_is_batt)
                                std::fprintf(stderr, "[Telemetry] usage slot %zu matches battery\n", idx);
                        }
                        else if (pending_battery_usage && idx == 0)
                        {
                            std::fprintf(stderr, "[Telemetry] assuming pending battery usage in mixed block\n");
                            usage_is_batt = true;
                        }

                        if (usage_is_batt)
                        {
                            HidBatteryField field{};
                            field.report_id = report_id;
                            field.bit_offset = offset + static_cast<uint32_t>(idx) * report_size;
                            field.bit_size = std::min<uint32_t>(report_size, 16);
                            field.is_feature = is_feature;
                            field.report_bits = offset + total_bits;
                            std::fprintf(stderr, "[Telemetry] battery field resolved report_id=%u offset=%u size=%u\n",
                                         field.report_id, field.bit_offset, field.bit_size);
                            return field;
                        }
                    }
                    if (pending_battery_usage)
                    {
                        HidBatteryField field{};
                        field.report_id = report_id;
                        field.bit_offset = offset;
                        field.bit_size = report_size ? std::min<uint32_t>(report_size, 16) : 8;
                        field.is_feature = is_feature;
                        field.report_bits = offset + total_bits;
                        std::fprintf(stderr, "[Telemetry] fallback battery field report_id=%u offset=%u size=%u\n",
                                     field.report_id, field.bit_offset, field.bit_size);
                        return field;
                    }
                    pending_battery_usage = false;
                    if (is_feature)
                        state.second += total_bits;
                    else
                        state.first += total_bits;
                    usages.clear();
                    usage_min.reset();
                    usage_max.reset();
                }
                else if (tag == 10)
                {
                    usages.clear();
                    usage_min.reset();
                    usage_max.reset();
                }
            }
        }
        return std::nullopt;
    }

    static float ExtractValue(const uint8_t *data, size_t len, const HidBatteryField &field)
    {
        if (field.bit_size == 0)
            return -1.0f;
        const size_t needed_bits = field.bit_offset + field.bit_size;
        if (needed_bits > len * 8)
            return -1.0f;
        uint32_t raw = 0;
        for (uint32_t bit = 0; bit < field.bit_size; ++bit)
        {
            uint32_t idx = field.bit_offset + bit;
            uint8_t byte = data[idx / 8];
            raw |= ((byte >> (idx % 8)) & 0x1u) << bit;
        }
        uint32_t max_value = (field.bit_size >= 31) ? 0xFFFFFFFFu : ((1u << field.bit_size) - 1u);
        if (max_value == 0)
            return -1.0f;
        float pct = (static_cast<float>(raw) * 100.0f) / static_cast<float>(max_value);
        return std::clamp(pct, 0.0f, 100.0f);
    }

    bool InitDevice(const std::string &path)
    {
        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            return false;
        auto field = ParseField(fd);
        Device dev;
        dev.fd = fd;
        dev.path = path;
        if (!field)
        {
            if (ConfigureManualDevice(dev))
            {
                devices_.push_back(std::move(dev));
                return true;
            }
            std::fprintf(stderr, "[Telemetry] %s has no battery usage\n", path.c_str());
            close(fd);
            return false;
        }
        dev.field = *field;
        dev.payload_bytes = (field->report_bits + 7) / 8;
        std::fprintf(stderr, "[Telemetry] HID device %s report_id=%u bits=%u feature=%d\n",
                     path.c_str(), dev.field.report_id, dev.field.report_bits, dev.field.is_feature ? 1 : 0);
        devices_.push_back(std::move(dev));
        return true;
    }

    bool ConfigureManualDevice(Device &dev)
    {
        struct hidraw_devinfo info
        {
            0
        };
        if (ioctl(dev.fd, HIDIOCGRAWINFO, &info) < 0)
            return false;
        if (static_cast<uint16_t>(info.vendor) != kCemianVendorId ||
            static_cast<uint16_t>(info.product) != kCemianProductId)
            return false;
        dev.manual = true;
        dev.manual_index = kCemianBatteryIndex;
        dev.manual_report_len = kCemianReportLength;
        std::fprintf(stderr, "[Telemetry] %s using manual HID fallback\n", dev.path.c_str());
        return true;
    }

    float ReadDevice(Device &dev)
    {
        if (dev.fd < 0)
            return -1.0f;

        if (dev.manual)
            return ReadManualDevice(dev);

        if (dev.payload_bytes == 0)
            return -1.0f;
        const size_t total_len = dev.payload_bytes + (dev.field.report_id ? 1 : 0);
        std::vector<uint8_t> buffer(total_len ? total_len : 1, 0);
        auto fetch_feature = [&](std::vector<uint8_t> &buf) -> bool {
            if (dev.field.report_id && !buf.empty())
                buf[0] = dev.field.report_id;
            const int req_len = static_cast<int>(buf.size());
            if (ioctl(dev.fd, HIDIOCGFEATURE(req_len), buf.data()) < 0)
            {
                if (errno == ENODEV || errno == EIO)
                {
                    close(dev.fd);
                    dev.fd = -1;
                }
                std::fprintf(stderr, "[Telemetry] HID %s feature ioctl failed errno=%d\n",
                             dev.path.c_str(), errno);
                return false;
            }
            if (!buf.empty())
                std::fprintf(stderr, "[Telemetry] HID %s feature len=%d first=0x%02x\n",
                             dev.path.c_str(), req_len, buf[0]);
            else
                std::fprintf(stderr, "[Telemetry] HID %s feature len=%d\n", dev.path.c_str(), req_len);
            return true;
        };

        if (dev.field.is_feature)
        {
            if (!fetch_feature(buffer))
                return -1.0f;
        }
        else
        {
            auto attempt_read = [&]() -> ssize_t {
                return read(dev.fd, buffer.data(), buffer.size());
            };

            ssize_t rd = attempt_read();
            if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                pollfd pfd{};
                pfd.fd = dev.fd;
                pfd.events = POLLIN;
                int pr = poll(&pfd, 1, 100);
                if (pr > 0)
                    rd = attempt_read();
            }

            if (rd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    std::fprintf(stderr, "[Telemetry] HID %s read timeout -> feature fallback\n",
                                 dev.path.c_str());
                    if (!fetch_feature(buffer))
                        return -1.0f;
                }
                else
                {
                    std::fprintf(stderr, "[Telemetry] HID %s read failed errno=%d\n",
                                 dev.path.c_str(), errno);
                    close(dev.fd);
                    dev.fd = -1;
                    return -1.0f;
                }
            }
            else if (dev.field.report_id && buffer[0] != dev.field.report_id)
            {
                std::fprintf(stderr,
                             "[Telemetry] HID %s read report_id mismatch got=0x%02x expect=0x%02x\n",
                             dev.path.c_str(), buffer.empty() ? 0 : buffer[0], dev.field.report_id);
                return -1.0f;
            }
            else
            {
                std::fprintf(stderr, "[Telemetry] HID %s read %zd bytes first=0x%02x\n",
                             dev.path.c_str(), rd, buffer.empty() ? 0 : buffer[0]);
            }
        }
        const uint8_t *payload = buffer.data() + (dev.field.report_id ? 1 : 0);
        float pct = ExtractValue(payload, dev.payload_bytes, dev.field);
        if (pct >= 0.0f)
        {
            std::fprintf(stderr, "[Telemetry] HID %s battery %.1f%%\n", dev.path.c_str(), pct);
            dev.logged_once = true;
            dev.last_value = pct;
        }
        return pct;
    }

    float ReadManualDevice(Device &dev)
    {
        if (dev.manual_report_len == 0)
            return -1.0f;

        std::vector<uint8_t> buffer(dev.manual_report_len, 0);
        auto attempt_read = [&]() -> ssize_t {
            return read(dev.fd, buffer.data(), buffer.size());
        };

        ssize_t rd = attempt_read();
        if (rd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            pollfd pfd{};
            pfd.fd = dev.fd;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 100);
            if (pr > 0)
                rd = attempt_read();
        }

        if (rd < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                std::fprintf(stderr, "[Telemetry] HID %s manual read failed errno=%d\n",
                             dev.path.c_str(), errno);
                close(dev.fd);
                dev.fd = -1;
            }
            return -1.0f;
        }

        if (rd <= static_cast<ssize_t>(dev.manual_index))
        {
            std::fprintf(stderr, "[Telemetry] HID %s manual short read %zd\n",
                         dev.path.c_str(), rd);
            return -1.0f;
        }

        uint8_t raw = buffer[dev.manual_index];
        float pct = std::clamp(static_cast<float>(raw), 0.0f, 100.0f);
        std::fprintf(stderr, "[Telemetry] HID %s battery %.1f%% (manual)\n",
                     dev.path.c_str(), pct);
        dev.logged_once = true;
        dev.last_value = pct;
        return pct;
    }

    std::vector<Device> devices_;
    std::chrono::steady_clock::time_point last_scan_{};
};
}

TelemetryWorker::TelemetryWorker(SignalMonitor *signal_monitor)
    : signal_monitor_(signal_monitor) {}

TelemetryWorker::~TelemetryWorker() {
    Stop();
}

void TelemetryWorker::Start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&TelemetryWorker::ThreadMain, this);
}

void TelemetryWorker::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

TelemetryWorker::Snapshot TelemetryWorker::Latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void TelemetryWorker::ThreadMain() {
    HidBatteryMonitor hid_monitor;
    auto last_signal = std::chrono::steady_clock::time_point{};
    auto last_temp = std::chrono::steady_clock::time_point{};
    auto last_fps = std::chrono::steady_clock::time_point{};
    auto last_hid_batt = std::chrono::steady_clock::time_point{};

    std::fprintf(stderr, "[Telemetry] worker thread start\n");

    while (running_) {
        const auto now = std::chrono::steady_clock::now();

        if (signal_monitor_) {
            if (last_signal.time_since_epoch().count() == 0 ||
                (now - last_signal) >= kSignalInterval) {
                signal_monitor_->Poll();
                last_signal = now;
            }
        }

        Snapshot snap;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap = latest_;
        }

        if (signal_monitor_) {
            snap.ground_signal = signal_monitor_->Latest();
            snap.packet_rate = signal_monitor_->LatestRate();
        }
        bool updated = false;
        if (last_temp.time_since_epoch().count() == 0 ||
            (now - last_temp) >= kTempInterval) {
            snap.ground_temp_c = ReadTemperatureC();
            snap.has_ground_temp = true;
            last_temp = now;
            updated = true;
        }

        if (last_fps.time_since_epoch().count() == 0 ||
            (now - last_fps) >= kFpsInterval) {
            snap.output_fps = GetOutputFps();
            last_fps = now;
            updated = true;
        }

        if (last_hid_batt.time_since_epoch().count() == 0 ||
            (now - last_hid_batt) >= kHidBatteryInterval) {
            const float hid_value = hid_monitor.Poll();
            if (hid_value >= 0.0f) {
                snap.hid_batt_percent = hid_value;
                snap.has_hid_batt = true;
            } else {
                auto hid = QueryHidBatteryPercent();
                if (hid.has_value()) {
                    snap.hid_batt_percent = hid.value();
                    snap.has_hid_batt = true;
                } else {
                    snap.has_hid_batt = false;
                }
            }
            last_hid_batt = now;
            updated = true;
        }

        if (signal_monitor_) {
            updated = true;
        }

        if (updated) {
            snap.timestamp = now;
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = snap;
        }

        if (!running_) {
            break;
        }
        std::this_thread::sleep_for(kLoopSleep);
    }
    std::fprintf(stderr, "[Telemetry] worker thread stop\n");
}
