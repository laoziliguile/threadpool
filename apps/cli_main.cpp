// ============================================================================
//  apps/cli_main.cpp —— 无界面演示程序（先跑这个，再打开图形界面）
//
//  作用：在没有窗口、没有显卡依赖的环境里验证线程池的真实行为：
//    1. 分批提交任务，观察队列积压如何触发扩容；
//    2. 实时打印“线程数 / 忙碌数 / 积压 / 完成数 / 平均等待”；
//    3. 负载结束后观察空闲缩容；
//    4. 打印最后的统计和最近的事件。
//
//  用法（在 build 目录下）：
//    bin\threadforge_cli.exe
//    bin\threadforge_cli.exe --tasks 800 --threads 2 --max 12 --workload cpu
//    bin\threadforge_cli.exe --tasks 500 --workload io --batch 10
//
//  注意：输出刻意全用英文/ASCII。Windows 控制台默认代码页是 936(GBK)，
//  源码里的中文直接 printf 出来会变乱码（这是本项目的一个真实踩坑记录）。
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "threadpool/pool.hpp"
#include "threadpool/simulator.hpp"

namespace {

using namespace std::chrono_literals;

struct Options {
    std::size_t tasks = 300;
    std::size_t min_threads = 2;
    std::size_t max_threads = 8;
    std::size_t batch = 25;          // 每批提交多少个任务
    int batch_pause_ms = 20;         // 每批之间的停顿，给扩容留出反应时间
    tp::WorkloadKind workload = tp::WorkloadKind::Mixed;
};

void print_usage() {
    std::printf(
        "ThreadForge CLI - headless thread pool demo\n"
        "  --tasks N        total tasks to submit (default 300)\n"
        "  --threads N      initial/min worker threads (default 2)\n"
        "  --max N          max worker threads for scaling (default 8)\n"
        "  --batch N        submit batch size (default 25)\n"
        "  --pause MS       pause between batches in ms (default 20)\n"
        "  --workload KIND  cpu | io | mixed (default mixed)\n"
        "  --help           show this message\n");
}

bool parse_workload(const char* text, tp::WorkloadKind& out) {
    if (std::strcmp(text, "cpu") == 0) { out = tp::WorkloadKind::Cpu; return true; }
    if (std::strcmp(text, "io") == 0) { out = tp::WorkloadKind::Io; return true; }
    if (std::strcmp(text, "mixed") == 0) { out = tp::WorkloadKind::Mixed; return true; }
    return false;
}

std::size_t parse_size(const char* text, std::size_t fallback) {
    if (!text || text[0] == '\0') return fallback;
    // 只接受"纯十进制数字"：
    //   - strtoul 认前导空白和负号，"-5" 会被回绕成一个巨大的无符号数；
    //   - 超出范围的输入只返回 ULONG_MAX 并置 errno，不查就会静默变成天文数字。
    if (!std::isdigit(static_cast<unsigned char>(text[0]))) return fallback;

    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE) return fallback;
    if (v > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return fallback;
    }
    return static_cast<std::size_t>(v);
}

