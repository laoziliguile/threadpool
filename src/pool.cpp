// pool.cpp —— 线程池本体：取任务、执行、扩容、缩容、关闭
//
// 读这个文件建议顺序：submit → worker_main → monitor_main → shutdown。
// 三个"最容易写错"的地方已经用【易错点】标出来了，请重点看。
#include "threadpool/pool.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <system_error>
#include <utility>

namespace tp {

namespace {

// 监控线程的心跳间隔。100ms 是"反应够快"和"CPU 开销够小"的折中。
constexpr auto kMonitorTick = std::chrono::milliseconds(100);

// 任务级事件按 1/32 采样（id 低 5 位为 0 的才记）。
// 为什么不全记：任务吞吐上万时，日志本身的加锁和字符串分配会变成新瓶颈。
// 失败、取消、拒绝、扩缩容这些“重要但低频”的事件则一条不落。
constexpr TaskId kTaskEventSampleMask = 0x1Fu;

// 老化重建的“精度”：有效等级攒够这么多级，重建一次堆就够。
// 老化是连续的（默认 0.2 级/秒），监控线程 100ms 一跳只让它变化 0.02 级，
// 每跳都重建纯属白干——见 ThreadPool::aging_rebuild_interval()。
constexpr double kAgingRebuildLevelStep = 0.1;

double ns_between(TimePoint a, TimePoint b) {
    if (b <= a) return 0.0;
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

}  // namespace

// ---------------------------------------------------------------------------
//  构造 / 析构
// ---------------------------------------------------------------------------
ThreadPool::ThreadPool(PoolConfig config)
    // 【易错点】queue_ 与 config_ 必须来自同一份、且都是 sanitize 之后的配置。
    // 早先 queue_ 用的是原始配置、config_ 用的是夹紧后的，
    // 结果"队列实际容量"和快照里显示的容量会不一致。
    : config_(config.sanitized()),
      requested_config_(config),
      queue_(config_.queue_capacity,
             AgingPolicy{config_.aging_enabled,
                         config_.aging_levels_per_second,
                         config_.aging_max_bonus_levels}) {}

ThreadPool::~ThreadPool() {
    // 析构里兜底调用 shutdown：即使使用者忘了关，也不会让 std::thread 在
    // 析构时报 "terminate called without an active exception"。
    shutdown(true);
}

// ---------------------------------------------------------------------------
//  start：建第一批线程 + 起监控线程
// ---------------------------------------------------------------------------
bool ThreadPool::start() {
    std::vector<std::unique_ptr<Worker>> rollback_join;
    bool started = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return false;

        // 校验的是【调用方原样请求的那份配置】，不是夹紧后的 config_。
        // 拿 config_ 来校验永远为真（sanitize 的边界恰好满足全部规则），
        // 那条失败分支就成了摆设——文档里"配置非法时返回 false"也就成了空话。
        const std::vector<std::string> errors = requested_config_.validate();
        if (!errors.empty()) {
            for (const auto& err : errors) {
                events_.add(EventKind::Warn, "配置非法，线程池未启动：" + err);
            }
            return false;
        }

        running_ = true;
        stopping_ = false;
        drain_ = true;
        started_at_ = Clock::now();
        last_scale_at_ = started_at_;
        last_aging_rebuild_ = started_at_;
        idle_since_ = TimePoint{};
        monitor_stop_ = false;

        // 【易错点】建线程是会抛 std::system_error 的（句柄耗尽、内存不足……）。
        // 此刻 running_ 已经是 true，如果让异常直接逃出去，调用方拿到的是一个
        // "自称在跑、其实没监控线程"的坏池子，而 start() 声明返回 bool 本就不该抛。
        // 所以这里自己吞掉并回滚：已经起来的线程标退休、搬到锁外 join。
        try {
            spawn_workers_locked(config_.min_threads);
            // 监控线程在锁内创建，但它在锁外才真正开始跑（等我们释放 mutex_）
            monitor_ = std::thread(&ThreadPool::monitor_main, this);
        } catch (const std::system_error& e) {
            started = false;
            running_ = false;
            stopping_ = false;
            monitor_stop_ = true;
            for (auto& worker : workers_) {
                worker->retire_requested.store(true, std::memory_order_release);
                rollback_join.push_back(std::move(worker));
            }
            workers_.clear();
            events_.add(EventKind::Warn, std::string("线程池启动失败，已回滚：") + e.what());
        }
    }

