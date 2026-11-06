#pragma once
// ============================================================================
//  task.hpp —— 任务的“数据模型”
//
//  这个头文件里没有任何线程池逻辑，只有“一个任务长什么样”。
//  把它单独拆出来的原因：任务的生命周期状态（等待/运行/成功/失败/取消）
//  会被工作线程写、被界面线程读，所以字段必须用原子类型，这是后面所有
//  并发逻辑的基础。
//
//  这里的小函数（优先级查表、时间换算）全部写成 inline：
//  它们只有几行、每个调用点都能被内联展开，没必要为它们单独开一个 .cpp。
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace tp {

using TaskId = std::uint64_t;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ---------------------------------------------------------------------------
//  小工具：时间点 → 纳秒 / 时长 → 毫秒
//  （放在最前面：Task 的 waited_ms/run_ms 要用到它们）
// ---------------------------------------------------------------------------
inline long long to_ns(TimePoint t) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
}
inline TimePoint from_ns(long long ns) {
    return TimePoint(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(ns)));
}
inline double ms_between(TimePoint a, TimePoint b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// ---------------------------------------------------------------------------
//  优先级：数值越大越紧急。4 档足够表达业务语义，又不会让界面太花。
// ---------------------------------------------------------------------------
enum class Priority : int {
    Low = 0,       // 后台任务：日志归档、缓存预热
    Normal = 1,    // 普通业务：默认档
    High = 2,      // 用户可感知：页面请求、交互反馈
    Critical = 3,  // 必须立刻做：心跳、超时补偿
};

constexpr int kPriorityLevels = 4;
constexpr Priority kPriorityList[kPriorityLevels] = {
    Priority::Low, Priority::Normal, Priority::High, Priority::Critical};

// 英文名用于日志（避免控制台编码问题），中文名用于界面。
inline const char* to_string(Priority p) {
    switch (p) {
        case Priority::Low: return "Low";
        case Priority::Normal: return "Normal";
        case Priority::High: return "High";
        case Priority::Critical: return "Critical";
    }
    return "Normal";  // 枚举已全覆盖，这里只是让编译器闭嘴
}

inline const char* priority_label_zh(Priority p) {
    switch (p) {
        case Priority::Low: return "低";
        case Priority::Normal: return "普通";
        case Priority::High: return "高";
        case Priority::Critical: return "紧急";
    }
    return "普通";
}

// 0..3：界面下拉框的下标。非法值统一夹到 Normal，避免下标越界。
inline int priority_slot(Priority p) {
    const int slot = static_cast<int>(p);
    if (slot < 0 || slot >= kPriorityLevels) return static_cast<int>(Priority::Normal);
    return slot;
}

// 下标 → Priority，越界时夹紧到两端（-7 → Low，99 → Critical，不能越界读 kPriorityList）
inline Priority priority_from_slot(int slot) {
    if (slot < 0) return Priority::Low;
    if (slot >= kPriorityLevels) return Priority::Critical;
    return kPriorityList[slot];
}

// ---------------------------------------------------------------------------
//  任务状态机：
//      Pending ──► Running ──► Succeeded
//         │           ├────► Failed（任务体抛异常）
//         │           └────► Cancelled（跑到一半发现取消请求，自己提前收工）
//         └──► Cancelled（还没开始就被取消，直接从队列摘掉）
//
//  取消是两段式的：
//    · 还在排队 → cancel() 把它从队列里摘掉，永远不会执行；
//    · 已经在跑 → cancel() 只能置起 cancel_requested，任务体自己轮询
//      CancellationToken::is_cancelled() 决定何时收工。强行打断线程
//      （pthread_cancel / TerminateThread）会漏锁、漏析构，不可用。
// ---------------------------------------------------------------------------
enum class TaskState {
    Pending,    // 在队列里排队
    Running,    // 已被某个 Worker 取走
    Succeeded,  // 正常结束
    Failed,     // 抛异常结束
    Cancelled,  // 未执行完就被取消
};

inline const char* to_string(TaskState s) {
    switch (s) {
        case TaskState::Pending: return "Pending";
        case TaskState::Running: return "Running";
        case TaskState::Succeeded: return "Succeeded";
        case TaskState::Failed: return "Failed";
        case TaskState::Cancelled: return "Cancelled";
    }
    return "Pending";
}

inline const char* task_state_label_zh(TaskState s) {
    switch (s) {
        case TaskState::Pending: return "排队";
        case TaskState::Running: return "执行中";
        case TaskState::Succeeded: return "成功";
        case TaskState::Failed: return "失败";
        case TaskState::Cancelled: return "已取消";
    }
    return "排队";
}

struct Task;

using TaskPtr = std::shared_ptr<Task>;

// ---------------------------------------------------------------------------
//  Task：一个待执行的工作单元 + 它的统计信息
//
//  为什么用 shared_ptr 传递？
//    · 队列、Worker、界面快照这三方都会短暂持有同一个任务；
//    · Task 内部有 std::atomic 成员，天然不可拷贝，只能靠指针共享。
// ---------------------------------------------------------------------------
struct Task {
    TaskId id = 0;                    // 全局唯一，单调递增
    std::string name;                 // 界面上显示的名字，如 "cpu-task-42"
    Priority priority = Priority::Normal;
    std::uint64_t sequence = 0;       // 提交序号，用于同优先级时保证 FIFO
    std::function<void()> work;       // 真正干活的可调用对象
    TimePoint submitted_at{};         // 入队时刻（提交线程写入，之后只读）

    // ---- 运行期状态：会被别的线程并发读，所以是原子的 ----
    std::atomic<TaskState> state{TaskState::Pending};
    std::atomic<bool> cancel_requested{false};
    // 任务体是否真的"看到了"取消请求（只读标志不算数，要有人轮询到才算）
    std::atomic<bool> cancel_observed{false};

    // 时间戳统一存“纳秒计数”。C++ 里 std::atomic<time_point> 不存在，
    // 但 steady_clock 的 duration 是整数，可以无损塞进 long long。
    std::atomic<long long> started_ns{0};
    std::atomic<long long> finished_ns{0};

    Task() = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) = delete;
    Task& operator=(Task&&) = delete;

    // 已等待时长（毫秒）：排队中算到 now，已开始算到开始时刻
    double waited_ms(TimePoint now) const {
        if (state.load(std::memory_order_acquire) == TaskState::Pending) {
            return ms_between(submitted_at, now);
        }
        // 已经开始或结束：等待时间在开始那一刻就定格了
        const long long start_ns = started_ns.load(std::memory_order_relaxed);
        if (start_ns > 0) {
            return ms_between(submitted_at, from_ns(start_ns));
        }
        return 0.0;
    }

    // 已执行时长（毫秒）：未开始返回 0，已结束返回完整执行耗时
    double run_ms(TimePoint now) const {
        const long long start_ns = started_ns.load(std::memory_order_relaxed);
        if (start_ns <= 0) return 0.0;  // 还没开始
        const long long finish_ns = finished_ns.load(std::memory_order_relaxed);
        const TimePoint end = (finish_ns > 0) ? from_ns(finish_ns) : now;
        return ms_between(from_ns(start_ns), end);
    }

    bool is_pending() const { return state.load(std::memory_order_acquire) == TaskState::Pending; }
    bool is_running() const { return state.load(std::memory_order_acquire) == TaskState::Running; }
    bool is_finished() const { return state.load(std::memory_order_acquire) >= TaskState::Succeeded; }
    bool cancel_requested_flag() const { return cancel_requested.load(std::memory_order_relaxed); }
};

