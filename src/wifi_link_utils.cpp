#include "wifi_link_utils.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
std::vector<std::string> ListWifiInterfaces()
{
    std::vector<std::string> interfaces;
    FILE *pipe = popen("iw dev 2>/dev/null", "r");
    if (!pipe)
        return interfaces;

    char line[256];
    while (fgets(line, sizeof(line), pipe))
    {
        const std::string text(line);
        const auto pos = text.find("Interface ");
        if (pos == std::string::npos)
            continue;
        const auto begin = pos + 10;
        const auto end = text.find_first_of(" \t\r\n", begin);
        if (end != begin)
            interfaces.push_back(text.substr(begin, end - begin));
    }
    pclose(pipe);
    return interfaces;
}

bool IsConnected(const std::string &iface)
{
    const std::string command = "iw dev " + iface + " link 2>/dev/null";
    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe)
        return false;

    bool connected = false;
    char line[256];
    while (fgets(line, sizeof(line), pipe))
    {
        if (std::string(line).rfind("Connected to ", 0) == 0)
        {
            connected = true;
            break;
        }
    }
    pclose(pipe);
    return connected;
}
} // namespace

std::string DetectConnectedWifiInterface(const std::string &preferred_iface)
{
    const auto interfaces = ListWifiInterfaces();
    if (!preferred_iface.empty())
    {
        for (const auto &iface : interfaces)
        {
            if (iface == preferred_iface && IsConnected(iface))
                return iface;
        }
    }

    for (const auto &iface : interfaces)
    {
        if (iface != preferred_iface && IsConnected(iface))
            return iface;
    }
    return {};
}
