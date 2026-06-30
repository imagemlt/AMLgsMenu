#pragma once

#include "command_transport.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class ApTcpClient : public CommandTransport
{
public:
    ApTcpClient(const std::string &ip, uint16_t port = 12355);
    ~ApTcpClient() override = default;

    bool Send(const std::string &cmd, bool expect_reply = false, int timeout_ms = 500) override;
    bool SendWithReply(const std::string &cmd, std::vector<std::string> &response,
                       int timeout_ms = 1000) override;

    const std::string &ip() const { return ip_; }
    uint16_t port() const { return port_; }

private:
    bool Execute(const std::string &cmd, std::vector<std::string> *response, int timeout_ms);

    std::string ip_;
    uint16_t port_;
    std::mutex mutex_;
};