// 解析命令行。返回 false 表示参数有问题（或用户要 --help）。
bool parse_args(int argc, char** argv, Options& opt, bool& want_help) {
    want_help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;

        if (arg == "--help" || arg == "-h") {
            want_help = true;
            return true;
        }
        if (arg == "--tasks") {
            if (!value) return false;
            opt.tasks = parse_size(value, opt.tasks);
            ++i;
        } else if (arg == "--threads") {
            if (!value) return false;
            opt.min_threads = parse_size(value, opt.min_threads);
            ++i;
        } else if (arg == "--max") {
            if (!value) return false;
            opt.max_threads = parse_size(value, opt.max_threads);
            ++i;
        } else if (arg == "--batch") {
            if (!value) return false;
            opt.batch = std::max<std::size_t>(1, parse_size(value, opt.batch));
            ++i;
        } else if (arg == "--pause") {
            if (!value) return false;
            opt.batch_pause_ms = static_cast<int>(parse_size(value, 20));
            ++i;
        } else if (arg == "--workload") {
            if (!value || !parse_workload(value, opt.workload)) return false;
            ++i;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (opt.min_threads < 1) opt.min_threads = 1;
    if (opt.max_threads < opt.min_threads) opt.max_threads = opt.min_threads;
    return true;
}

// 打印一行实时状态。max_threads_seen 用来记录整场运行中线程数的峰值。
void print_progress(const tp::PoolSnapshot& snap, std::size_t max_seen, double elapsed_s) {
    std::printf(
        "t=%5.2fs  threads=%2zu (peak %2zu) busy=%2zu queue=%3zu | done=%4llu fail=%llu "
        "rej=%llu | avg_wait=%7.2fms avg_run=%7.2fms\n",
        elapsed_s, snap.threads, max_seen, snap.busy_threads, snap.queue_size,
        static_cast<unsigned long long>(snap.completed),
        static_cast<unsigned long long>(snap.failed),
        static_cast<unsigned long long>(snap.rejected), snap.avg_wait_ms, snap.avg_run_ms);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    bool want_help = false;
    if (!parse_args(argc, argv, opt, want_help)) {
        print_usage();
        return 1;
    }
    if (want_help) {
        print_usage();
        return 0;
    }

    // ---- 1. 准备配置 ----
    tp::PoolConfig cfg;
    cfg.min_threads = opt.min_threads;
    cfg.max_threads = opt.max_threads;
    cfg.queue_capacity = 4096;
    cfg.scale_up_threshold = 8;      // 积压 8 个就扩容
    cfg.scale_up_step = 2;
    cfg.scale_cooldown_seconds = 0.2;
    cfg.scale_down_step = 2;
    cfg.idle_workers_to_shrink = 2;
    cfg.idle_timeout_seconds = 0.8;
    cfg.aging_enabled = true;

    std::printf("=== ThreadForge CLI ===\n");
    std::printf("tasks=%zu threads=%zu..%zu batch=%zu workload=%s\n\n",
                opt.tasks, opt.min_threads, opt.max_threads, opt.batch,
                tp::to_string(opt.workload));

    tp::ThreadPool pool(cfg);
    if (!pool.start()) {
        std::fprintf(stderr, "pool failed to start (invalid config)\n");
        return 2;
    }

    tp::WorkloadFactory factory(20251029u);
    const auto begin = tp::Clock::now();
    std::size_t submitted = 0;

    // ---- 2. 分批提交：每批之后停一下，让监控线程有机会观察到积压 ----
    while (submitted < opt.tasks) {
        const std::size_t n = std::min(opt.batch, opt.tasks - submitted);
        for (std::size_t i = 0; i < n; ++i) {
            const tp::Priority prio = factory.random_priority();
            const std::string name = std::string(tp::to_string(opt.workload)) + "-" +
                                     std::to_string(submitted + i);
            if (!pool.submit(name, prio, factory.make(opt.workload, 1.0))) {
                // 队列满会走到这里：这是"背压"，不是错误
                std::printf("  [backpressure] queue full, task rejected\n");
            }
        }
        submitted += n;
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.batch_pause_ms));
    }

    // ---- 3. 轮询直到全部跑完（或超时）----
    std::size_t peak_threads = 0;
    const auto deadline = tp::Clock::now() + 60s;
    auto last_print = tp::Clock::now();
    while (tp::Clock::now() < deadline) {
        const tp::PoolSnapshot snap = pool.snapshot();
        peak_threads = std::max(peak_threads, snap.threads);

        if (tp::Clock::now() - last_print >= 250ms) {
            const double elapsed = std::chrono::duration<double>(tp::Clock::now() - begin).count();
            print_progress(snap, peak_threads, elapsed);
            last_print = tp::Clock::now();
        }
        // 只把"真的执行过"的任务算进完成数：rejected 是被背压挡在门外、
        // 从未进过队列的任务，把它算成"已完成"会让 CLI 在还有任务没跑时就宣布结束。
        const std::uint64_t done = snap.completed + snap.failed + snap.cancelled;
        if (done + snap.rejected >= opt.tasks) break;
        std::this_thread::sleep_for(20ms);
    }

    // ---- 4. 等缩容：负载消失后，线程应该慢慢退回 min_threads ----
    std::printf("\nworkload finished, waiting for scale-down...\n");
    const auto shrink_deadline = tp::Clock::now() + 10s;
    while (tp::Clock::now() < shrink_deadline) {
        const tp::PoolSnapshot snap = pool.snapshot();
        if (snap.threads <= opt.min_threads && snap.queue_size == 0) break;
        std::this_thread::sleep_for(100ms);
    }

    const tp::PoolSnapshot final_snap = pool.snapshot();
    const double total_s = std::chrono::duration<double>(tp::Clock::now() - begin).count();

    // ---- 5. 最终报告 ----
    std::printf("\n=== final report ===\n");
    std::printf("elapsed        : %.2fs\n", total_s);
    std::printf("threads now    : %zu (peak %zu, range %zu..%zu)\n",
                final_snap.threads, peak_threads, opt.min_threads, opt.max_threads);
    std::printf("submitted      : %llu\n",
                static_cast<unsigned long long>(final_snap.submitted));
    std::printf("completed      : %llu\n",
                static_cast<unsigned long long>(final_snap.completed));
    std::printf("failed         : %llu\n",
                static_cast<unsigned long long>(final_snap.failed));
    std::printf("cancelled      : %llu\n",
                static_cast<unsigned long long>(final_snap.cancelled));
    std::printf("rejected       : %llu\n",
                static_cast<unsigned long long>(final_snap.rejected));
    std::printf("avg wait       : %.2f ms\n", final_snap.avg_wait_ms);
    std::printf("avg run        : %.2f ms\n", final_snap.avg_run_ms);

    const double throughput = (total_s > 0.0)
                                  ? static_cast<double>(final_snap.completed) / total_s
                                  : 0.0;
    std::printf("throughput     : %.1f tasks/s\n", throughput);

    // ---- 6. 事件日志尾部：这是界面上滚动条里显示的内容 ----
    std::printf("\n=== recent events (last 12) ===\n");
    const std::vector<tp::PoolEvent> events = pool.events().tail(12);
    for (const auto& ev : events) {
        std::printf("  [%-9s] %s\n", tp::to_string(ev.kind), ev.text.c_str());
    }

    // ---- 7. 结论：被拒绝的任务是"没跑成"，不能算进"跑完了" ----
    const std::uint64_t done = final_snap.completed + final_snap.failed + final_snap.cancelled;
    const std::uint64_t accounted = done + final_snap.rejected;
    int exit_code = 0;
    if (final_snap.rejected > 0) {
        std::printf("\n[warning] %llu task(s) were rejected by backpressure and never ran;\n"
                    "          raise --max / queue_capacity, or lower the submit rate\n",
                    static_cast<unsigned long long>(final_snap.rejected));
        exit_code = 1;
    }
    if (accounted < opt.tasks) {
        std::printf("\n[warning] only %llu of %zu task(s) were accounted for (60s timeout)\n",
                    static_cast<unsigned long long>(accounted), opt.tasks);
        exit_code = 1;
    }
    if (final_snap.failed > 0) {
        std::printf("\n[note] %llu task(s) threw; the pool isolated them (see failed counter)\n",
                    static_cast<unsigned long long>(final_snap.failed));
    }
    if (exit_code == 0) {
        std::printf("\nresult         : all %zu task(s) ran (completed %llu)\n", opt.tasks,
                    static_cast<unsigned long long>(final_snap.completed));
    }

    pool.shutdown(true);
    std::printf("\nshutdown done. bye.\n");
    return exit_code;
}