    if (!started) {
        // 锁外唤醒再 join：这些线程要先拿到 mutex_ 才能看到退休标志并退出
        cv_.notify_all();
        for (auto& worker : rollback_join) {
            if (worker && worker->joinable()) worker->join();
        }
        return false;
    }

    events_.add(EventKind::Info,
                "线程池启动：初始 " + std::to_string(config_.min_threads) +
                    " 线程，上限 " + std::to_string(config_.max_threads));
    return true;
}

// ---------------------------------------------------------------------------
//  shutdown：两种关闭语义
// ---------------------------------------------------------------------------
void ThreadPool::shutdown(bool drain) {
    std::vector<std::unique_ptr<Worker>> to_join;
    std::size_t dropped = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;

        stopping_ = true;
        drain_ = drain;
        monitor_stop_ = true;

        if (!drain) {
            // 非优雅关闭：把排队任务全部标成 Cancelled，然后丢掉
            while (TaskPtr task = queue_.pop()) {
                task->state.store(TaskState::Cancelled, std::memory_order_release);
                cancelled_.fetch_add(1, std::memory_order_relaxed);
                ++dropped;
            }
            // 已经在跑的也"请求"取消：愿意轮询 token 的任务体能立刻收工；
            // 不理会的任务照旧跑完（后面的 join 会等它），绝不强杀线程。
            for (auto& entry : running_tasks_) {
                entry.second->cancel_requested.store(true, std::memory_order_release);
            }
        }
    }

    cv_.notify_all();          // 唤醒所有工作线程
    monitor_cv_.notify_all();  // 唤醒监控线程
    if (monitor_.joinable()) monitor_.join();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& w : retiring_) to_join.push_back(std::move(w));
        retiring_.clear();
        for (auto& w : workers_) to_join.push_back(std::move(w));
        workers_.clear();
    }
    // 【易错点】join 一定要在锁外。
    // 如果握着 mutex_ 等线程退出，而线程退出前又要拿 mutex_，就会死锁。
    for (auto& w : to_join) {
        if (w && w->joinable()) w->join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        stopping_ = false;
    }

    if (dropped > 0) {
        events_.add(EventKind::Cancel,
                    "非优雅关闭：丢弃 " + std::to_string(dropped) + " 个排队任务");
    }
    events_.add(EventKind::Info, "线程池已关闭");
}

bool ThreadPool::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && !stopping_;
}

// ---------------------------------------------------------------------------
//  提交路径的两个小工具
// ---------------------------------------------------------------------------
bool ThreadPool::accepting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && !stopping_;
}

// 任务级事件要不要落盘：默认按 id 采样 1/32，打开全量记录后一律落盘。
// 只读一个原子量，所以 enqueue() 在锁内调用它也是安全的。
bool ThreadPool::should_log_task(TaskId id) const {
    if (full_trace_.load(std::memory_order_relaxed)) return true;
    return (id & kTaskEventSampleMask) == 0;
}

bool ThreadPool::enqueue(const TaskPtr& task) {
    bool pushed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stopping_) return false;  // 装箱期间池子被关了
        pushed = queue_.push(task);
        if (pushed) {
            submitted_.fetch_add(1, std::memory_order_relaxed);
            // 【易错点】Submit 事件必须在这个临界区里发出去。
            // 一旦先解锁、事后再补日志，worker 可能已经 pop 了它并写下
            // Start/Finish——日志里就会出现"先完成、后提交"的倒挂顺序。
            // 这里套一层 events_ 自己的锁是安全的：事件日志从不回调线程池。
            if (should_log_task(task->id)) {
                events_.add(EventKind::Submit, "提交任务 " + task->name, task->id);
            }
        }
    }

    if (!pushed) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        events_.add(EventKind::Reject, "队列已满，提交被拒绝：" + task->name, task->id);
        return false;
    }

    cv_.notify_one();  // 只叫醒一个：一个任务只需要一个线程
    return true;
}

