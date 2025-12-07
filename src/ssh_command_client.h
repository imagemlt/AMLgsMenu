#pragma once

#include "command_transport.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class SshCommandClient : public CommandTransport {
public:
    SshCommandClient(std::string host = "10.5.0.10", uint16_t port = 22,
                     std::string user = "root", std::string password = "12345");
    ~SshCommandClient() override = default;

    bool Send(const std::string &cmd, bool expect_reply = false, int timeout_ms = 500) override;
    bool SendWithReply(const std::string &cmd, std::vector<std::string> &response,
                       int timeout_ms = 1000) override;

private:
    bool Execute(const std::string &cmd, std::vector<std::string> *response, int timeout_ms);
    static void SplitLines(const std::string &text, std::vector<std::string> &out);

    std::string host_;
    uint16_t port_;
    std::string user_;
    std::string password_;
    std::mutex mutex_;
};
