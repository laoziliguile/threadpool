#pragma once
// ============================================================================
//  priority_queue.hpp —— 带优先级的任务队列
//
//  刻意做成「非线程安全」的容器：它只负责两个纯粹的问题——
//    ① 谁应该先被取走（优先级 + 老化 + FIFO 次序）
//    ② 容量满了怎么办（拒绝，形成背压）
//  加锁的责任交给 ThreadPool。这样队列可以单独写单元测试，不用起线程，
//  这是本项目最重要的一个分层决策（见第 3 课）。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "threadpool/task.hpp"

namespace tp {

// ---------------------------------------------------------------------------
//  老化策略：低优先级任务排队越久，有效优先级涨得越高，防止被高优先级任务
//  “永远插队”而饿死。加成 = 等待秒数 × levels_per_second，封顶 max_bonus_levels。
// ---------------------------------------------------------------------------
struct AgingPolicy {
    bool enabled = true;
    double levels_per_second = 0.2;
    int max_bonus_levels = 3;
};

class PriorityTaskQueue {
public:
    explicit PriorityTaskQueue(std::size_t capacity = 2048, AgingPolicy aging = AgingPolicy{});

    // 入队。队列满了返回 false（调用方据此统计 rejected / 触发背压）。
    bool push(TaskPtr task);

    // 取出优先级最高的任务；队列为空返回 nullptr。
    TaskPtr pop();

    // 按 id 移除（用于取消还没开始的任务）。返回被移除的任务，找不到返回 nullptr。
    TaskPtr remove(TaskId id);

    void clear();

    // 优先级老化会让“堆序”随时间失效：堆里存的是静态下标，但得分在变。
    // 所以监控线程每隔一段时间调用一次 rebuild()，用当前时间重算得分并重建堆。
    // 复杂度 O(n)，n = 队列长度，2048 个任务重排约几十微秒，完全可以接受。
    void rebuild();

    std::size_t size() const { return heap_.size(); }
    bool empty() const { return heap_.empty(); }
    std::size_t capacity() const { return capacity_; }
    void set_capacity(std::size_t cap) { capacity_ = cap; }

    void set_aging(AgingPolicy policy) { aging_ = policy; }
    const AgingPolicy& aging() const { return aging_; }

    // 有效优先级（含老化加成），单位是“级”。界面上的“有效优先级”列用它。
    double effective_level(const Task& task, TimePoint now) const;

    // 按当前调度顺序导出前 max_items 个任务（界面队列表格用）。
    std::vector<TaskPtr> snapshot(std::size_t max_items) const;

    std::uint64_t pushed_total() const { return pushed_total_; }
    std::uint64_t rejected_total() const { return rejected_total_; }

private:
    // 堆比较器：返回 true 表示 a 的紧急度 “小于” b，于是 std::*_heap 会把
    // 最紧急的任务顶到 heap_[0]。时间点显式传入，保证一次重建内口径一致。
    bool less_urgent(const TaskPtr& a, const TaskPtr& b, TimePoint now) const;

    std::vector<TaskPtr> heap_;
    std::size_t capacity_;
    AgingPolicy aging_;
    std::uint64_t pushed_total_ = 0;
    std::uint64_t rejected_total_ = 0;
};

}  // namespace tp
