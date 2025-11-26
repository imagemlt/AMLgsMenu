#pragma once

#include <string>
#include <cstdint>

class UdpCommandClient {
public:
    UdpCommandClient(const std::string &ip = "127.0.0.1", uint16_t tx_port = 14650, uint16_t rx_port = 14651);
    ~UdpCommandClient();

    // Send a command over UDP. If expect_reply is true, waits for a response up to timeout_ms.
    // Returns true on send success (and reply if requested), false otherwise.
    bool Send(const std::string &cmd, bool expect_reply = false, int timeout_ms = 500);

private:
    int tx_fd_ = -1;
    int rx_fd_ = -1;
    uint16_t tx_port_ = 0;
    uint16_t rx_port_ = 0;
    std::string ip_;
};