// ---------------------------------------------------------------------------
//  submit：把任务包装好塞进队列
// ---------------------------------------------------------------------------
std::optional<TaskId> ThreadPool::submit(std::string name, Priority priority,
                                         std::function<void()> work) {
    if (!work) return std::nullopt;
    // 先在锁内确认池子还在收任务：未启动/正在关闭时直接拒绝，
    // 不再白白消耗一个 id 和序号、也不再做一次堆分配。
    if (!accepting()) return std::nullopt;

    auto task = std::make_shared<Task>();
    // id 和序号都用原子自增取，不需要串行化
    task->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    task->sequence = next_seq_.fetch_add(1, std::memory_order_relaxed);
    task->priority = priority;
    task->name = std::move(name);
    task->work = std::move(work);
    task->submitted_at = Clock::now();

    if (!enqueue(task)) return std::nullopt;
    return task->id;
}

// 带取消令牌的重载：任务体在长循环里轮询 token，发现被取消就自己收工。
std::optional<TaskId> ThreadPool::submit(std::string name, Priority priority,
                                         std::function<void(const CancellationToken&)> work) {
    if (!work) return std::nullopt;
    if (!accepting()) return std::nullopt;

    auto task = std::make_shared<Task>();
    task->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    task->sequence = next_seq_.fetch_add(1, std::memory_order_relaxed);
    task->priority = priority;
    task->name = name;
    task->submitted_at = Clock::now();

    // 【易错点】这里必须用 weak_ptr 指向自己：
    // Task::work 是 Task 的成员，闭包再捕获 shared_ptr<Task> 就成环了
    // （Task → work → 闭包 → Task），任务结束后引用计数永远回不到 0。
    // 执行期间 worker 手里一直握着 shared_ptr，所以 lock() 一定成功。
    std::weak_ptr<Task> self = task;
    task->work = [body = std::move(work), self]() {
        const TaskPtr alive = self.lock();
        body(CancellationToken(alive));
    };

    if (!enqueue(task)) return std::nullopt;
    return task->id;
}

