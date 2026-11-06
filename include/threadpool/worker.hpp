#pragma once
// ============================================================================
//  worker.hpp —— 一个工作线程的“身份证”
//
//  Worker 只负责三件事：拥有一个 std::thread、记录自己现在忙不忙、
//  累计自己干过多少活。它不知道队列怎么调度——调度逻辑全在 ThreadPool 里。
//  把“线程的躯壳”和“调度的大脑”分开，是读懂后面 pool.cpp 的关键。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "threadpool/task.hpp"

namespace tp {

class Worker {
public:
    explicit Worker(std::size_t index);
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // 启动线程，body 在另一个线程里跑；body 通常就是 ThreadPool::worker_main。
    // wake_on_retire：析构兜底时用来把卡在条件变量上的线程踹醒。
    // 没有它，置了 retire_requested 也只是"改了个标志"，线程照样睡着，join 永远等不到。
    void launch(std::function<void(Worker&)> body, std::function<void()> wake_on_retire = {});
    void join();
    bool joinable() const { return thread_.joinable(); }

    std::size_t index() const { return index_; }

    // ---- 状态：全部由本线程写、界面/监控线程读，必须是原子的 ----
    void mark_busy(TaskId id);
    void mark_idle();
    bool busy() const { return busy_.load(std::memory_order_acquire); }
    TaskId current_task() const { return current_.load(std::memory_order_acquire); }

    void note_executed(double run_ms);
    std::uint64_t executed() const { return executed_.load(std::memory_order_relaxed); }
    double busy_ms() const {
        return static_cast<double>(busy_us_.load(std::memory_order_relaxed)) / 1000.0;
    }

    // 统计归零（界面上的“清零计数器”按钮用）
    void reset_stats();

    // 缩容时由监控线程置位；工作线程在条件变量苏醒后发现它，就自行退出。
    std::atomic<bool> retire_requested{false};

private:
    std::size_t index_;
    std::thread thread_;
    std::function<void()> wake_on_retire_;  // 析构兜底时用它把睡着的线程踹醒
    std::atomic<bool> busy_{false};
    std::atomic<TaskId> current_{0};
    std::atomic<std::uint64_t> executed_{0};
    std::atomic<std::int64_t> busy_us_{0};
};

}  // namespace tp
