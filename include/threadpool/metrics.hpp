#pragma once
// ============================================================================
//  metrics.hpp —— 线程池的“体检报告”
//
//  界面线程绝不能直接摸线程池内部（会数据竞争），只能定时找线程池要一份
//  PoolSnapshot 值拷贝。Snapshot 是纯数据、无锁、可随便传阅，
//  这是“生产者-消费者”模型在 UI 层最常用的写法（每帧抓一张照片）。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "threadpool/config.hpp"
#include "threadpool/task.hpp"

namespace tp {

// 单个工作线程的状态
struct WorkerInfo {
    std::size_t index = 0;      // 线程编号，从 1 开始，界面上显示 W1/W2...
    bool busy = false;          // 正在执行任务
    bool retiring = false;      // 已被标记为“准备退休”（缩容中）
    TaskId current_task = 0;    // 正在执行的任务 id，0 表示空闲
    std::uint64_t executed = 0; // 累计执行过的任务数
    double busy_ms = 0.0;       // 累计忙碌毫秒数（用来算线程利用率）
};

// 队列里一个任务的“视图”（只拷贝界面需要的字段，不持有任务本身）
struct TaskInfo {
    TaskId id = 0;
    std::string name;
    Priority priority = Priority::Normal;
    double effective_level = 0.0;   // 优先级 + 老化加成
    double waited_ms = 0.0;         // 已经等了多久
};

struct PoolSnapshot {
    bool running = false;
    bool shutting_down = false;

    // ---- 线程 ----
    std::size_t threads = 0;
    std::size_t busy_threads = 0;
    std::size_t idle_threads = 0;
    std::size_t retiring_threads = 0;

    // ---- 队列 ----
    std::size_t queue_size = 0;
    std::size_t queue_capacity = 0;

    // ---- 累计计数 ----
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t rejected = 0;  // 因队列满被拒绝

    // ---- 平均耗时（毫秒）----
    double avg_wait_ms = 0.0;
    double avg_run_ms = 0.0;

    double uptime_seconds = 0.0;

    PoolConfig config;
    std::vector<WorkerInfo> workers;
    std::vector<TaskInfo> queue_view;
};

}  // namespace tp
