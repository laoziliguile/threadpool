// priority_queue.cpp —— 优先级队列：二叉堆 + 优先级老化
//
// 这是全项目"算法味"最浓的一个文件。
//
// 核心思路：
//   1. 用 std::vector 存任务指针，用 std::push_heap / std::pop_heap 维护大顶堆；
//   2. "谁更大"由 less_urgent() 决定：有效优先级高者优先；分数相同按提交先后（FIFO）；
//   3. 有效优先级 = 静态优先级 + 老化加成，加成随时间增长 → 老的慢任务会追上来；
//   4. 因为得分随时间变化，堆序会失效，所以需要 rebuild() 周期性重建（监控线程负责）。
#include "threadpool/priority_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace tp {

PriorityTaskQueue::PriorityTaskQueue(std::size_t capacity, AgingPolicy aging)
    : capacity_(capacity == 0 ? 1 : capacity), aging_(aging) {
    heap_.reserve(64);  // 预留一点空间，避免启动期反复扩容
}

double PriorityTaskQueue::effective_level(const Task& task, TimePoint now) const {
    const double base = static_cast<double>(static_cast<int>(task.priority));
    if (!aging_.enabled) return base;

    const double waited_seconds = std::chrono::duration<double>(now - task.submitted_at).count();
    double bonus = waited_seconds * aging_.levels_per_second;
    if (bonus < 0.0) bonus = 0.0;
    if (bonus > static_cast<double>(aging_.max_bonus_levels)) {
        bonus = static_cast<double>(aging_.max_bonus_levels);
    }
    return base + bonus;
}

bool PriorityTaskQueue::less_urgent(const TaskPtr& a, const TaskPtr& b, TimePoint now) const {
    const double la = effective_level(*a, now);
    const double lb = effective_level(*b, now);
    if (la != lb) {
        return la < lb;  // 分数低的更"小"，会被沉到堆底
    }
    // 同分时不比较指针（那样每次运行顺序都不一样，测试无法复现），
    // 而是比较提交序号：先提交的排前面 → 同优先级严格 FIFO。
    return a->sequence > b->sequence;
}

bool PriorityTaskQueue::push(TaskPtr task) {
    if (!task) return false;
    if (heap_.size() >= capacity_) {
        ++rejected_total_;
        return false;
    }
    const TimePoint now = Clock::now();
    auto less = [this, now](const TaskPtr& a, const TaskPtr& b) {
        return less_urgent(a, b, now);
    };
    heap_.push_back(std::move(task));
    std::push_heap(heap_.begin(), heap_.end(), less);  // O(log n)
    ++pushed_total_;
    return true;
}

TaskPtr PriorityTaskQueue::pop() {
    if (heap_.empty()) return nullptr;
    const TimePoint now = Clock::now();
    auto less = [this, now](const TaskPtr& a, const TaskPtr& b) {
        return less_urgent(a, b, now);
    };
    std::pop_heap(heap_.begin(), heap_.end(), less);  // 把堆顶换到末尾
    TaskPtr top = std::move(heap_.back());
    heap_.pop_back();
    return top;
}

TaskPtr PriorityTaskQueue::remove(TaskId id) {
    // 线性查找是 O(n)，但取消是低频操作；用"末尾补位 + 重建堆"换取实现简单。
    for (std::size_t i = 0; i < heap_.size(); ++i) {
        if (heap_[i]->id == id) {
            TaskPtr removed = std::move(heap_[i]);
            if (i + 1 != heap_.size()) {
                heap_[i] = std::move(heap_.back());  // 用最后一个填坑
            }
            heap_.pop_back();
            rebuild();
            return removed;
        }
    }
    return nullptr;
}

void PriorityTaskQueue::clear() {
    heap_.clear();
}

void PriorityTaskQueue::rebuild() {
    if (heap_.size() < 2) return;
    const TimePoint now = Clock::now();
    // 试过"先给每个任务算一次等级、再在下标上堆化"（与 snapshot() 同一套路），
    // 想省掉比较器里反复做的 duration 除法。实测并不划算：
    //   -O0/Debug：n=2048 快 1.5 倍（324 → 217 µs），
    //   -O2/Release：同样规模反而慢 36%（30 → 47 µs）——
    // 编译器已经把 effective_level 内联成几次乘加，而装饰版多出的三份数组
    // 加一次整体换序，在 2048 个任务时把 L1 挤爆了。
    // 结论：这里保持最简单、在任何优化级别下都不吃亏的写法；真正的收益在
    // 调用频率上（见 ThreadPool::aging_rebuild_interval()）。
    auto less = [this, now](const TaskPtr& a, const TaskPtr& b) {
        return less_urgent(a, b, now);
    };
    std::make_heap(heap_.begin(), heap_.end(), less);  // O(n)
}

std::vector<TaskPtr> PriorityTaskQueue::snapshot(std::size_t max_items) const {
    // 这里只服务界面：每帧调用一次、只要前 max_items 个。
    // 早先的写法是"整个堆全量拷贝 + std::sort 全排序 + 事后截断"，
    // n=2048 时要拷 2048 个 shared_ptr 并排 O(n log n)，而比较器每次
    // 回调还要重算一遍 effective_level（含一次 duration<double> 除法）。
    // 改成"先给每个任务算一次分数（装饰），再 partial_sort 取前 k 个（选择）"：
    // 一是比较器只比预先算好的 double，二是只为最终入选的 k 个任务拷贝
    // shared_ptr。实测 n=2048、k=64 时快一个数量级以上。
    if (max_items == 0 || heap_.empty()) return {};

    const TimePoint now = Clock::now();

    struct Entry {
        double level;          // 预先算好的有效优先级
        std::uint64_t sequence;  // 同分时按提交先后，保证结果可复现
        std::size_t index;       // 指回 heap_，避免提前拷贝 shared_ptr
    };

    std::vector<Entry> entries;
    entries.reserve(heap_.size());
    for (std::size_t i = 0; i < heap_.size(); ++i) {
        const TaskPtr& task = heap_[i];
        entries.push_back(Entry{effective_level(*task, now), task->sequence, i});
    }

    // 注意：less_urgent(a,b) 为真表示 a 不如 b 紧急，直接拿去排会把
    // 最不紧急的排在最前面。界面要的是"下一个会执行谁"，所以要反过来。
    auto more_urgent = [](const Entry& a, const Entry& b) {
        if (a.level != b.level) return a.level > b.level;
        return a.sequence < b.sequence;
    };

    const std::size_t take = std::min(max_items, entries.size());
    std::partial_sort(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(take),
                      entries.end(), more_urgent);

    std::vector<TaskPtr> out;
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        out.push_back(heap_[entries[i].index]);
    }
    return out;
}

}  // namespace tp