// ---------------------------------------------------------------------------
//  cancel：两段式取消
//    · 还在排队 → 从队列摘掉，任务永远不会执行；
//    · 已经被 worker 取走 → 只能置标志，等任务体自己轮询收工（协作式）。
//  这里绝不能强杀线程（pthread_cancel / TerminateThread）：锁、栈上的对象、
//  RAII 析构全都会被跳过，池子会以最难查的方式坏掉。
// ---------------------------------------------------------------------------
bool ThreadPool::cancel(TaskId id) {
    TaskPtr task;
    bool was_queued = false;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task = queue_.remove(id);  // 第一种情况：还在排队
        if (task) {
            was_queued = true;
            found = true;
        } else {
            const auto it = running_tasks_.find(id);  // 第二种情况：已经被取走、正在跑
            // 状态已经是终态就不算了：worker 收工后到把任务从 running_tasks_ 里
            // 摘掉之间有一个很短的窗口，这里按"已结束"处理，返回值才稳定可测。
            if (it != running_tasks_.end() && !it->second->is_finished()) {
                task = it->second;
                found = true;
            }
        }
        if (found) {
            task->cancel_requested.store(true, std::memory_order_release);
            if (was_queued) {
                // 排队中的任务不会被执行，这里就能直接定终态
                task->state.store(TaskState::Cancelled, std::memory_order_release);
                cancelled_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // 事件在锁外发：EventLog 自己有一把锁，嵌套取锁不是好习惯
    if (!found) {
        events_.add(EventKind::Warn, "取消失败：任务不存在或已结束", id);
        return false;
    }
    if (was_queued) {
        events_.add(EventKind::Cancel, "任务已取消（排队中）：" + task->name, id);
    } else {
        events_.add(EventKind::Cancel, "已请求取消（执行中，等任务体收工）：" + task->name, id);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  update_config：界面滑块实时生效
// ---------------------------------------------------------------------------
void ThreadPool::update_config(const PoolConfig& config) {
    const PoolConfig cfg = config.sanitized();
    std::vector<std::unique_ptr<Worker>> to_join;
    std::string warning;
    bool monitor_alive = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 记住原始请求：夹紧是为了让池子能继续跑，但不能把非法配置"洗白"，
        // 否则下一次 start() 又会以为一切正常。
        requested_config_ = config;
        config_ = cfg;
        // 真的夹紧了就说一声：用户才知道自己填的值没被完全采纳
        const std::vector<std::string> problems = config.validate();
        if (!problems.empty()) {
            warning = "配置非法，已夹紧后应用：" + problems.front();
        }
        queue_.set_capacity(cfg.queue_capacity);
        queue_.set_aging(AgingPolicy{cfg.aging_enabled,
                                     cfg.aging_levels_per_second,
                                     cfg.aging_max_bonus_levels});

        // 注意不能在这里 return：配置其实已经生效，若直接返回，
        // 下面那条"配置已更新"事件就被吞掉了（界面日志里看不到反馈）。
        monitor_alive = monitor_.joinable() && !monitor_stop_;
        if (running_ && !stopping_) {
            // 最小线程数调大了：立刻补线程
            while (workers_.size() < cfg.min_threads) {
                spawn_workers_locked(1);
            }
            // 最大线程数调小了：立刻回收超出的部分（从空闲的、编号大的开始）
            if (workers_.size() > cfg.max_threads) {
                retire_workers_locked(workers_.size() - cfg.max_threads);
            }
        }
    }

    // 退休线程的 join 交给监控线程（它本来就在做这件事），
    // 只有监控线程不在时才由调用方兜底 join —— 否则界面线程会被
    // 一个正在跑长任务的 worker 卡住，直到它自己结束。
    if (!monitor_alive) {
        to_join = take_retiring();
        for (auto& w : to_join) {
            if (w && w->joinable()) w->join();
        }
    }
    if (!warning.empty()) events_.add(EventKind::Warn, warning);
    events_.add(EventKind::Info, "配置已更新：线程范围 " +
                                     std::to_string(cfg.min_threads) + "~" +
                                     std::to_string(cfg.max_threads));
}

// ---------------------------------------------------------------------------
//  snapshot：给界面拍一张快照（值拷贝，之后随便传）
// ---------------------------------------------------------------------------
PoolSnapshot ThreadPool::snapshot() const {
    PoolSnapshot snap;
    std::lock_guard<std::mutex> lock(mutex_);

    snap.running = running_ && !stopping_;
    snap.shutting_down = stopping_;
    snap.config = config_;

    snap.threads = workers_.size();
    snap.retiring_threads = retiring_.size();
    snap.busy_threads = 0;
    snap.workers.reserve(workers_.size());
    for (const auto& w : workers_) {
        WorkerInfo info;
        info.index = w->index();
        info.busy = w->busy();
        info.retiring = w->retire_requested.load(std::memory_order_relaxed);
        info.current_task = w->current_task();
        info.executed = w->executed();
        info.busy_ms = w->busy_ms();
        if (info.busy) ++snap.busy_threads;
        snap.workers.push_back(info);
    }
    snap.idle_threads = snap.threads - snap.busy_threads;

    snap.queue_size = queue_.size();
    snap.queue_capacity = queue_.capacity();

    snap.submitted = submitted_.load(std::memory_order_relaxed);
    snap.completed = completed_.load(std::memory_order_relaxed);
    snap.failed = failed_.load(std::memory_order_relaxed);
    snap.cancelled = cancelled_.load(std::memory_order_relaxed);
    snap.rejected = rejected_.load(std::memory_order_relaxed);

    const std::uint64_t started = started_.load(std::memory_order_relaxed);
    if (started > 0) {
        snap.avg_wait_ms =
            static_cast<double>(wait_ns_total_.load(std::memory_order_relaxed)) / 1e6 /
            static_cast<double>(started);
    }
    const std::uint64_t finished_running = started_.load(std::memory_order_relaxed);
    if (finished_running > 0) {
        snap.avg_run_ms =
            static_cast<double>(run_ns_total_.load(std::memory_order_relaxed)) / 1e6 /
            static_cast<double>(finished_running);
    }

    if (started_at_ != TimePoint{}) {
        snap.uptime_seconds = ns_between(started_at_, Clock::now()) / 1e9;
    }

    // 队列表格：只取前 64 个，避免界面排版跟不上
    const TimePoint now = Clock::now();
    for (const auto& task : queue_.snapshot(kQueueViewLimit)) {
        TaskInfo info;
        info.id = task->id;
        info.name = task->name;
        info.priority = task->priority;
        info.effective_level = queue_.effective_level(*task, now);
        info.waited_ms = task->waited_ms(now);
        snap.queue_view.push_back(std::move(info));
    }
    return snap;
}

void ThreadPool::reset_counters() {
    {
        // 计数器和 worker 的累计量一起在锁内清零：worker 线程是在锁内
        // 记 executed/busy 的，若这里无锁清零，可能把它正在累加的那一笔抹掉，
        // 导致平均耗时算出来是负向漂移（分子清零、分母没清零）。
        std::lock_guard<std::mutex> lock(mutex_);
        submitted_.store(0, std::memory_order_relaxed);
        started_.store(0, std::memory_order_relaxed);
        completed_.store(0, std::memory_order_relaxed);
        failed_.store(0, std::memory_order_relaxed);
        cancelled_.store(0, std::memory_order_relaxed);
        rejected_.store(0, std::memory_order_relaxed);
        wait_ns_total_.store(0, std::memory_order_relaxed);
        run_ns_total_.store(0, std::memory_order_relaxed);
        for (auto& w : workers_) {
            w->reset_stats();
        }
    }
    events_.add(EventKind::Info, "计数器已清零");
}

// ---------------------------------------------------------------------------
//  worker_main：一个工作线程的一生
//
//  这是一个标准的“条件变量 + 队列”消费者循环，几乎所有的线程池都是这个骨架：
//
//      while (true) {
//          等条件成立（有任务 / 要关闭 / 要退休）
//          取任务 → 标记忙 → 解锁 → 执行 → 标记闲
//      }
//
//  【易错点】干活一定要在锁外。锁里只做“把任务从队列搬到手里”这一件小事，
//  否则一个慢任务会把整个线程池卡死。
// ---------------------------------------------------------------------------
void ThreadPool::worker_main(Worker& worker) {
    for (;;) {
        TaskPtr task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this, &worker]() {
                return worker.retire_requested.load(std::memory_order_acquire) ||
                       stopping_ || !queue_.empty();
            });

            if (worker.retire_requested.load(std::memory_order_acquire)) break;
            // 关闭时：如果选了"优雅"（drain_），要先把队列吃干净再走
            if (stopping_ && (!drain_ || queue_.empty())) break;

            task = queue_.pop();
            if (!task) continue;  // 被别的线程抢先拿走了，回去继续等

            worker.mark_busy(task->id);
            task->state.store(TaskState::Running, std::memory_order_release);
            task->started_ns.store(to_ns(Clock::now()), std::memory_order_relaxed);
            // 登记到 running_tasks_：任务一离开队列，cancel() 就只剩这张表能找回它
            running_tasks_[task->id] = task;
        }

        run_task(worker, task);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_tasks_.erase(task->id);
        }
    }
}

