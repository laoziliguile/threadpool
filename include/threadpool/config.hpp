#pragma once
// ============================================================================
//  config.hpp —— 线程池的全部可调参数
//
//  设计原则：所有“魔法数字”都必须能用一句话解释清楚，并且能被界面实时修改。
//  这个结构体是值语义的 POD 风格对象，复制成本极低，线程池内部存一份副本，
//  界面每帧读一份快照即可，不需要锁。
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

namespace tp {

struct PoolConfig {
    // ---- 线程数量 ----
    std::size_t min_threads = 4;    // 启动即创建，空闲也不销毁
    std::size_t max_threads = 12;   // 动态扩容上限（不要超过 CPU 核数太多）

    // ---- 队列 ----
    std::size_t queue_capacity = 2048;  // 队列满时 submit 直接拒绝（背压）

    // ---- 扩容规则 ----
    std::size_t scale_up_threshold = 8;  // 积压任务 ≥ 该值就认为“忙不过来”
    std::size_t scale_up_step = 2;       // 每次扩容新增几个线程
    double scale_cooldown_seconds = 0.3; // 两次扩容之间的最小间隔，防止抖动

    // ---- 缩容规则 ----
    std::size_t scale_down_step = 1;     // 每次缩容回收几个线程
    std::size_t idle_workers_to_shrink = 3;  // 空闲线程数达到多少才允许缩容
    double idle_timeout_seconds = 2.0;   // 持续空闲多久才回收（滞回，防抖动）

    // ---- 优先级老化（防止低优先级任务饿死）----
    bool aging_enabled = true;
    double aging_levels_per_second = 0.2;  // 每等待 1 秒，有效优先级涨 0.2 级
    int aging_max_bonus_levels = 3;        // 老化加成上限（级）

    // 返回所有不合法项的中文说明；空 vector 表示配置合法。
    // 之所以不抛异常：配置来自界面输入，用户改到一半时不希望程序崩。
    std::vector<std::string> validate() const;

    // 把非法值夹到合法范围（界面滑块使用）
    PoolConfig sanitized() const;
};

}  // namespace tp
