#include "udp_command_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <iostream>

UdpCommandClient::UdpCommandClient(const std::string &ip, uint16_t tx_port, uint16_t rx_port)
    : tx_port_(tx_port), rx_port_(rx_port), ip_(ip)
{
    tx_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx_fd_ < 0)
    {
        std::perror("[AMLgsMenu] udp socket (tx)");
    }
    rx_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx_fd_ < 0)
    {
        std::perror("[AMLgsMenu] udp socket (rx)");
    }
    else
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(rx_port_);
        if (bind(rx_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            std::perror("[AMLgsMenu] bind udp rx");
            close(rx_fd_);
            rx_fd_ = -1;
        }
    }
    StartRxThread();
}

UdpCommandClient::~UdpCommandClient()
{
    StopRxThread();
    if (tx_fd_ >= 0)
        close(tx_fd_);
    if (rx_fd_ >= 0)
        close(rx_fd_);
}

bool UdpCommandClient::Send(const std::string &cmd, bool expect_reply, int timeout_ms)
{
    if (expect_reply)
    {
        std::vector<std::string> discard;
        return Execute(cmd, &discard, timeout_ms);
    }
    return Execute(cmd, nullptr, timeout_ms);
}

bool UdpCommandClient::SendWithReply(const std::string &cmd, std::vector<std::string> &response, int timeout_ms)
{
    response.clear();
    return Execute(cmd, &response, timeout_ms);
}

bool UdpCommandClient::SendWithReplyAsync(
    const std::string &cmd,
    const std::vector<std::string> &keys,
    std::function<void(const std::unordered_map<std::string, std::string> &, bool)> cb,
    int timeout_ms)
{
    if (tx_fd_ < 0)
    {
        std::cout << "[AMLgsMenu] tx_fd_ < 0" << std::endl;
        return false;
    }
    if (rx_fd_ < 0)
    {
        std::cout << "[AMLgsMenu] rx_fd_ < 0" << std::endl;
        return false;
    }
    if (cmd.empty() || keys.empty())
    {
        return false;
    }

    std::fprintf(stdout, "[AMLgsMenu] udp exec: %s\n", cmd.c_str());

    PendingBatch batch{};
    batch.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    batch.callback = std::move(cb);
    for (const auto &key : keys)
    {
        if (!key.empty())
            batch.keys.insert(key);
    }
    if (batch.keys.empty())
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (!pending_batches_.empty())
        {
            return false;
        }
        pending_batches_.push_back(std::move(batch));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tx_port_);
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1)
    {
        std::fprintf(stderr, "[AMLgsMenu] invalid UDP target IP: %s\n", ip_.c_str());
        return false;
    }
    ssize_t n = sendto(tx_fd_, cmd.data(), cmd.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (n < 0)
    {
        std::perror("[AMLgsMenu] sendto");
        return false;
    }
    return true;
}

std::string UdpCommandClient::Trim(const std::string &text)
{
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
    {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool UdpCommandClient::Execute(const std::string &cmd, std::vector<std::string> *response, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(io_mutex_);
    if (tx_fd_ < 0)
    {
        std::cout << "[AMLgsMenu] tx_fd_ < 0" << std::endl;
        return false;
    }
    std::fprintf(stdout, "[AMLgsMenu] udp exec: %s\n", cmd.c_str());
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tx_port_);
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1)
    {
        std::fprintf(stderr, "[AMLgsMenu] invalid UDP target IP: %s\n", ip_.c_str());
        return false;
    }
    ssize_t n = sendto(tx_fd_, cmd.data(), cmd.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (n < 0)
    {
        std::perror("[AMLgsMenu] sendto");
        return false;
    }
    if (rx_fd_ < 0)
    {
        std::perror("[AMLgsMenu] sendto rxfd < 0");
        return true;
    }

    if (rx_thread_.joinable())
    {
        if (response)
        {
            response->clear();
            return false;
        }
        return true;
    }

    if (response)
    {
        response->clear();
    }

    bool ack_received = false;
    bool done = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[1024];

    while (true)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            break;
        }
        pollfd pfd{};
        pfd.fd = rx_fd_;
        pfd.events = POLLIN;
        int wait_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::perror("[AMLgsMenu] poll");
            return false;
        }
        if (pr == 0)
        {
            break;
        }
        if (!(pfd.revents & POLLIN))
        {
            continue;
        }
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        ssize_t rn = recvfrom(rx_fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromlen);
        if (rn < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::perror("[AMLgsMenu] recvfrom");
            return false;
        }
        std::string packet(buf, buf + rn);
        std::string trimmed = Trim(packet);
        if (trimmed == "OK")
        {
            if (!ack_received)
            {
                ack_received = true;
                continue;
            }
            done = true;
            break;
        }
        if (response && !trimmed.empty())
        {
            response->push_back(trimmed);
        }
    }

    if (!ack_received)
    {
        std::cout << "[AMLgsMenu] no ACK received for command: " << cmd << std::endl;
        return false;
    }

    if (!done)
    {
        // flush any pending final OK without blocking
        while (true)
        {
            pollfd pfd{};
            pfd.fd = rx_fd_;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 0);
            if (pr <= 0 || !(pfd.revents & POLLIN))
            {
                break;
            }
            sockaddr_in from{};
            socklen_t fromlen = sizeof(from);
            ssize_t rn = recvfrom(rx_fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromlen);
            if (rn <= 0)
            {
                break;
            }
            std::string trimmed = Trim(std::string(buf, buf + rn));
            if (trimmed == "OK")
            {
                done = true;
                break;
            }
        }
    }

    return true;
}

