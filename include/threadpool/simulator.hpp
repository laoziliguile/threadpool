#pragma once
// ============================================================================
//  simulator.hpp —— 假任务生成器
//
//  线程池本身不关心任务干什么，但要“看见”负载才有意义。这里提供三种模拟负载：
//    · Cpu：纯计算（FNV 哈希循环），用来观察 CPU 打满时的扩容行为
//    · Io ：sleep，模拟等数据库/网络，用来观察“线程数远大于核数”也能提吞吐
//    · Mixed：随机混合上面两种
//  所有随机数在“生成任务时”就取好，任务是自包含的纯函数，
//  因此工作线程里不需要加锁，也不会有数据竞争（这点很重要，见第 10 课）。
// ============================================================================

#include <cstdint>
#include <functional>
#include <random>
#include <string>

#include "threadpool/task.hpp"

namespace tp {

enum class WorkloadKind {
    Cpu = 0,
    Io = 1,
    Mixed = 2,
};

const char* to_string(WorkloadKind kind);
const char* workload_label_zh(WorkloadKind kind);

class WorkloadFactory {
public:
    explicit WorkloadFactory(std::uint32_t seed = 20251029u);

    // 生成一个可执行的任务体。intensity 放大工作量（1.0 = 默认强度）。
    std::function<void()> make(WorkloadKind kind, double intensity = 1.0);

    Priority random_priority();
    void reseed(std::uint32_t seed);

private:
    std::mt19937 rng_;
};

}  // namespace tp