// run_task：真正执行任务体，并把统计信息记下来
void ThreadPool::run_task(Worker& worker, const TaskPtr& task) {
    // 取消检查 ①：从 pop 出来到真正开始执行之间，被 cancel() 抢先了
    if (task->cancel_requested.load(std::memory_order_acquire)) {
        task->state.store(TaskState::Cancelled, std::memory_order_release);
        task->finished_ns.store(to_ns(Clock::now()), std::memory_order_relaxed);
        cancelled_.fetch_add(1, std::memory_order_relaxed);
        worker.mark_idle();
        events_.add(EventKind::Cancel, "开始执行前发现已取消：" + task->name, task->id);
        return;
    }

    started_.fetch_add(1, std::memory_order_relaxed);
    // 等待时间 = 开始时刻 - 提交时刻
    const long long started_ns = task->started_ns.load(std::memory_order_relaxed);
    if (started_ns > 0) {
        const double waited_ns = ns_between(task->submitted_at, from_ns(started_ns));
        wait_ns_total_.fetch_add(static_cast<std::uint64_t>(waited_ns),
                                 std::memory_order_relaxed);
    }

    const bool sampled = should_log_task(task->id);
    if (sampled) {
        events_.add(EventKind::Start, "开始执行 " + task->name, task->id);
    }

    const TimePoint t0 = Clock::now();
    bool ok = true;
    try {
        task->work();
    } catch (...) {
        // 任务体抛异常绝不能让它逃出工作线程，否则整个池子少一个线程
        ok = false;
    }
    const TimePoint t1 = Clock::now();

    // 取消检查 ②：任务体大多是自己发现 token 被置起才 return 的。
    // 只认"确实被观察到"的取消（cancel_observed）：任务体压根没理取消请求、
    // 一路把活干完的，仍然算 Succeeded —— 谎报 Cancelled 比不取消更难查。
    const bool cancelled = task->cancel_observed.load(std::memory_order_relaxed);

    const double run_ms = ms_between(t0, t1);
    task->finished_ns.store(to_ns(t1), std::memory_order_relaxed);
    task->state.store(cancelled ? TaskState::Cancelled
                                : (ok ? TaskState::Succeeded : TaskState::Failed),
                      std::memory_order_release);
    worker.mark_idle();
    worker.note_executed(run_ms);

    run_ns_total_.fetch_add(static_cast<std::uint64_t>(run_ms * 1e6),
                            std::memory_order_relaxed);
    if (cancelled) {
        cancelled_.fetch_add(1, std::memory_order_relaxed);
        events_.add(EventKind::Cancel, "任务按取消请求中途收工：" + task->name, task->id);
    } else if (ok) {
        completed_.fetch_add(1, std::memory_order_relaxed);
        if (sampled) {
            events_.add(EventKind::Finish, "完成 " + task->name, task->id);
        }
    } else {
        failed_.fetch_add(1, std::memory_order_relaxed);
        events_.add(EventKind::Fail, "任务抛异常：" + task->name, task->id);
    }
}

