// worker.cpp —— 工作线程的躯壳
//
// 这个文件刻意做得很薄：只负责"记录我是谁、我在忙什么、我干了多少活"。
// 真正的调度逻辑一行都不在这里，全在 pool.cpp。职责分离让两边都好读好测。
#include "threadpool/worker.hpp"

#include <utility>

namespace tp {

Worker::Worker(std::size_t index) : index_(index) {}

Worker::~Worker() {
    // 兜底：如果没人 join 就析构了，std::thread 会直接 std::terminate，
    // 所以这里必须先请求退休、再等它退出。
    // 但只置标志是不够的：线程可能正卡在条件变量的 wait 上，没人 notify 就永远醒不过来，
    // 于是 join 会一直等到天荒地老。wake_on_retire_ 就是干这件事的。
    retire_requested.store(true, std::memory_order_release);
    if (wake_on_retire_) wake_on_retire_();
    join();
}

void Worker::launch(std::function<void(Worker&)> body, std::function<void()> wake_on_retire) {
    if (thread_.joinable()) return;  // 已经跑起来了，不要重复启动
    wake_on_retire_ = std::move(wake_on_retire);
    thread_ = std::thread([this, body = std::move(body)]() {
        if (body) body(*this);
    });
}

void Worker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Worker::mark_busy(TaskId id) {
    current_.store(id, std::memory_order_release);
    busy_.store(true, std::memory_order_release);
}

void Worker::mark_idle() {
    busy_.store(false, std::memory_order_release);
    current_.store(0, std::memory_order_release);
}

void Worker::note_executed(double run_ms) {
    executed_.fetch_add(1, std::memory_order_relaxed);
    if (run_ms < 0.0) run_ms = 0.0;
    const std::int64_t us = static_cast<std::int64_t>(run_ms * 1000.0);
    busy_us_.fetch_add(us, std::memory_order_relaxed);
}

void Worker::reset_stats() {
    executed_.store(0, std::memory_order_relaxed);
    busy_us_.store(0, std::memory_order_relaxed);
}

}  // namespace tp
