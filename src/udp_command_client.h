#pragma once

#include "command_transport.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class UdpCommandClient : public CommandTransport {
public:
    UdpCommandClient(const std::string &ip = "127.0.0.1", uint16_t tx_port = 14650, uint16_t rx_port = 14651);
    ~UdpCommandClient();

    // Fire-and-forget command; still waits for ACK but discards output.
    bool Send(const std::string &cmd, bool expect_reply = false, int timeout_ms = 500) override;
    // Send and capture textual output (each datagram trimmed per line).
    bool SendWithReply(const std::string &cmd, std::vector<std::string> &response,
                       int timeout_ms = 1000) override;

private:
    bool Execute(const std::string &cmd, std::vector<std::string> *response, int timeout_ms);
    static std::string Trim(const std::string &text);

    int tx_fd_ = -1;
    int rx_fd_ = -1;
    uint16_t tx_port_ = 0;
    uint16_t rx_port_ = 0;
    std::string ip_;
    std::mutex io_mutex_;
};
