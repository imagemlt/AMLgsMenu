#pragma once

#include <string>
#include <vector>

class CommandTransport {
public:
    virtual ~CommandTransport() = default;
    virtual bool Send(const std::string &cmd, bool expect_reply = false, int timeout_ms = 500) = 0;
    virtual bool SendWithReply(const std::string &cmd, std::vector<std::string> &response,
                               int timeout_ms = 1000) = 0;
};
