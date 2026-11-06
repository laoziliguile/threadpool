// tests/test_pool.cpp —— 线程池的并发行为测试
//
// 并发测试和普通测试最大的区别：结果不是"立刻就有"的，必须"等"。
// 所以每个用例都用 wait_until(条件, 超时) 而不是 sleep 固定的时间——
// 固定 sleep 在快的机器上浪费时间，在慢的机器上随机失败（这是新手的经典坑）。
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "threadpool/pool.hpp"

namespace {

using namespace std::chrono_literals;

template <typename Predicate>
bool wait_until(Predicate pred, std::chrono::milliseconds timeout) {
    const auto deadline = tp::Clock::now() + timeout;
    while (tp::Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();  // 最后再试一次，避免刚好在超时边界上错过
}

}  // namespace

TF_TEST(pool_runs_all_submitted_tasks) {
    tp::PoolConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 2;
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    std::atomic<int> counter{0};
    const int kTasks = 10;
    for (int i = 0; i < kTasks; ++i) {
        pool.submit("job-" + std::to_string(i), tp::Priority::Normal, [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    TF_CHECK(wait_until([&]() { return counter.load() == kTasks; }, 3000ms));
    TF_CHECK_EQ(pool.snapshot().completed, static_cast<std::uint64_t>(kTasks));
    pool.shutdown(true);
}

TF_TEST(pool_runs_higher_priority_first) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 1;            // 只有一个线程，调度顺序一目了然
    cfg.queue_capacity = 64;
    cfg.aging_enabled = false;      // 关掉老化，否则低优先级会追上来
    cfg.scale_up_threshold = 64;    // 让监控线程不要来捣乱

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    std::atomic<bool> gate{false};
    std::atomic<int> done{0};
    std::mutex order_mutex;
    std::vector<int> order;

    // 第一个任务把唯一的线程占住，后面的任务才会真的排进队列
    pool.submit("blocker", tp::Priority::Normal, [&gate]() {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    });
    TF_CHECK(wait_until([&]() { return pool.snapshot().busy_threads == 1; }, 2000ms));

    auto record = [&](int tag) {
        return [&order, &order_mutex, &done, tag]() {
            {
                std::lock_guard<std::mutex> lock(order_mutex);
                order.push_back(tag);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        };
    };
    pool.submit("low", tp::Priority::Low, record(1));
    pool.submit("high", tp::Priority::High, record(2));
    pool.submit("normal", tp::Priority::Normal, record(3));

    // submit() 返回时任务已经在队列里（push 是在池锁内完成的），
    // 而且唯一的工作线程还卡在 gate 上，所以不需要 sleep 等"入队完成"。
    gate.store(true, std::memory_order_release);

    TF_CHECK(wait_until([&]() { return done.load() == 3; }, 3000ms));
    pool.shutdown(true);

    TF_CHECK_EQ(order.size(), static_cast<std::size_t>(3));
    if (order.size() == 3) {
        TF_CHECK_EQ(order[0], 2);  // High
        TF_CHECK_EQ(order[1], 3);  // Normal
        TF_CHECK_EQ(order[2], 1);  // Low
    }
}

TF_TEST(pool_rejects_when_queue_is_full) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 1;
    cfg.queue_capacity = 2;         // 最多排 2 个
    cfg.aging_enabled = false;
    cfg.scale_up_threshold = 2;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    std::atomic<bool> gate{false};
    pool.submit("blocker", tp::Priority::Normal, [&gate]() {
        while (!gate.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    });
    TF_CHECK(wait_until([&]() { return pool.snapshot().busy_threads == 1; }, 2000ms));

    TF_CHECK(pool.submit("a", tp::Priority::Normal, []() {}).has_value());
    TF_CHECK(pool.submit("b", tp::Priority::Normal, []() {}).has_value());
    TF_CHECK(!pool.submit("c", tp::Priority::Normal, []() {}).has_value());  // 满了

    TF_CHECK_EQ(pool.snapshot().rejected, static_cast<std::uint64_t>(1));
    gate.store(true, std::memory_order_release);
    pool.shutdown(true);
}

TF_TEST(pool_cancel_pending_task) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 1;
    cfg.aging_enabled = false;
    cfg.scale_up_threshold = 64;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    std::atomic<bool> gate{false};
    std::atomic<int> victim_runs{0};
    pool.submit("blocker", tp::Priority::Normal, [&gate]() {
        while (!gate.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    });
    TF_CHECK(wait_until([&]() { return pool.snapshot().busy_threads == 1; }, 2000ms));

    const auto id = pool.submit("victim", tp::Priority::Normal,
                                [&victim_runs]() { victim_runs.fetch_add(1); });
    TF_CHECK(id.has_value());
    if (id) {
        TF_CHECK(pool.cancel(*id));          // 还在排队，能取消
        TF_CHECK(!pool.cancel(*id));         // 已经不在队列里了，第二次必然失败
    }

    gate.store(true, std::memory_order_release);
    // 等到"队列空 + 没有线程在忙"，而不是拍一个 50ms 的固定睡眠：
    // 队列空只说明任务被取走了，不代表 worker 真的跑完了。
    TF_CHECK(wait_until([&]() {
        const tp::PoolSnapshot s = pool.snapshot();
        return s.queue_size == 0 && s.busy_threads == 0;
    }, 2000ms));

    TF_CHECK_EQ(victim_runs.load(), 0);  // 被取消的任务不能被执行
    TF_CHECK_EQ(pool.snapshot().cancelled, static_cast<std::uint64_t>(1));
    pool.shutdown(true);
}

TF_TEST(pool_survives_task_exception) {
    tp::PoolConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 2;
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    pool.submit("boom", tp::Priority::Normal,
                []() { throw std::runtime_error("task failed on purpose"); });
    std::atomic<int> ok{0};
    pool.submit("after", tp::Priority::Normal, [&ok]() { ok.fetch_add(1); });

    TF_CHECK(wait_until([&]() { return ok.load() == 1; }, 3000ms));
    const tp::PoolSnapshot snap = pool.snapshot();
    TF_CHECK_EQ(snap.failed, static_cast<std::uint64_t>(1));
    TF_CHECK_EQ(snap.completed, static_cast<std::uint64_t>(1));
    TF_CHECK(pool.running());  // 一个任务抛异常不能把池子搞挂
    pool.shutdown(true);
}

TF_TEST(pool_scales_up_under_backlog) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 6;
    cfg.queue_capacity = 256;
    cfg.scale_up_threshold = 3;
    cfg.scale_up_step = 2;
    cfg.scale_cooldown_seconds = 0.05;
    cfg.idle_timeout_seconds = 30.0;  // 本用例不关心缩容
    cfg.idle_workers_to_shrink = 6;
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    for (int i = 0; i < 24; ++i) {
        pool.submit("io-" + std::to_string(i), tp::Priority::Normal,
                    []() { std::this_thread::sleep_for(40ms); });
    }

    TF_CHECK(wait_until([&]() { return pool.snapshot().threads > 1; }, 3000ms));
    TF_CHECK(wait_until(
        [&]() { return pool.snapshot().completed >= static_cast<std::uint64_t>(24); },
        8000ms));
    TF_CHECK(pool.snapshot().threads <= cfg.max_threads);
    pool.shutdown(true);
}

TF_TEST(pool_scales_down_when_idle) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 6;
    cfg.queue_capacity = 256;
    cfg.scale_up_threshold = 3;
    cfg.scale_up_step = 3;
    cfg.scale_down_step = 3;
    cfg.scale_cooldown_seconds = 0.05;
    cfg.idle_timeout_seconds = 0.3;
    cfg.idle_workers_to_shrink = 1;
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    for (int i = 0; i < 18; ++i) {
        pool.submit("io-" + std::to_string(i), tp::Priority::Normal,
                    []() { std::this_thread::sleep_for(20ms); });
    }
    TF_CHECK(wait_until([&]() { return pool.snapshot().threads > 1; }, 3000ms));
    TF_CHECK(wait_until(
        [&]() { return pool.snapshot().completed >= static_cast<std::uint64_t>(18); },
        8000ms));

    // 负载消失后，线程数应该慢慢退回下限
    TF_CHECK(wait_until(
        [&]() { return pool.snapshot().threads <= cfg.min_threads; }, 8000ms));
    pool.shutdown(true);
}

TF_TEST(pool_non_drain_shutdown_cancels_queue) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 1;
    cfg.queue_capacity = 64;
    cfg.aging_enabled = false;
    cfg.scale_up_threshold = 64;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    // 用一个"睡 150ms"的任务占住唯一的线程：测试期间它不会去抢后面的任务，
    // 从而让"排队 → 关闭时被取消"这件事可以被稳定地观察到。
    pool.submit("blocker", tp::Priority::Normal, []() { std::this_thread::sleep_for(150ms); });
    TF_CHECK(wait_until([&]() { return pool.snapshot().busy_threads == 1; }, 2000ms));

    for (int i = 0; i < 5; ++i) {
        pool.submit("queued-" + std::to_string(i), tp::Priority::Normal, []() {});
    }
    TF_CHECK_EQ(pool.snapshot().queue_size, static_cast<std::size_t>(5));

    pool.shutdown(false);  // 非优雅：排队任务全部取消

    TF_CHECK(!pool.running());
    const tp::PoolSnapshot snap = pool.snapshot();
    TF_CHECK_EQ(snap.cancelled, static_cast<std::uint64_t>(5));
    TF_CHECK_EQ(snap.queue_size, static_cast<std::size_t>(0));
}

TF_TEST(pool_update_config_grows_threads_immediately) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 4;
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());
    TF_CHECK_EQ(pool.snapshot().threads, static_cast<std::size_t>(1));

    tp::PoolConfig bigger = cfg;
    bigger.min_threads = 3;
    pool.update_config(bigger);

    TF_CHECK_EQ(pool.snapshot().threads, static_cast<std::size_t>(3));
    pool.shutdown(true);
}

