#include "udp_command_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

UdpCommandClient::UdpCommandClient(const std::string &ip, uint16_t tx_port, uint16_t rx_port)
    : tx_port_(tx_port), rx_port_(rx_port), ip_(ip) {
    tx_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx_fd_ < 0) {
        std::perror("[AMLgsMenu] udp socket (tx)");
    }
    rx_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx_fd_ < 0) {
        std::perror("[AMLgsMenu] udp socket (rx)");
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(rx_port_);
        if (bind(rx_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            std::perror("[AMLgsMenu] bind udp rx");
            close(rx_fd_);
            rx_fd_ = -1;
        }
    }
}

UdpCommandClient::~UdpCommandClient() {
    if (tx_fd_ >= 0) close(tx_fd_);
    if (rx_fd_ >= 0) close(rx_fd_);
}

bool UdpCommandClient::Send(const std::string &cmd, bool expect_reply, int timeout_ms) {
    if (tx_fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tx_port_);
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "[AMLgsMenu] invalid UDP target IP: %s\n", ip_.c_str());
        return false;
    }
    ssize_t n = sendto(tx_fd_, cmd.data(), cmd.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (n < 0) {
        std::perror("[AMLgsMenu] sendto");
        return false;
    }
    if (!expect_reply || rx_fd_ < 0) {
        return true;
    }
    pollfd pfd{};
    pfd.fd = rx_fd_;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        if (pr < 0) std::perror("[AMLgsMenu] poll");
        return false;
    }
    if (pfd.revents & POLLIN) {
        char buf[1024];
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        ssize_t rn = recvfrom(rx_fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromlen);
        if (rn < 0) {
            std::perror("[AMLgsMenu] recvfrom");
            return false;
        }
        // optional: print first line of reply
        std::string resp(buf, buf + rn);
        std::fprintf(stdout, "[AMLgsMenu] UDP cmd reply: %s\n", resp.c_str());
    }
    return true;
}
