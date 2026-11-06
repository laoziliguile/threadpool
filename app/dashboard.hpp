#pragma once
// ============================================================================
//  app/dashboard.hpp —— 仪表盘的"状态"与"绘制"
//
//  界面代码的一个基本原则：把【数据】和【画法】分开。
//  Dashboard 持有界面自己的状态（滑块值、曲线历史、缓存），
//  线程池的状态则永远通过 pool.snapshot() 每帧取一份值拷贝，绝不直接摸内部。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

#include "threadpool/pool.hpp"
#include "threadpool/simulator.hpp"

namespace app {

class Dashboard {
public:
    Dashboard();

    // 每帧调用一次
    void draw(tp::ThreadPool& pool);

    // 演示模式（命令行参数 --demo）：一启动就开持续压测，适合截图和给别人演示
    void enable_demo();

private:
    // ---- 各块界面 ----
    void draw_top_bar(tp::ThreadPool& pool, const tp::PoolSnapshot& snap);
    void draw_config_panel(tp::ThreadPool& pool, const tp::PoolSnapshot& snap);
    void draw_metrics(const tp::PoolSnapshot& snap);
    void draw_thread_grid(const tp::PoolSnapshot& snap);
    void draw_charts(const tp::PoolSnapshot& snap);
    void draw_tables(const tp::PoolSnapshot& snap);

    // ---- 数据流 ----
    void handle_load_generator(tp::ThreadPool& pool, double dt);
    void sample_history(const tp::PoolSnapshot& snap, double now);
    void submit_one(tp::ThreadPool& pool);
    void refresh_events(tp::ThreadPool& pool, double now);

    // ---- 界面自己的状态 ----
    tp::PoolConfig cfg_;              // 面板上正在编辑的配置
    tp::WorkloadFactory factory_;     // 造任务用
    bool live_apply_ = true;          // 滑块改动是否立刻生效
    bool full_trace_ = false;         // 任务级事件全量记录（默认 1/32 采样）
    // 配置刚被应用过 → 下一帧从 snap.config 回读"线程池真正生效的值"。
    // update_config() 会 sanitize（比如 max 被抬到 min、阈值被夹进容量），
    // 不回读的话面板显示的数字和实际线程数会对不上，用户改什么都像没生效。
    bool resync_config_ = false;

    int workload_kind_ = 2;           // 0=Cpu 1=Io 2=Mixed
    int priority_mode_ = 0;           // 0=随机，1..4 = 低/普通/高/紧急
    float intensity_ = 1.0f;
    int batch_size_ = 20;
    bool sustain_ = false;
    float rate_per_second_ = 60.0f;
    double submit_accumulator_ = 0.0;
    std::uint64_t submitted_by_ui_ = 0;

    // ---- 曲线历史（每 0.15s 采样一次，保留 240 点 ≈ 36 秒）----
    // 用 vector 而不是 deque：ImGui::PlotLines 只接受连续数组，
    // deque 每帧都得拷一份，vector 可以直接把 data() 交出去。
    static constexpr std::size_t kHistoryPoints = 240;
    std::vector<float> hist_threads_;
    std::vector<float> hist_queue_;
    std::vector<float> hist_wait_;
    std::vector<float> hist_throughput_;
    double last_sample_at_ = 0.0;
    std::uint64_t last_completed_ = 0;
    float throughput_ = 0.0f;

    // ---- 事件缓存：没必要每帧都去加锁拷 200 条字符串 ----
    std::vector<tp::PoolEvent> events_cache_;
    std::uint64_t events_total_ = 0;    // 累计产生过多少条事件
    std::uint64_t events_dropped_ = 0;  // 因为环形缓冲满而被覆盖掉多少条
    double last_events_at_ = -1.0;
    // 缓存里最新一条事件的时间：只要它变了，说明真的来了新事件，下一帧滚到底
    // （不能用 size() 判断：tail(200) 在满了以后长度一直是 200）
    tp::TimePoint last_event_at_{};
    bool scroll_events_ = false;
};

}  // namespace app
