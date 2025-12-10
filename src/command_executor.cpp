#include "command_executor.h"

#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>

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
    while (true)
    {
        CommandJob job;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [&]
                     { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty())
                break;
            job = std::move(queue_.front());
            queue_.pop();
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