// ---------------------------------------------------------------------------
//  monitor_main：每 100ms 巡检一次
// ---------------------------------------------------------------------------
void ThreadPool::monitor_main() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            monitor_cv_.wait_for(lock, kMonitorTick,
                                 [this]() { return monitor_stop_; });
            if (monitor_stop_) break;
            if (!running_ || stopping_) break;
            maybe_scale_locked();
        }

        // 【易错点】退休线程的 join 必须放在锁外。
        // 否则就是经典的“A 持锁等 B，B 等锁”死锁。
        std::vector<std::unique_ptr<Worker>> gone = take_retiring();
        for (auto& w : gone) {
            if (w && w->joinable()) w->join();
        }
    }
}

// 老化重建的最小间隔：有效等级每涨 kAgingRebuildLevelStep 级重建一次就够，
// 再密只是在重复做同样的堆化。下限取一个监控心跳（比心跳还密等于没限流）。
std::chrono::milliseconds ThreadPool::aging_rebuild_interval() const {
    const double rate = config_.aging_levels_per_second;  // 老化速度：级 / 秒
    if (rate <= 0.0) return std::chrono::hours(1);
    const double seconds = kAgingRebuildLevelStep / rate;
    const auto interval =
        std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000.0 + 0.5));
    return interval < kMonitorTick ? kMonitorTick : interval;
}