// ---------------------------------------------------------------------------
//  协作式取消：任务体拿着 CancellationToken，轮询到自己被取消就收工
// ---------------------------------------------------------------------------
TF_TEST(pool_cancels_running_task_cooperatively) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 1;
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    std::atomic<bool> entered{false};
    std::atomic<bool> observed_cancel{false};
    std::atomic<int> ticks{0};

    const auto id = pool.submit("long-runner", tp::Priority::Normal,
                                [&](const tp::CancellationToken& token) {
        entered.store(true, std::memory_order_release);
        // 真实业务里这里是一次长时间的遍历/拷贝；每轮都问一声“还要不要继续”
        for (;;) {
            if (token.is_cancelled()) {
                observed_cancel.store(true, std::memory_order_release);
                return;
            }
            ticks.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(1ms);
        }
    });
    TF_CHECK(id.has_value());
    TF_CHECK(wait_until([&]() { return entered.load(std::memory_order_acquire); }, 2000ms));

    // 关键：任务已经在跑，cancel() 也必须返回 true（置标志，不强杀线程）
    TF_CHECK(pool.cancel(*id));
    TF_CHECK(wait_until([&]() { return observed_cancel.load(std::memory_order_acquire); }, 3000ms));
    TF_CHECK(wait_until([&]() { return pool.snapshot().busy_threads == 0; }, 3000ms));

    const tp::PoolSnapshot snap = pool.snapshot();
    TF_CHECK_EQ(snap.cancelled, static_cast<std::uint64_t>(1));
    TF_CHECK_EQ(snap.completed, static_cast<std::uint64_t>(0));  // 中途收工 ≠ 完成
    TF_CHECK(!pool.cancel(*id));  // 已经结束，再取消只能失败
    TF_CHECK(ticks.load() > 0);   // 确实跑了一会儿才被打断
    pool.shutdown(true);
}

