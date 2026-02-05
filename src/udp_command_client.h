#pragma once

#include "command_transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
    bool SendWithReplyAsync(const std::string &cmd,
                            const std::vector<std::string> &keys,
                            std::function<void(const std::unordered_map<std::string, std::string> &, bool)> cb,
                            int timeout_ms = 1000);

private:
    struct PendingBatch
    {
        std::unordered_set<std::string> keys;
        std::unordered_map<std::string, std::string> values;
        std::string current_key;
        std::chrono::steady_clock::time_point deadline;
        std::function<void(const std::unordered_map<std::string, std::string> &, bool)> callback;
    };

    bool Execute(const std::string &cmd, std::vector<std::string> *response, int timeout_ms);
    static std::string Trim(const std::string &text);
    void StartRxThread();
    void StopRxThread();
    void RxThreadMain();
    void HandleRxLine(const std::string &line,
                      std::function<void(const std::unordered_map<std::string, std::string> &, bool)> &cb,
                      std::unordered_map<std::string, std::string> &cb_values,
                      bool &cb_ok);
    void CheckPendingTimeouts(std::function<void(const std::unordered_map<std::string, std::string> &, bool)> &cb,
                              std::unordered_map<std::string, std::string> &cb_values,
                              bool &cb_ok);

    int tx_fd_ = -1;
    int rx_fd_ = -1;
    uint16_t tx_port_ = 0;
    uint16_t rx_port_ = 0;
    std::string ip_;
    std::mutex io_mutex_;

    std::atomic<bool> rx_stop_{false};
    std::thread rx_thread_;
    std::mutex pending_mutex_;
    std::deque<PendingBatch> pending_batches_;
};
