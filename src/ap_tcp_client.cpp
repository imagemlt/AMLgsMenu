#include "ap_tcp_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

ApTcpClient::ApTcpClient(const std::string &ip, uint16_t port)
    : ip_(ip), port_(port)
{
}

bool ApTcpClient::Send(const std::string &cmd, bool expect_reply, int timeout_ms)
{
    if (expect_reply)
    {
        std::vector<std::string> discard;
        return Execute(cmd, &discard, timeout_ms);
    }
    return Execute(cmd, nullptr, timeout_ms);
}

bool ApTcpClient::SendWithReply(const std::string &cmd, std::vector<std::string> &response,
                                int timeout_ms)
{
    response.clear();
    return Execute(cmd, &response, timeout_ms);
}

bool ApTcpClient::Execute(const std::string &cmd, std::vector<std::string> *response, int timeout_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::fprintf(stdout, "[AMLgsMenu] ap_tcp exec: %s\n", cmd.c_str());
    std::fflush(stdout);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::fprintf(stderr, "[AMLgsMenu] ap_tcp socket failed: %s\n", std::strerror(errno));
        return false;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        std::fprintf(stderr, "[AMLgsMenu] ap_tcp setsockopt SO_SNDTIMEO: %s\n", std::strerror(errno));
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        std::fprintf(stderr, "[AMLgsMenu] ap_tcp setsockopt SO_RCVTIMEO: %s\n", std::strerror(errno));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1)
    {
        std::fprintf(stderr, "[AMLgsMenu] ap_tcp invalid IP: %s\n", ip_.c_str());
        close(fd);
        return false;
    }

    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        std::fprintf(stderr, "[AMLgsMenu] ap_tcp connect to %s:%u failed: %s\n",
                     ip_.c_str(), port_, std::strerror(errno));
        close(fd);
        return false;
    }

    std::string request = cmd + "\n";
    size_t total = 0;
    while (total < request.size())
    {
        ssize_t sent = send(fd, request.data() + total, request.size() - total, 0);
        if (sent <= 0)
        {
            if (sent == 0)
            {
                std::fprintf(stderr, "[AMLgsMenu] ap_tcp send returned 0, connection may be closed\n");
            }
            else if (errno == EINTR)
            {
                continue;
            }
            else
            {
                std::fprintf(stderr, "[AMLgsMenu] ap_tcp send failed: %s\n", std::strerror(errno));
            }
            close(fd);
            return false;
        }
        total += static_cast<size_t>(sent);
    }

    std::string collected;
    char buf[1024];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int wait_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (wait_ms <= 0)
            break;
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            break;
        if (!(pfd.revents & POLLIN))
            break;
        ssize_t rn = recv(fd, buf, sizeof(buf), 0);
        if (rn <= 0)
            break;
        collected.append(buf, static_cast<size_t>(rn));
    }

    close(fd);

    if (response)
    {
        std::istringstream iss(collected);
        std::string line;
        while (std::getline(iss, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (!line.empty())
            {
                response->push_back(line);
                std::fprintf(stdout, "[AMLgsMenu] ap_tcp resp: %s\n", line.c_str());
            }
        }
        std::fflush(stdout);
    }

    return !collected.empty();
}