// 取消一个已经跑完的任务必须返回 false，而不是踩空/重复计数
TF_TEST(pool_cancel_after_finish_returns_false) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 1;
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    std::atomic<bool> ran{false};
    const auto id = pool.submit("quick", tp::Priority::Normal,
                                [&]() { ran.store(true, std::memory_order_release); });
    TF_CHECK(id.has_value());
    TF_CHECK(wait_until([&]() { return ran.load(std::memory_order_acquire); }, 2000ms));
    TF_CHECK(wait_until([&]() { return pool.snapshot().busy_threads == 0; }, 2000ms));

    TF_CHECK(!pool.cancel(*id));
    TF_CHECK_EQ(pool.snapshot().completed, static_cast<std::uint64_t>(1));
    TF_CHECK_EQ(pool.snapshot().cancelled, static_cast<std::uint64_t>(0));
    pool.shutdown(true);
}

// 任务体压根不理取消请求、一路干完：终态必须是 Succeeded，不能谎报 Cancelled
TF_TEST(pool_task_ignoring_cancel_still_succeeds) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 1;
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    std::atomic<bool> entered{false};
    const auto id = pool.submit("stubborn", tp::Priority::Normal, [&]() {
        entered.store(true, std::memory_order_release);
        std::this_thread::sleep_for(60ms);  // 不轮询 token，就是不收工
    });
    TF_CHECK(id.has_value());
    TF_CHECK(wait_until([&]() { return entered.load(std::memory_order_acquire); }, 2000ms));
    TF_CHECK(pool.cancel(*id));  // 请求被接受（但只是"尽力而为"）
    TF_CHECK(wait_until([&]() { return pool.snapshot().busy_threads == 0; }, 3000ms));

    const tp::PoolSnapshot snap = pool.snapshot();
    TF_CHECK_EQ(snap.completed, static_cast<std::uint64_t>(1));  // 干完了就是成功
    TF_CHECK_EQ(snap.cancelled, static_cast<std::uint64_t>(0));
    pool.shutdown(true);
}

