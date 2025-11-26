#include "command_executor.h"

#include <cstdio>
#include <cstdlib>

CommandExecutor::~CommandExecutor() {
    Stop();
}

void CommandExecutor::Start() {
    if (running_) return;
    stop_ = false;
    running_ = true;
    worker_ = std::thread(&CommandExecutor::ThreadFunc, this);
}

void CommandExecutor::Stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
}

void CommandExecutor::Enqueue(const std::string &cmd) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(cmd);
    }
    cv_.notify_one();
}

void CommandExecutor::ThreadFunc() {
    while (true) {
        std::string cmd;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) break;
            cmd = queue_.front();
            queue_.pop();
        }
        std::fprintf(stdout, "[AMLgsMenu] exec: %s\n", cmd.c_str());
        std::fflush(stdout);
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::fprintf(stderr, "[AMLgsMenu] Command failed (rc=%d): %s\n", rc, cmd.c_str());
        }
    }
}
