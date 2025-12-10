#pragma once

#include <condition_variable>
#include <functional>
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
    void EnqueueShell(const std::string &cmd);
    void EnqueueRemote(std::function<void()> job);

private:
    struct CommandJob {
        enum class Kind { Shell, Remote };
        Kind kind = Kind::Shell;
        std::string shell_cmd;
        std::function<void()> remote_job;
    };

    void ThreadFunc();

    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<CommandJob> queue_;
    bool running_ = false;
    bool stop_ = false;
};