// ---------------------------------------------------------------------------
//  CancellationToken：交给任务体的一只“耳朵”
//
//  submit() 的重载会把这个 token 传进任务体，任务体在长循环里轮询
//  token.is_cancelled()，发现被取消就自己 return —— 这就是协作式取消。
//
//  用 weak_ptr 而不是 shared_ptr 是刻意的：token 常常被任务体捕获进
//  Task::work 里，若持强引用就会形成 Task → work → token → Task 的循环，
//  任务结束后永远不释放。weak_ptr 只在任务还活着时有意义，正好够用。
// ---------------------------------------------------------------------------
class CancellationToken {
public:
    CancellationToken() = default;

    // 一般由 ThreadPool 在提交时构造。公开出来只是为了让包装任务体的代码
    // 也能构造它（例如 submit 的 lambda 里）。
    explicit CancellationToken(std::weak_ptr<Task> task) : task_(std::move(task)) {}

    // 任务已结束/已释放时返回 false：此时再去"取消"已经没有意义了
    bool valid() const noexcept { return !task_.expired(); }

    bool is_cancelled() const noexcept {
        const TaskPtr task = task_.lock();
        if (!task || !task->cancel_requested.load(std::memory_order_relaxed)) return false;
        // 记一笔"确实被观察到了"：池子据此区分"任务体看到取消后主动收工"
        // 和"任务体根本没理这回事、把活干完了"，两者的终态不一样。
        task->cancel_observed.store(true, std::memory_order_relaxed);
        return true;
    }

    TaskId id() const noexcept {
        const TaskPtr task = task_.lock();
        return task ? task->id : 0;
    }

private:
    std::weak_ptr<Task> task_;
};

}  // namespace tp