// maybe_scale_locked：扩容 / 缩容 / 老化重建，调用时必须已持有 mutex_
void ThreadPool::maybe_scale_locked() {
    if (!running_ || stopping_) return;

    const TimePoint now = Clock::now();
    const std::size_t pending = queue_.size();
    const std::size_t total = workers_.size();
    const std::size_t busy = busy_count_locked();
    const std::size_t idle = total - busy;

    // 老化：堆里的分数会随时间上涨，所以要定期重建顺序。
    // 但重建频率不该跟着监控心跳走——100ms 一跳时老化只走了 0.02 级，
    // 而一次重建是 O(n) 的整堆化。按“攒够 kAgingRebuildLevelStep 级”限流，
    // 老化速度调快时间隔自动缩短，调慢则重建得更省。
    if (queue_.aging().enabled && now - last_aging_rebuild_ >= aging_rebuild_interval()) {
        queue_.rebuild();
        last_aging_rebuild_ = now;
    }

    const double since_last_scale = ns_between(last_scale_at_, now) / 1e9;

    // ---- 扩容 ----
    if (pending >= config_.scale_up_threshold && total < config_.max_threads &&
        since_last_scale >= config_.scale_cooldown_seconds) {
        std::size_t add = std::min(config_.scale_up_step, config_.max_threads - total);
        add = std::max<std::size_t>(add, 1);
        spawn_workers_locked(add);
        last_scale_at_ = now;
        idle_since_ = TimePoint{};
        events_.add(EventKind::ScaleUp,
                    "扩容 +" + std::to_string(add) + " → " +
                        std::to_string(workers_.size()) + " 线程（积压 " +
                        std::to_string(pending) + "）");
        return;
    }

    // ---- 缩容 ----
    // 三个条件同时满足才缩：线程多于下限、空闲线程足够多、队列已清空
    const bool can_shrink = total > config_.min_threads &&
                            idle >= config_.idle_workers_to_shrink && pending == 0;
    if (!can_shrink) {
        idle_since_ = TimePoint{};  // 一旦不满足，空闲计时重来（滞回）
        return;
    }
    if (idle_since_ == TimePoint{}) idle_since_ = now;
    const double idle_for = ns_between(idle_since_, now) / 1e9;
    if (idle_for < config_.idle_timeout_seconds) return;
    if (since_last_scale < config_.scale_cooldown_seconds) return;

    std::size_t shrink = std::min(config_.scale_down_step, total - config_.min_threads);
    shrink = std::max<std::size_t>(shrink, 1);
    retire_workers_locked(shrink);
    last_scale_at_ = now;
    idle_since_ = TimePoint{};
    events_.add(EventKind::ScaleDown,
                "缩容 -" + std::to_string(shrink) + " → " +
                    std::to_string(workers_.size()) + " 线程（缩容前空闲 " +
                    std::to_string(idle) + "）");
}

std::size_t ThreadPool::busy_count_locked() const {
    std::size_t busy = 0;
    for (const auto& w : workers_) {
        if (w->busy()) ++busy;
    }
    return busy;
}

// spawn_workers_locked：创建 count 个线程并启动（已持锁）
void ThreadPool::spawn_workers_locked(std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        auto worker = std::unique_ptr<Worker>(new Worker(next_worker_index_++));
        // 第二个参数是析构兜底用的唤醒器：万一有线程没被 notify 就析构了，
        // 它能保证 join 不会永远等下去。
        worker->launch([this](Worker& w) { worker_main(w); },
                       [this]() { cv_.notify_all(); });
        workers_.push_back(std::move(worker));
    }
}

// retire_workers_locked：标记 count 个线程退休，把它们挪到 retiring_ 等待 join
void ThreadPool::retire_workers_locked(std::size_t count) {
    std::size_t retired = 0;

    // 第一轮：优先挑空闲线程。它们能立刻退出，不会打断正在执行的任务。
    for (auto it = workers_.begin(); it != workers_.end() && retired < count;) {
        Worker* w = it->get();
        if (!w->busy()) {
            w->retire_requested.store(true, std::memory_order_release);
            retiring_.push_back(std::move(*it));
            it = workers_.erase(it);
            ++retired;
        } else {
            ++it;
        }
    }

    // 第二轮：空闲线程不够（比如全是忙的），就从末尾挑，等它干完当前任务自然退出
    while (retired < count && !workers_.empty()) {
        auto it = workers_.end() - 1;
        (*it)->retire_requested.store(true, std::memory_order_release);
        retiring_.push_back(std::move(*it));
        workers_.erase(it);
        ++retired;
    }

    if (retired > 0) cv_.notify_all();  // 叫醒这些线程，让它们看到退休标志
}

std::vector<std::unique_ptr<Worker>> ThreadPool::take_retiring() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::unique_ptr<Worker>> out;
    out.swap(retiring_);
    return out;
}

}  // namespace tp