void UdpCommandClient::StartRxThread()
{
    if (rx_fd_ < 0)
        return;
    rx_stop_.store(false);
    rx_thread_ = std::thread([this]() { RxThreadMain(); });
}

void UdpCommandClient::StopRxThread()
{
    rx_stop_.store(true);
    if (rx_thread_.joinable())
        rx_thread_.join();
}

void UdpCommandClient::HandleRxLine(
    const std::string &line,
    std::function<void(const std::unordered_map<std::string, std::string> &, bool)> &cb,
    std::unordered_map<std::string, std::string> &cb_values,
    bool &cb_ok)
{
    if (line.empty() || line == "OK")
        return;

    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_batches_.empty())
        return;

    PendingBatch &batch = pending_batches_.front();
    if (line.rfind("__key__", 0) == 0)
    {
        batch.current_key = line.substr(7);
        return;
    }

    if (batch.current_key.empty())
        return;
    if (batch.values.find(batch.current_key) == batch.values.end())
    {
        batch.values[batch.current_key] = line;
    }

    bool complete = true;
    for (const auto &key : batch.keys)
    {
        if (batch.values.find(key) == batch.values.end())
        {
            complete = false;
            break;
        }
    }
    if (complete)
    {
        cb = batch.callback;
        cb_values = batch.values;
        cb_ok = true;
        pending_batches_.pop_front();
    }
}

void UdpCommandClient::CheckPendingTimeouts(
    std::function<void(const std::unordered_map<std::string, std::string> &, bool)> &cb,
    std::unordered_map<std::string, std::string> &cb_values,
    bool &cb_ok)
{
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_batches_.empty())
        return;
    auto now = std::chrono::steady_clock::now();
    PendingBatch &batch = pending_batches_.front();
    if (now < batch.deadline)
        return;
    cb = batch.callback;
    cb_values = {};
    cb_ok = false;
    pending_batches_.pop_front();
}

void UdpCommandClient::RxThreadMain()
{
    char buf[1024];
    while (!rx_stop_.load())
    {
        pollfd pfd{};
        pfd.fd = rx_fd_;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 200);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            std::perror("[AMLgsMenu] udp rx poll");
            continue;
        }
        std::function<void(const std::unordered_map<std::string, std::string> &, bool)> cb;
        std::unordered_map<std::string, std::string> cb_values;
        bool cb_ok = false;

        if (pr > 0 && (pfd.revents & POLLIN))
        {
            sockaddr_in from{};
            socklen_t fromlen = sizeof(from);
            ssize_t rn = recvfrom(rx_fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromlen);
            if (rn > 0)
            {
                std::string trimmed = Trim(std::string(buf, buf + rn));
                if (!trimmed.empty())
                {
                    std::fprintf(stdout, "[AMLgsMenu] udp resp: %s\n", trimmed.c_str());
                }
                HandleRxLine(trimmed, cb, cb_values, cb_ok);
            }
        }

        if (!cb)
        {
            CheckPendingTimeouts(cb, cb_values, cb_ok);
        }

        if (cb)
        {
            cb(cb_values, cb_ok);
        }
    }
}