// Submit 事件必须在 worker 能看到这个任务之前就落进日志。
// 否则事件表里会出现"先完成、后提交"的倒挂：读日志的人会以为自己眼花了，
// 而真正的原因只是日志是在解锁之后才补记的。
TF_TEST(pool_submit_event_never_after_start) {
    tp::PoolConfig cfg;
    cfg.min_threads = 4;
    cfg.max_threads = 4;
    cfg.queue_capacity = 512;
    cfg.scale_up_threshold = 511;  // 别让监控线程扩容，省得日志里混进无关事件
    cfg.idle_workers_to_shrink = 4;
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(pool.start());

    // 任务级事件按 id 低 5 位采样（1/32），提交 200 个才能命中若干个
    int accepted = 0;
    for (int i = 0; i < 200; ++i) {
        const auto id = pool.submit("evt-" + std::to_string(i), tp::Priority::Normal,
                                    []() { std::this_thread::sleep_for(1ms); });
        if (id.has_value()) ++accepted;
    }
    TF_CHECK_EQ(accepted, 200);

    TF_CHECK(wait_until(
        [&]() {
            const tp::PoolSnapshot snap = pool.snapshot();
            return snap.completed + snap.failed == static_cast<std::uint64_t>(200);
        },
        10000ms));
    pool.shutdown(true);

    const std::vector<tp::PoolEvent> events = pool.events().tail(512);
    std::map<tp::TaskId, std::size_t> submitted_at;
    std::map<tp::TaskId, std::size_t> started_at;
    std::map<tp::TaskId, std::size_t> finished_at;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const tp::PoolEvent& ev = events[i];
        if (ev.task == 0) continue;  // 扩缩容、启停之类的"与具体任务无关"的事件
        if (ev.kind == tp::EventKind::Submit) {
            submitted_at[ev.task] = i;
        } else if (ev.kind == tp::EventKind::Start) {
            started_at[ev.task] = i;
        } else if (ev.kind == tp::EventKind::Finish) {
            finished_at[ev.task] = i;
        }
    }

    std::size_t checked = 0;
    for (const auto& entry : submitted_at) {
        const auto started = started_at.find(entry.first);
        const auto finished = finished_at.find(entry.first);
        if (started == started_at.end() || finished == finished_at.end()) continue;
        TF_CHECK(entry.second < started->second);      // 提交 早于 开始
        TF_CHECK(started->second < finished->second);  // 开始 早于 结束
        ++checked;
    }
    TF_CHECK(checked > 0);  // 一个都没对上，说明采样根本没生效，这用例等于没写
}

