#include "command_executor.h"

#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <chrono>

CommandExecutor::~CommandExecutor()
{
    Stop();
}

void CommandExecutor::Start()
{
    if (running_)
        return;
    stop_ = false;
    running_ = true;
    worker_ = std::thread(&CommandExecutor::ThreadFunc, this);
}

void CommandExecutor::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable())
    {
        worker_.join();
    }
    running_ = false;
}

void CommandExecutor::EnqueueShell(const std::string &cmd)
{
    if (!running_)
    {
        std::fprintf(stderr, "[CommandExecutor] cannot enqueue command, executor not running\n");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(CommandJob{CommandJob::Kind::Shell, cmd, {}});
    }
    cv_.notify_one();
}

void CommandExecutor::EnqueueRemote(std::function<void()> job)
{
    if (!running_ || !job)
        return;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(CommandJob{CommandJob::Kind::Remote, {}, std::move(job)});
    }
    cv_.notify_one();
}

void CommandExecutor::ThreadFunc()
{
#ifdef __linux__
    setpriority(PRIO_PROCESS, 0, 5);
#endif
    using namespace std::chrono;
    auto next_report = steady_clock::now() + minutes(1);
    while (true)
    {
        CommandJob job;
        bool have_job = false;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_until(lock, next_report, [&]
                           { return stop_ || !queue_.empty(); });
            auto now = steady_clock::now();
            if (now >= next_report)
            {
                std::fprintf(stdout, "[CommandExecutor] queue depth: %zu\n", queue_.size());
                std::fflush(stdout);
                next_report = now + minutes(1);
            }
            if (stop_ && queue_.empty())
                break;
            if (!queue_.empty())
            {
                job = std::move(queue_.front());
                queue_.pop();
                have_job = true;
            }
        }
        if (!have_job)
        {
            continue;
        }
        if (job.kind == CommandJob::Kind::Shell)
        {
            std::fprintf(stdout, "[AMLgsMenu] exec: %s\n", job.shell_cmd.c_str());
            std::fflush(stdout);
            int rc = std::system(job.shell_cmd.c_str());
            if (rc != 0)
            {
                std::fprintf(stderr, "[AMLgsMenu] Command failed (rc=%d): %s\n", rc, job.shell_cmd.c_str());
            }
        }
        else
        {
            job.remote_job();
        }
    }
}
