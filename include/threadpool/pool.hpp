#pragma once
// ============================================================================
//  pool.hpp —— 线程池本体（项目的心脏）
//
//  对外只暴露 5 个动作：start / submit / cancel / update_config / shutdown，
//  外加一个只读的 snapshot() 给界面用。所有并发细节都封在 pool.cpp 内部。
//
//  内部结构（从下往上看，理解这张图就看懂了 80% 的代码）：
//
//     submit() ──►  PriorityTaskQueue（priority_queue.cpp）
//                      │  加锁压入、notify_one
//                      ▼
//     ┌──────────── ThreadPool::mutex_ + condition_variable ────────────┐
//     │  Worker#1  取任务 → 执行 → mark_idle                            │
//     │  Worker#2  取任务 → 执行 → mark_idle                            │
//     │  ...                                                            │
//     └──────────────────────────────────────────────────────────────────┘
//                      ▲
//                 monitor 线程：每 100ms 看一次队列积压/空闲比例
//                 → 扩容 / 缩容 / 重建优先级堆（老化）
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "threadpool/config.hpp"
#include "threadpool/event_log.hpp"
#include "threadpool/metrics.hpp"
#include "threadpool/priority_queue.hpp"
#include "threadpool/task.hpp"
#include "threadpool/worker.hpp"

namespace tp {

// 界面一次最多展示多少条排队任务 / 多少条事件日志。
// 放在这里是为了让内核与 UI 共用同一个上限，而不是在 App 里再抄一遍魔数。
inline constexpr std::size_t kQueueViewLimit = 64;
inline constexpr std::size_t kEventViewLimit = 200;

class ThreadPool {
public:
    explicit ThreadPool(PoolConfig config = PoolConfig{});
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 创建 min_threads 个工作线程并启动监控线程。
    // 校验的是【调用方原样请求的配置】：非法时返回 false（内部那份 config_ 早已被
    // 夹紧到合法范围，拿它来校验永远通过，那条失败分支就成了摆设）。
    bool start();

    // drain=true  ：把队列里剩余任务跑完再退出（优雅关闭）
    // drain=false ：丢弃排队任务（全部标记为 Cancelled）后立刻退出
    void shutdown(bool drain = true);

    bool running() const;

    // 提交任务。队列满 / 未启动 / 正在关闭时返回 std::nullopt。
    std::optional<TaskId> submit(std::string name, Priority priority, std::function<void()> work);

    // 同上，但任务体额外拿到一个取消令牌：长循环里可以轮询
    // token.is_cancelled()，发现被取消就自己提前 return（协作式取消）。
    std::optional<TaskId> submit(std::string name, Priority priority,
                                 std::function<void(const CancellationToken&)> work);

    // 取消任务：
    //   · 还在排队 → 直接从队列摘掉，任务永远不会执行，返回 true；
    //   · 已经在跑 → 置起取消标志后返回 true，但线程不会被打断，
    //     任务体必须自己轮询 CancellationToken::is_cancelled() 才会收工；
    //   · 已经结束 / 不存在 → 返回 false。
    bool cancel(TaskId id);

    // 界面每帧调用一次，拿一份无锁的值快照
    PoolSnapshot snapshot() const;

    // 运行期热更新配置（界面滑块实时生效）。非法项会被夹到合法范围、并记一条
    // 告警事件；但这次"原样请求"也会被记住，下一次 start() 仍会据此拒绝启动。
    void update_config(const PoolConfig& config);

    const EventLog& events() const { return events_; }
    void clear_events() { events_.clear(); }

    void reset_counters();

    // 任务级事件（提交/开始/完成）默认按 1/32 采样：几千任务/秒时全量落盘会让
    // 环形缓冲几秒就被刷爆，反而看不到想看的那一条。排查具体任务时打开全量
    // 记录，每个任务的事件都写——代价是界面上“已覆盖”的数字飞涨，抓完记得关。
    void set_task_event_full_trace(bool enabled) {
        full_trace_.store(enabled, std::memory_order_relaxed);
    }
    bool task_event_full_trace() const {
        return full_trace_.load(std::memory_order_relaxed);
    }

private:
    // ---- 工作线程主循环（第 5 课重点）----
    void worker_main(Worker& worker);
    void run_task(Worker& worker, const TaskPtr& task);

    // ---- 提交路径 ----
    bool accepting() const;                    // 池子此刻还收任务吗
    bool enqueue(const TaskPtr& task);         // 锁内入队，失败时记背压事件
    bool should_log_task(TaskId id) const;     // 任务级事件要不要落盘（采样开关）

    // ---- 监控线程：扩容 / 缩容 / 老化重建（第 6、7 课重点）----
    void monitor_main();
    void maybe_scale_locked();                     // 必须在持有 mutex_ 时调用
    std::chrono::milliseconds aging_rebuild_interval() const;  // 老化重建的限流间隔
    std::size_t busy_count_locked() const;
    void spawn_workers_locked(std::size_t count);  // 必须在持有 mutex_ 时调用
    void retire_workers_locked(std::size_t count); // 标记空闲线程退休
    std::vector<std::unique_ptr<Worker>> take_retiring();  // 锁外回收

    // ---- 状态（全部受 mutex_ 保护）----
    mutable std::mutex mutex_;
    std::condition_variable cv_;             // 任务到达 / 关闭 / 退休 唤醒
    // 两份配置，各管一头：
    //   config_            —— 真正生效的那份，永远是 sanitize 之后的合法值；
    //   requested_config_  —— 调用方最后一次"原样"交给我们的，start() 拿它做校验。
    // 声明顺序有讲究：queue_ 构造时要读 config_，所以这两份必须写在 queue_ 前面。
    PoolConfig config_;
    PoolConfig requested_config_;
    PriorityTaskQueue queue_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::unique_ptr<Worker>> retiring_;  // 已标记退休、等待 join
    // 正在执行的任务：cancel() 靠它才能把"已出队"的任务找回来置取消标志
    std::unordered_map<TaskId, TaskPtr> running_tasks_;
    bool running_ = false;
    bool stopping_ = false;
    bool drain_ = true;
    TimePoint started_at_{};
    TimePoint last_scale_at_{};
    TimePoint last_aging_rebuild_{};         // 上次因老化重建堆的时刻（限流用）
    TimePoint idle_since_{};                 // 空闲比例达标的起始时刻（缩容滞回）
    std::size_t next_worker_index_ = 1;

    // ---- 监控线程 ----
    std::thread monitor_;
    std::condition_variable monitor_cv_;
    bool monitor_stop_ = false;

    // ---- 无锁计数器（工作线程随手累加，snapshot 时汇总）----
    std::atomic<TaskId> next_id_{1};
    std::atomic<std::uint64_t> next_seq_{1};
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> started_{0};   // 真正开始执行过的任务数（算平均等待时间用）
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::uint64_t> cancelled_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> wait_ns_total_{0};
    std::atomic<std::uint64_t> run_ns_total_{0};
    // 任务级事件是否全量记录（true = 关掉 1/32 采样）；和上面这些计数器一样无锁读写
    std::atomic<bool> full_trace_{false};

    EventLog events_;
};

}  // namespace tp
