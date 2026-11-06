// simulator.cpp —— 造"看起来很真实"的假任务
//
// 关键设计：随机数在【生成任务时】就抽好，任务体只捕获几个普通变量。
// 这样工作线程执行任务时不需要碰 rng_，天然没有数据竞争。
// 也正因如此，WorkloadFactory 本身不是线程安全的——它只在提交任务的那一侧用。
#include "threadpool/simulator.hpp"

#include <chrono>
#include <thread>

namespace tp {

const char* to_string(WorkloadKind kind) {
    switch (kind) {
        case WorkloadKind::Cpu: return "Cpu";
        case WorkloadKind::Io: return "Io";
        case WorkloadKind::Mixed: return "Mixed";
    }
    return "Cpu";
}

const char* workload_label_zh(WorkloadKind kind) {
    switch (kind) {
        case WorkloadKind::Cpu: return "计算密集";
        case WorkloadKind::Io: return "IO 等待";
        case WorkloadKind::Mixed: return "混合";
    }
    return "计算密集";
}

WorkloadFactory::WorkloadFactory(std::uint32_t seed) : rng_(seed) {}

void WorkloadFactory::reseed(std::uint32_t seed) {
    rng_.seed(seed);
}

std::function<void()> WorkloadFactory::make(WorkloadKind kind, double intensity) {
    if (!(intensity > 0.0)) intensity = 1.0;  // 顺便挡住 NaN
    std::uniform_int_distribution<int> roll(0, 99);
    const int r = roll(rng_);

    WorkloadKind actual = kind;
    if (kind == WorkloadKind::Mixed) {
        actual = (r < 55) ? WorkloadKind::Cpu : WorkloadKind::Io;  // 混合负载偏计算一点
    }

    if (actual == WorkloadKind::Cpu) {
        // 2~19ms 的纯计算。用"跑到截止时间"而不是固定迭代次数，
        // 这样换台机器、换个优化等级，任务耗时依然大致可控。
        const double target_ms = (2.0 + static_cast<double>(r % 18)) * intensity;
        return [target_ms]() {
            const auto budget = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double, std::milli>(target_ms));
            const TimePoint deadline = Clock::now() + budget;
            std::uint64_t h = 1469598103934665603ull;  // FNV-1a 偏移基数
            while (Clock::now() < deadline) {
                for (int i = 0; i < 1024; ++i) {
                    h ^= static_cast<std::uint64_t>(i);
                    h *= 1099511628211ull;  // FNV 质数
                }
            }
            // volatile 写一次，防止编译器把整段计算当成死代码优化掉
            volatile std::uint64_t sink = h;
            (void)sink;
        };
    }

    // IO 型：睡一觉。注意 sleep 不占 CPU，所以线程数可以远超核数。
    const int sleep_ms = static_cast<int>((2.0 + static_cast<double>(r % 30)) * intensity);
    return [sleep_ms]() { std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms)); };
}

Priority WorkloadFactory::random_priority() {
    // 分布刻意做成"金字塔"：大部分任务普通优先级，紧急任务是少数，
    // 否则优先级队列和没有优先级没有区别。
    std::uniform_int_distribution<int> roll(0, 99);
    const int r = roll(rng_);
    if (r < 30) return Priority::Low;       // 30%
    if (r < 75) return Priority::Normal;    // 45%
    if (r < 95) return Priority::High;      // 20%
    return Priority::Critical;              // 5%
}

}  // namespace tp
