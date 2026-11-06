// tests/test_priority_queue.cpp —— 优先级队列的单元测试
//
// 注意这些用例全部是"单线程、无锁"的：这正是当初把队列与线程池分开的目的。
// 如果优先级逻辑写错了，先在这里挂，而不是让整个程序在界面上表现得很奇怪。
#include "test_framework.hpp"

#include <chrono>
#include <memory>

#include "threadpool/priority_queue.hpp"

namespace {

using namespace std::chrono_literals;

tp::TaskPtr make_task(tp::TaskId id, tp::Priority prio, std::uint64_t seq) {
    auto task = std::make_shared<tp::Task>();
    task->id = id;
    task->priority = prio;
    task->sequence = seq;
    task->submitted_at = tp::Clock::now();
    return task;
}

tp::AgingPolicy no_aging() {
    tp::AgingPolicy policy;
    policy.enabled = false;
    return policy;
}

}  // namespace

TF_TEST(queue_orders_by_priority) {
    tp::PriorityTaskQueue queue(64, no_aging());
    queue.push(make_task(1, tp::Priority::Normal, 1));
    queue.push(make_task(2, tp::Priority::Low, 2));
    queue.push(make_task(3, tp::Priority::Critical, 3));
    queue.push(make_task(4, tp::Priority::High, 4));

    const tp::TaskPtr a = queue.pop();
    const tp::TaskPtr b = queue.pop();
    const tp::TaskPtr c = queue.pop();
    const tp::TaskPtr d = queue.pop();

    TF_CHECK(a != nullptr);
    TF_CHECK(b != nullptr);
    TF_CHECK(c != nullptr);
    TF_CHECK(d != nullptr);
    if (!a || !b || !c || !d) return;

    TF_CHECK_EQ(a->id, static_cast<tp::TaskId>(3));  // Critical 最先出队
    TF_CHECK_EQ(b->id, static_cast<tp::TaskId>(4));  // 然后 High
    TF_CHECK_EQ(c->id, static_cast<tp::TaskId>(1));  // 然后 Normal
    TF_CHECK_EQ(d->id, static_cast<tp::TaskId>(2));  // 最后 Low
}

TF_TEST(queue_is_fifo_for_same_priority) {
    tp::PriorityTaskQueue queue(64, no_aging());
    queue.push(make_task(10, tp::Priority::High, 1));
    queue.push(make_task(11, tp::Priority::High, 2));
    queue.push(make_task(12, tp::Priority::High, 3));

    TF_CHECK_EQ(queue.pop()->id, static_cast<tp::TaskId>(10));
    TF_CHECK_EQ(queue.pop()->id, static_cast<tp::TaskId>(11));
    TF_CHECK_EQ(queue.pop()->id, static_cast<tp::TaskId>(12));
}

TF_TEST(queue_aging_promotes_old_low_priority) {
    tp::AgingPolicy aging;
    aging.enabled = true;
    aging.levels_per_second = 100.0;  // 故意放大，让效果立刻可见
    aging.max_bonus_levels = 3;

    tp::PriorityTaskQueue queue(8, aging);
    queue.push(make_task(1, tp::Priority::Low, 1));
    queue.push(make_task(2, tp::Priority::Normal, 2));

    const auto now = tp::Clock::now();
    const tp::TaskPtr low = queue.snapshot(8).back();   // Low 排在后面
    TF_CHECK(low != nullptr);
    if (low) {
        // 人为把等待时间改到"已经等了 100ms"：加成 = 100ms × 100/s = 10 级，被上限压到 3 级
        const double level_now = queue.effective_level(*low, now + 100ms);
        TF_CHECK(level_now > 1.0);   // 已经超过 Normal 的 1 级
        TF_CHECK(level_now <= 3.0);  // 但没有突破上限
    }
}

TF_TEST(queue_rejects_when_full) {
    tp::PriorityTaskQueue queue(2, no_aging());
    TF_CHECK(queue.push(make_task(1, tp::Priority::Normal, 1)));
    TF_CHECK(queue.push(make_task(2, tp::Priority::Normal, 2)));
    TF_CHECK(!queue.push(make_task(3, tp::Priority::Normal, 3)));
    TF_CHECK_EQ(queue.size(), static_cast<std::size_t>(2));
    TF_CHECK_EQ(queue.rejected_total(), static_cast<std::uint64_t>(1));
    TF_CHECK_EQ(queue.pushed_total(), static_cast<std::uint64_t>(2));
}

TF_TEST(queue_remove_by_id) {
    tp::PriorityTaskQueue queue(8, no_aging());
    queue.push(make_task(1, tp::Priority::Normal, 1));
    queue.push(make_task(2, tp::Priority::High, 2));
    queue.push(make_task(3, tp::Priority::Low, 3));

    const tp::TaskPtr removed = queue.remove(2);
    TF_CHECK(removed != nullptr);
    if (removed) TF_CHECK_EQ(removed->id, static_cast<tp::TaskId>(2));
    TF_CHECK_EQ(queue.size(), static_cast<std::size_t>(2));

    TF_CHECK(queue.remove(999) == nullptr);  // 不存在的 id
    TF_CHECK_EQ(queue.size(), static_cast<std::size_t>(2));

    // 剩下的任务依然能按优先级正确出队
    TF_CHECK_EQ(queue.pop()->id, static_cast<tp::TaskId>(1));
    TF_CHECK_EQ(queue.pop()->id, static_cast<tp::TaskId>(3));
}

TF_TEST(queue_snapshot_is_sorted_and_limited) {
    tp::PriorityTaskQueue queue(64, no_aging());
    queue.push(make_task(1, tp::Priority::Low, 1));
    queue.push(make_task(2, tp::Priority::Critical, 2));
    queue.push(make_task(3, tp::Priority::Normal, 3));
    queue.push(make_task(4, tp::Priority::High, 4));

    const auto all = queue.snapshot(10);
    TF_CHECK_EQ(all.size(), static_cast<std::size_t>(4));
    if (all.size() == 4) {
        TF_CHECK_EQ(all[0]->id, static_cast<tp::TaskId>(2));  // Critical
        TF_CHECK_EQ(all[1]->id, static_cast<tp::TaskId>(4));  // High
        TF_CHECK_EQ(all[2]->id, static_cast<tp::TaskId>(3));  // Normal
        TF_CHECK_EQ(all[3]->id, static_cast<tp::TaskId>(1));  // Low
    }

    const auto top_two = queue.snapshot(2);
    TF_CHECK_EQ(top_two.size(), static_cast<std::size_t>(2));
}
