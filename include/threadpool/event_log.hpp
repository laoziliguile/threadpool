#pragma once
// ============================================================================
//  event_log.hpp —— 线程安全的事件环形缓冲
//
//  为什么需要它：可视化界面要能回答“刚才发生了什么”。我们把提交、开始、
//  结束、扩容、缩容这些动作都记成一条事件，界面上滚动显示。
//  用环形缓冲（固定容量、满了覆盖最旧的）保证内存不会无限增长，
//  同时避免用 std::vector::erase 造成的 O(n) 搬移。
// ============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "threadpool/task.hpp"

namespace tp {

enum class EventKind {
    Submit,     // 任务入队
    Start,      // 任务开始执行
    Finish,     // 任务成功结束
    Fail,       // 任务抛异常
    Cancel,     // 任务被取消
    Reject,     // 队列满，提交被拒绝
    ScaleUp,    // 扩容
    ScaleDown,  // 缩容
    Info,       // 启动/关闭等普通信息
    Warn,       // 需要引起注意的情况
};

const char* to_string(EventKind kind);
const char* event_label_zh(EventKind kind);

struct PoolEvent {
    TimePoint at{};
    EventKind kind = EventKind::Info;
    TaskId task = 0;      // 0 表示与具体任务无关
    std::string text;
};

class EventLog {
public:
    explicit EventLog(std::size_t capacity = 512);

    // 线程安全：任何线程都可以调用
    void add(EventKind kind, std::string text, TaskId task = 0);

    // 取最近 n 条（按时间正序，最旧的在前）
    std::vector<PoolEvent> tail(std::size_t n) const;

    void clear();

    std::uint64_t total() const { return total_.load(std::memory_order_relaxed); }

    // 因为环形缓冲被写满而覆盖掉的条数（界面用它提示"日志有丢失"）。
    std::uint64_t dropped() const;

private:
    mutable std::mutex mutex_;
    std::vector<PoolEvent> buffer_;
    std::size_t head_ = 0;   // 下一条写入位置
    std::size_t size_ = 0;
    std::atomic<std::uint64_t> total_{0};
    std::uint64_t dropped_{0};
};

}  // namespace tp
