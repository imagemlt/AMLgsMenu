#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

class CommandExecutor {
public:
    CommandExecutor() = default;
    ~CommandExecutor();

    void Start();
    void Stop();
    void Enqueue(const std::string &cmd);

private:
    void ThreadFunc();

    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<std::string> queue_;
    bool running_ = false;
    bool stop_ = false;
};
