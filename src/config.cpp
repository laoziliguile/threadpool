// config.cpp —— 配置校验与"夹紧"
//
// 新手常犯的错：在业务代码里到处 if (cfg.min_threads > cfg.max_threads) ...
// 正确做法是在入口处集中校验一次（validate），需要容错时再夹紧一次（sanitized）。
#include "threadpool/config.hpp"

#include <algorithm>
#include <string>

namespace tp {

std::vector<std::string> PoolConfig::validate() const {
    std::vector<std::string> errors;

    if (min_threads < 1) {
        errors.emplace_back("最小线程数必须 ≥ 1");
    }
    if (max_threads < min_threads) {
        errors.emplace_back("最大线程数不能小于最小线程数");
    }
    if (max_threads > 1024) {
        errors.emplace_back("最大线程数建议 ≤ 1024（再多就是压测工具了）");
    }
    if (queue_capacity < 1) {
        errors.emplace_back("队列容量必须 ≥ 1");
    }
    if (scale_up_threshold < 1) {
        errors.emplace_back("扩容阈值必须 ≥ 1");
    }
    if (scale_up_threshold > queue_capacity) {
        errors.emplace_back("扩容阈值不能大于队列容量（否则永远等不到扩容）");
    }
    if (scale_up_step < 1) {
        errors.emplace_back("每次扩容步长必须 ≥ 1");
    }
    if (scale_down_step < 1) {
        errors.emplace_back("每次缩容步长必须 ≥ 1");
    }
    if (scale_cooldown_seconds < 0.0) {
        errors.emplace_back("扩容冷却时间不能为负");
    }
    if (idle_timeout_seconds < 0.0) {
        errors.emplace_back("空闲回收时间不能为负");
    }
    if (aging_levels_per_second < 0.0) {
        errors.emplace_back("老化速度不能为负");
    }
    if (aging_max_bonus_levels < 0) {
        errors.emplace_back("老化加成上限不能为负");
    }
    if (idle_workers_to_shrink < 1) {
        errors.emplace_back("缩容所需的空闲线程数必须 ≥ 1");
    }
    return errors;
}

PoolConfig PoolConfig::sanitized() const {
    PoolConfig c = *this;

    c.min_threads = std::max<std::size_t>(1, std::min<std::size_t>(c.min_threads, 1024));
    c.max_threads = std::max<std::size_t>(c.min_threads, std::min<std::size_t>(c.max_threads, 1024));
    c.queue_capacity = std::max<std::size_t>(1, std::min<std::size_t>(c.queue_capacity, 1u << 20));

    c.scale_up_step = std::max<std::size_t>(1, std::min<std::size_t>(c.scale_up_step, c.max_threads));
    c.scale_down_step = std::max<std::size_t>(1, std::min<std::size_t>(c.scale_down_step, c.max_threads));

    c.scale_up_threshold = std::max<std::size_t>(1, std::min<std::size_t>(c.scale_up_threshold, c.queue_capacity));
    c.idle_workers_to_shrink = std::max<std::size_t>(1, std::min<std::size_t>(c.idle_workers_to_shrink, c.max_threads));

    c.scale_cooldown_seconds = std::max(0.0, std::min(c.scale_cooldown_seconds, 60.0));
    c.idle_timeout_seconds = std::max(0.0, std::min(c.idle_timeout_seconds, 600.0));
    c.aging_levels_per_second = std::max(0.0, std::min(c.aging_levels_per_second, 100.0));
    c.aging_max_bonus_levels = std::max(0, std::min(c.aging_max_bonus_levels, 100));
    return c;
}

}  // namespace tp