// 非法配置必须明确拒绝启动，而不是"偷偷夹紧、照常开跑"。
// 夹紧是为了让池子还能用，但不能因此把"你给的配置非法"这件事瞒下来。
TF_TEST(pool_rejects_illegal_config_on_start) {
    tp::PoolConfig cfg;
    cfg.min_threads = 8;
    cfg.max_threads = 2;  // max < min：非法
    cfg.aging_enabled = false;

    tp::ThreadPool pool(cfg);
    TF_CHECK(!pool.start());  // 原始请求非法 → 拒绝启动
    TF_CHECK(!pool.running());

    // 但内部那份生效配置是合法的（max 被夹到 min = 8），界面拿到的永远是合法值
    const tp::PoolSnapshot after = pool.snapshot();
    TF_CHECK_EQ(after.config.min_threads, static_cast<std::size_t>(8));
    TF_CHECK_EQ(after.config.max_threads, static_cast<std::size_t>(8));

    // 改回合法配置后，同一个池子能正常启动（不允许一次失败就把对象弄坏）
    cfg.max_threads = 16;
    pool.update_config(cfg);
    TF_CHECK(pool.start());
    TF_CHECK(pool.running());
    pool.shutdown(true);
    TF_CHECK(!pool.running());
}

// Worker 析构时线程可能正卡在条件变量上：只置标志而不 notify，join 就会永远等下去。
// 这条用例失败时的表现不是断言报错，而是整个测试进程卡住——所以 CMakeLists 里给
// tp_tests 设了 60 秒超时，把"卡死"变成一个明确的失败。
TF_TEST(worker_destructor_wakes_parked_thread) {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> parked{false};

    {
        tp::Worker worker(99);
        worker.launch(
            [&](tp::Worker& w) {
                std::unique_lock<std::mutex> lock(mutex);
                parked.store(true, std::memory_order_release);
                // 睡到"有人叫我退休"为止；没人 notify 就永远醒不过来
                cv.wait(lock,
                        [&]() { return w.retire_requested.load(std::memory_order_acquire); });
            },
            [&]() { cv.notify_all(); });

        TF_CHECK(wait_until([&]() { return parked.load(std::memory_order_acquire); }, 2000ms));
        // worker 在这个作用域结束时析构：它能顺利 join 返回，就是本用例的全部内容
    }
    TF_CHECK(true);
}

// 任务级事件默认 1/32 采样（见 kTaskEventSampleMask）：吞吐高时全量落盘会把环形
// 缓冲刷爆。排查单个任务时可以关掉采样，此时它的每一条事件都必须留下。
// 这里提交的第一个任务 id = 1，不是 32 的倍数——默认采样一定会漏掉它。
TF_TEST(pool_task_event_full_trace_logs_every_task) {
    tp::PoolConfig cfg;
    cfg.min_threads = 1;
    cfg.max_threads = 1;
    cfg.aging_enabled = false;
    tp::ThreadPool pool(cfg);

    TF_CHECK(!pool.task_event_full_trace());
    pool.set_task_event_full_trace(true);
    TF_CHECK(pool.task_event_full_trace());
    TF_CHECK(pool.start());

    const std::optional<tp::TaskId> id = pool.submit("trace-me", tp::Priority::Normal, []() {});
    TF_CHECK(id.has_value());
    if (id.has_value()) {
        TF_CHECK(wait_until([&pool]() { return pool.snapshot().completed == 1; }, 2000ms));
    }
    pool.shutdown(true);

    int submit_seen = 0;
    int start_seen = 0;
    int finish_seen = 0;
    if (id.has_value()) {
        for (const tp::PoolEvent& event : pool.events().tail(tp::kEventViewLimit)) {
            if (event.task != *id) continue;
            if (event.kind == tp::EventKind::Submit) ++submit_seen;
            if (event.kind == tp::EventKind::Start) ++start_seen;
            if (event.kind == tp::EventKind::Finish) ++finish_seen;
        }
    }
    TF_CHECK_EQ(submit_seen, 1);
    TF_CHECK_EQ(start_seen, 1);
    TF_CHECK_EQ(finish_seen, 1);
}
