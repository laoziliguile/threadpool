// app/dashboard.cpp —— 仪表盘的全部绘制逻辑
//
// 界面代码的结构建议：一个面板一个函数，函数里只有"摆控件"和"读数据"，
// 不要在里面写业务逻辑（提交任务、扩容这些都应该在线程池里）。
// 这样界面代码改来改去也不会把内核弄坏，这是本项目分层最直接的收益。
#include "app/dashboard.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <utility>

#include "app/theme.hpp"
#include "imgui.h"

namespace app {

namespace {

// 布局里的像素尺寸统一过这个函数，界面才会跟着系统 DPI 缩放等比放大
float sc(float value) { return value * ui_scale(); }

ImVec4 priority_color(tp::Priority p) {
    const Palette& pal = palette();
    switch (p) {
        case tp::Priority::Low: return pal.text_dim;
        case tp::Priority::Normal: return pal.text;
        case tp::Priority::High: return pal.warn;
        case tp::Priority::Critical: return pal.err;
    }
    return pal.text;
}

ImVec4 event_color(tp::EventKind kind) {
    const Palette& pal = palette();
    switch (kind) {
        case tp::EventKind::Submit: return pal.text_dim;
        case tp::EventKind::Start: return pal.text_dim;
        case tp::EventKind::Finish: return pal.ok;
        case tp::EventKind::Fail: return pal.err;
        case tp::EventKind::Cancel: return pal.warn;
        case tp::EventKind::Reject: return pal.err;
        case tp::EventKind::ScaleUp: return pal.accent;
        case tp::EventKind::ScaleDown: return pal.warn;
        case tp::EventKind::Info: return pal.text_dim;
        case tp::EventKind::Warn: return pal.warn;
    }
    return pal.text;
}

// 卡片：上面一行小字标题，下面一行大字数值
void metric_card(const char* label, const char* value, const ImVec4& color) {
    ImGui::BeginGroup();
    ImGui::TextColored(palette().text_dim, "%s", label);
    if (fonts().title) ImGui::PushFont(fonts().title);
    ImGui::TextColored(color, "%s", value);
    if (fonts().title) ImGui::PopFont();
    ImGui::EndGroup();
}

void section(const char* title) {
    ImGui::Spacing();
    ImGui::TextColored(palette().accent, "%s", title);
    ImGui::Separator();
}

void plot_series(const char* label, const std::vector<float>& data, float vmin, float vmax) {
    ImGui::TextColored(palette().text_dim, "%s", label);
    if (data.empty()) {
        ImGui::TextDisabled("等待数据…");
        return;
    }
    // 历史本来就是连续存放的，直接交给 ImGui，不再每帧拷一份 vector
    ImGui::PlotLines(label, data.data(), static_cast<int>(data.size()), 0, nullptr, vmin, vmax,
                     ImVec2(-FLT_MIN, sc(58.0f)));
}

}  // namespace

Dashboard::Dashboard() : factory_(20251029u) {
    hist_threads_.reserve(kHistoryPoints);
    hist_queue_.reserve(kHistoryPoints);
    hist_wait_.reserve(kHistoryPoints);
    hist_throughput_.reserve(kHistoryPoints);
}

void Dashboard::enable_demo() {
    workload_kind_ = 2;       // 混合负载：既有 CPU 忙等也有 IO 睡眠，扩缩容更好看
    priority_mode_ = 0;       // 随机优先级
    batch_size_ = 20;
    intensity_ = 1.0f;
    rate_per_second_ = 450.0f; // 每秒 450 个：超过 4 个线程的吞吐能力，队列会积压、扩容会触发
    sustain_ = true;
}

// ===========================================================================
//  入口：每帧调用
// ===========================================================================
void Dashboard::draw(tp::ThreadPool& pool) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    double dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0 || dt > 0.25) dt = 1.0 / 60.0;  // 窗口被拖动/最小化时 DeltaTime 会跳变

    handle_load_generator(pool, dt);

    const tp::PoolSnapshot snap = pool.snapshot();
    // 上一帧应用过配置 → 用线程池里的"生效值"覆盖面板，让 sanitize 的结果可见
    if (resync_config_) {
        cfg_ = snap.config;
        resync_config_ = false;
    }
    const double now = ImGui::GetTime();
    sample_history(snap, now);
    refresh_events(pool, now);

    draw_top_bar(pool, snap);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild("##left_panel", ImVec2(sc(356.0f), 0.0f), ImGuiChildFlags_None);
    draw_config_panel(pool, snap);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##right_panel", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    draw_metrics(snap);
    draw_thread_grid(snap);
    draw_charts(snap);
    draw_tables(snap);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::End();
}

// ===========================================================================
//  顶部栏：标题、状态、操作按钮
// ===========================================================================
void Dashboard::draw_top_bar(tp::ThreadPool& pool, const tp::PoolSnapshot& snap) {
    const Palette& pal = palette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.panel_bg);
    ImGui::BeginChild("##topbar", ImVec2(0.0f, sc(66.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(sc(16.0f), sc(10.0f)));
    ImGui::BeginGroup();
    if (fonts().title) ImGui::PushFont(fonts().title);
    ImGui::TextColored(pal.accent, "ThreadForge");
    if (fonts().title) ImGui::PopFont();
    ImGui::TextColored(pal.text_dim, "进阶线程池：动态扩缩容 · 优先级调度 · 可视化");
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, sc(24.0f));
    ImGui::BeginGroup();
    const bool running = snap.running;
    ImGui::TextColored(running ? pal.ok : pal.err, running ? "运行中" : "已停止");
    ImGui::TextColored(pal.text_dim, "运行 %.1fs   %.0f FPS", snap.uptime_seconds,
                       static_cast<double>(ImGui::GetIO().Framerate));
    ImGui::EndGroup();

    // 右侧按钮组
    const float buttons_width = sc(640.0f);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttons_width);
    ImGui::SetCursorPosY(sc(20.0f));

    const ImVec2 btn(sc(104.0f), sc(30.0f));
    if (!running) {
        if (ImGui::Button("启动线程池", btn)) {
            pool.update_config(cfg_);  // 让面板上编辑过的配置生效
            pool.start();
            resync_config_ = true;     // 下一帧回读 sanitize 后的真实值
        }
    } else {
        if (ImGui::Button("优雅关闭", btn)) pool.shutdown(true);
        ImGui::SameLine();
        if (ImGui::Button("立即停止", btn)) pool.shutdown(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("清零统计", btn)) pool.reset_counters();
    ImGui::SameLine();
    if (ImGui::Button("清空日志", btn)) pool.clear_events();
    ImGui::SameLine();
    if (ImGui::Button("重置配置", btn)) {
        cfg_ = tp::PoolConfig{};
        if (running) pool.update_config(cfg_);
        resync_config_ = true;
    }

    ImGui::EndChild();
}

// ===========================================================================
//  左侧：配置面板 + 负载发生器
// ===========================================================================
void Dashboard::draw_config_panel(tp::ThreadPool& pool, const tp::PoolSnapshot& snap) {
    const Palette& pal = palette();
    bool changed = false;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.panel_bg);
    ImGui::BeginChild("##config_box", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();

    // 压紧一点行距：左栏要放下十几项配置，不压缩的话小屏幕得滚半天
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sc(8.0f), sc(3.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sc(8.0f), sc(3.0f)));

    ImGui::TextColored(pal.accent, "线程池配置");
    ImGui::TextColored(pal.text_dim, "拖动滑块即时生效，线程池运行中也能改");

    section("线程与扩容");
    // 滑块的上下界互相联动、并且 AlwaysClamp：从源头上就滑不出非法配置
    // （max 不能小于 min、阈值不能超过容量……），sanitize() 也就不会在背后偷改用户的数。
    const int max_cur = static_cast<int>(cfg_.max_threads < 1 ? 1 : cfg_.max_threads);
    const int min_cur = static_cast<int>(cfg_.min_threads < 1 ? 1 : cfg_.min_threads);
    const int cap_cur = static_cast<int>(cfg_.queue_capacity < 1 ? 1 : cfg_.queue_capacity);
    const std::size_t span =
        cfg_.max_threads > cfg_.min_threads ? cfg_.max_threads - cfg_.min_threads : 1;

    int min_threads = static_cast<int>(cfg_.min_threads);
    if (ImGui::SliderInt("##min_threads", &min_threads, 1, max_cur, "最少线程  %d 个",
                         ImGuiSliderFlags_AlwaysClamp)) {
        cfg_.min_threads = static_cast<std::size_t>(std::max(1, min_threads));
        changed = true;
    }
    int max_threads = static_cast<int>(cfg_.max_threads);
    if (ImGui::SliderInt("##max_threads", &max_threads, min_cur, 64, "最多线程  %d 个",
                         ImGuiSliderFlags_AlwaysClamp)) {
        cfg_.max_threads = static_cast<std::size_t>(std::max(1, max_threads));
        changed = true;
    }
    int up_threshold = static_cast<int>(cfg_.scale_up_threshold);
    if (ImGui::SliderInt("##up_threshold", &up_threshold, 1, cap_cur, "积压触发扩容  %d 个",
                         ImGuiSliderFlags_AlwaysClamp)) {
        cfg_.scale_up_threshold = static_cast<std::size_t>(std::max(1, up_threshold));
        changed = true;
    }
    int up_step = static_cast<int>(cfg_.scale_up_step);
    if (ImGui::SliderInt("##up_step", &up_step, 1, static_cast<int>(span), "每次扩容  +%d 线程",
                         ImGuiSliderFlags_AlwaysClamp)) {
        cfg_.scale_up_step = static_cast<std::size_t>(std::max(1, up_step));
        changed = true;
    }
    float cooldown = static_cast<float>(cfg_.scale_cooldown_seconds);
    if (ImGui::SliderFloat("##cooldown", &cooldown, 0.0f, 3.0f, "扩容冷却  %.2f 秒")) {
        cfg_.scale_cooldown_seconds = static_cast<double>(cooldown);
        changed = true;
    }

    section("空闲缩容");
    int down_step = static_cast<int>(cfg_.scale_down_step);
    if (ImGui::SliderInt("##down_step", &down_step, 1, static_cast<int>(span), "每次缩容  -%d 线程",
                         ImGuiSliderFlags_AlwaysClamp)) {
        cfg_.scale_down_step = static_cast<std::size_t>(std::max(1, down_step));
        changed = true;
    }
    int idle_need = static_cast<int>(cfg_.idle_workers_to_shrink);
    if (ImGui::SliderInt("##idle_need", &idle_need, 1, max_cur, "空闲线程门槛  %d 个",
                         ImGuiSliderFlags_AlwaysClamp)) {
        cfg_.idle_workers_to_shrink = static_cast<std::size_t>(std::max(1, idle_need));
        changed = true;
    }
    float idle_timeout = static_cast<float>(cfg_.idle_timeout_seconds);
    if (ImGui::SliderFloat("##idle_timeout", &idle_timeout, 0.1f, 10.0f, "持续空闲  %.1f 秒后缩容")) {
        cfg_.idle_timeout_seconds = static_cast<double>(idle_timeout);
        changed = true;
    }

    section("队列与调度");
    // 容量下界跟着阈值走：容量小于阈值时，扩容条件永远凑不齐，等于把扩容关掉了
    const int cap_floor = std::max(16, static_cast<int>(cfg_.scale_up_threshold));
    int capacity = static_cast<int>(cfg_.queue_capacity);
    if (ImGui::SliderInt("##capacity", &capacity, cap_floor, 8192, "队列容量  %d 个",
                         ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
        cfg_.queue_capacity = static_cast<std::size_t>(std::max(cap_floor, capacity));
        changed = true;
    }
    if (ImGui::Checkbox("优先级老化（防止低优先级饿死）", &cfg_.aging_enabled)) changed = true;
    float aging_speed = static_cast<float>(cfg_.aging_levels_per_second);
    if (ImGui::SliderFloat("##aging_speed", &aging_speed, 0.0f, 3.0f, "老化速度  %.2f 级/秒")) {
        cfg_.aging_levels_per_second = static_cast<double>(aging_speed);
        changed = true;
    }
    int aging_cap = cfg_.aging_max_bonus_levels;
    if (ImGui::SliderInt("##aging_cap", &aging_cap, 0, 10, "老化加成上限  %d 级")) {
        cfg_.aging_max_bonus_levels = aging_cap;
        changed = true;
    }
    ImGui::Checkbox("改动立即生效", &live_apply_);

    section("事件日志");
    // 任务级事件默认 1/32 采样（见 kTaskEventSampleMask）：几千任务/秒时全量
    // 落盘会让环形缓冲几秒就被刷爆。要追某个具体任务时打开这个开关。
    if (ImGui::Checkbox("记录所有任务事件（关掉采样）", &full_trace_)) {
        pool.set_task_event_full_trace(full_trace_);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("默认每 32 个任务只记 1 条提交/开始/完成。\n"
                          "打开后每个任务的事件都写进环形缓冲，\n"
                          "高吞吐下几秒就会被覆盖——用来短时间抓现场。");
    }

    // 线程池真正生效的那份配置：sanitize 之后的值，和上面滑块可能不同
    ImGui::Spacing();
    ImGui::TextColored(pal.text_dim, "生效：线程 %d~%d · 阈值 %d · 容量 %d",
                       static_cast<int>(snap.config.min_threads),
                       static_cast<int>(snap.config.max_threads),
                       static_cast<int>(snap.config.scale_up_threshold),
                       static_cast<int>(snap.config.queue_capacity));

    const std::vector<std::string> errors = cfg_.validate();
    if (!errors.empty()) {
        ImGui::Spacing();
        for (const auto& err : errors) {
            ImGui::TextColored(pal.err, "配置错误：%s", err.c_str());
        }
    }
    if (changed && live_apply_ && snap.running) {
        pool.update_config(cfg_);
        resync_config_ = true;  // 让 sanitize 的结果在下一帧回到面板上
    }

    // ---- 负载发生器 ----
    section("负载发生器");
    const char* kinds[] = {"计算密集（CPU）", "IO 等待（sleep）", "混合负载"};
    ImGui::Combo("##kind", &workload_kind_, kinds, 3);
    const char* prios[] = {"随机优先级", "低", "普通", "高", "紧急"};
    ImGui::Combo("##prio", &priority_mode_, prios, 5);
    ImGui::SliderFloat("##intensity", &intensity_, 0.2f, 4.0f, "任务强度  %.1f 倍");
    ImGui::SliderInt("##batch", &batch_size_, 1, 200, "每批提交  %d 个");

    ImGui::BeginDisabled(!snap.running);
    if (ImGui::Button("提交一批任务", ImVec2(-FLT_MIN, sc(34.0f)))) {
        for (int i = 0; i < batch_size_; ++i) submit_one(pool);
    }

    ImGui::Spacing();
    ImGui::Checkbox("持续压测（按速率自动提交）", &sustain_);
    ImGui::SliderFloat("##rate", &rate_per_second_, 1.0f, 500.0f, "每秒提交  %.0f 个");
    ImGui::EndDisabled();

    if (!snap.running) {
        ImGui::TextColored(pal.warn, "线程池未启动：先点右上角“启动线程池”");
    }

    ImGui::PopStyleVar(2);
    ImGui::EndChild();
}

// ===========================================================================
//  指标卡片
// ===========================================================================
void Dashboard::draw_metrics(const tp::PoolSnapshot& snap) {
    const Palette& pal = palette();
    const float busy_ratio =
        snap.threads > 0 ? static_cast<float>(snap.busy_threads) / static_cast<float>(snap.threads)
                         : 0.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.panel_bg);
    ImGui::BeginChild("##metrics", ImVec2(0.0f, sc(96.0f)), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    char buf[96];
    if (ImGui::BeginTable("##metric_row", 6, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();

        std::snprintf(buf, sizeof(buf), "%zu / %zu", snap.busy_threads, snap.threads);
        ImGui::TableNextColumn();
        metric_card("活跃线程", buf, busy_ratio > 0.85f ? pal.warn : pal.accent);

        std::snprintf(buf, sizeof(buf), "%zu", snap.queue_size);
        ImGui::TableNextColumn();
        metric_card("队列积压", buf, snap.queue_size > 0 ? pal.warn : pal.text);

        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(snap.completed));
        ImGui::TableNextColumn();
        metric_card("已完成任务", buf, pal.ok);

        std::snprintf(buf, sizeof(buf), "%llu / %llu",
                      static_cast<unsigned long long>(snap.failed),
                      static_cast<unsigned long long>(snap.rejected));
        ImGui::TableNextColumn();
        metric_card("失败 / 拒绝", buf, (snap.failed + snap.rejected) > 0 ? pal.err : pal.text_dim);

        std::snprintf(buf, sizeof(buf), "%.1f ms", snap.avg_wait_ms);
        ImGui::TableNextColumn();
        metric_card("平均等待", buf, pal.text);

        std::snprintf(buf, sizeof(buf), "%.0f /s", static_cast<double>(throughput_));
        ImGui::TableNextColumn();
        metric_card("吞吐量", buf, pal.accent);

        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// ===========================================================================
//  线程网格：一个方块 = 一个线程
// ===========================================================================
void Dashboard::draw_thread_grid(const tp::PoolSnapshot& snap) {
    const Palette& pal = palette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.panel_bg);
    ImGui::BeginChild("##worker_grid", ImVec2(0.0f, sc(132.0f)), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    ImGui::TextColored(pal.text_dim, "工作线程 %zu 个（忙碌 %zu · 空闲 %zu · 退休中 %zu）",
                       snap.threads, snap.busy_threads, snap.idle_threads,
                       snap.retiring_threads);
    ImGui::SameLine();
    ImGui::TextColored(pal.busy, "  [忙]");
    ImGui::SameLine();
    ImGui::TextColored(pal.text, "[闲]");
    ImGui::SameLine();
    ImGui::TextColored(pal.retiring, "[退]");

    if (snap.workers.empty()) {
        ImGui::TextDisabled("线程池未启动，没有工作线程");
        ImGui::EndChild();
        return;
    }

    const float cell_w = sc(36.0f);
    const float cell_h = sc(22.0f);
    const float gap = sc(6.0f);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int per_row =
        std::max(1, static_cast<int>((avail + gap) / (cell_w + gap)));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    for (std::size_t i = 0; i < snap.workers.size(); ++i) {
        const tp::WorkerInfo& w = snap.workers[i];
        ImGui::PushID(static_cast<int>(w.index));

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec4 fill = w.retiring ? pal.retiring : (w.busy ? pal.busy : pal.idle);
        draw_list->AddRectFilled(pos, ImVec2(pos.x + cell_w, pos.y + cell_h), u32(fill), 5.0f);

        char label[16];
        std::snprintf(label, sizeof(label), "W%u", static_cast<unsigned>(w.index));
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        draw_list->AddText(
            ImVec2(pos.x + (cell_w - text_size.x) * 0.5f, pos.y + (cell_h - text_size.y) * 0.5f),
            u32(w.busy ? pal.window_bg : pal.text), label);

        ImGui::InvisibleButton("##cell", ImVec2(cell_w, cell_h));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("线程 W%u", static_cast<unsigned>(w.index));
            ImGui::Text("状态：%s", w.retiring ? "退休中" : (w.busy ? "忙碌" : "空闲"));
            ImGui::Text("当前任务：%llu",
                        static_cast<unsigned long long>(w.current_task));
            ImGui::Text("累计执行：%llu 个",
                        static_cast<unsigned long long>(w.executed));
            ImGui::Text("累计忙碌：%.1f ms", w.busy_ms);
            ImGui::EndTooltip();
        }
        ImGui::PopID();

        const bool row_end = ((i + 1) % static_cast<std::size_t>(per_row) == 0);
        if (!row_end && i + 1 < snap.workers.size()) ImGui::SameLine(0.0f, gap);
    }

    ImGui::EndChild();
}

// ===========================================================================
//  曲线：线程数 / 积压 / 等待 / 吞吐
// ===========================================================================
void Dashboard::draw_charts(const tp::PoolSnapshot& snap) {
    const Palette& pal = palette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.panel_bg);
    ImGui::BeginChild("##charts", ImVec2(0.0f, sc(206.0f)), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    const float max_threads_axis = std::max(4.0f, static_cast<float>(snap.config.max_threads) + 2.0f);
    if (ImGui::BeginTable("##charts_table", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        plot_series("活跃线程数", hist_threads_, 0.0f, max_threads_axis);
        ImGui::TableNextColumn();
        plot_series("队列积压", hist_queue_, 0.0f, FLT_MAX);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        plot_series("平均等待（ms）", hist_wait_, 0.0f, FLT_MAX);
        ImGui::TableNextColumn();
        plot_series("吞吐（任务/秒）", hist_throughput_, 0.0f, FLT_MAX);
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// ===========================================================================
//  底部：等待队列 + 事件日志
// ===========================================================================
void Dashboard::draw_tables(const tp::PoolSnapshot& snap) {
    const Palette& pal = palette();

    if (!ImGui::BeginTable("##bottom_table", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("##queue_col", ImGuiTableColumnFlags_WidthStretch, 0.56f);
    ImGui::TableSetupColumn("##event_col", ImGuiTableColumnFlags_WidthStretch, 0.44f);
    ImGui::TableNextRow();

    // ---- 左：等待队列 ----
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.panel_bg);
    ImGui::BeginChild("##queue_box", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();

    ImGui::TextColored(pal.accent, "等待队列");
    ImGui::SameLine();
    ImGui::TextColored(pal.text_dim, "（按调度顺序，显示前 %d 个）",
                       static_cast<int>(tp::kQueueViewLimit));
    ImGui::Separator();

    if (snap.queue_view.empty()) {
        ImGui::TextDisabled("队列为空。线程池很闲，或者已经全部跑完了。");
    } else if (ImGui::BeginTable("##queue_table", 5,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_ScrollY,
                                 ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, sc(52.0f));
        ImGui::TableSetupColumn("任务名", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("优先级", ImGuiTableColumnFlags_WidthFixed, sc(60.0f));
        ImGui::TableSetupColumn("有效优先级", ImGuiTableColumnFlags_WidthFixed, sc(84.0f));
        ImGui::TableSetupColumn("已等待", ImGuiTableColumnFlags_WidthFixed, sc(72.0f));
        ImGui::TableHeadersRow();

        for (const auto& task : snap.queue_view) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%llu", static_cast<unsigned long long>(task.id));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(task.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(priority_color(task.priority), "%s",
                               tp::priority_label_zh(task.priority));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", task.effective_level);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f ms", task.waited_ms);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // ---- 右：事件日志 ----
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.panel_bg);
    ImGui::BeginChild("##event_box", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();

    ImGui::TextColored(pal.accent, "事件日志");
    ImGui::SameLine();
    ImGui::TextColored(pal.text_dim, "（最近 %d 条 · 累计 %llu 条 · 已覆盖 %llu 条）",
                       static_cast<int>(tp::kEventViewLimit),
                       static_cast<unsigned long long>(events_total_),
                       static_cast<unsigned long long>(events_dropped_));
    ImGui::Separator();

    if (ImGui::BeginChild("##event_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
        const tp::TimePoint now = tp::Clock::now();
        for (const auto& ev : events_cache_) {
            const double ago = -tp::ms_between(ev.at, now) / 1000.0;
            ImGui::TextColored(event_color(ev.kind), "[%s]", tp::event_label_zh(ev.kind));
            ImGui::SameLine();
            ImGui::TextColored(pal.text_dim, "%+.1fs", ago);
            ImGui::SameLine();
            ImGui::TextUnformatted(ev.text.c_str());
        }
        // 有新事件时自动滚到底部，像控制台一样
        if (scroll_events_) {
            ImGui::SetScrollHereY(1.0f);
            scroll_events_ = false;
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::EndTable();
}

// ===========================================================================
//  数据流：压测提交 / 采样 / 事件缓存
// ===========================================================================
void Dashboard::handle_load_generator(tp::ThreadPool& pool, double dt) {
    if (!sustain_ || !pool.running()) return;

    submit_accumulator_ += dt * static_cast<double>(rate_per_second_);
    int count = static_cast<int>(submit_accumulator_);
    if (count <= 0) return;
    if (count > 200) count = 200;  // 防止界面卡了一下之后一次灌几千个
    submit_accumulator_ -= static_cast<double>(count);
    for (int i = 0; i < count; ++i) submit_one(pool);
}

void Dashboard::submit_one(tp::ThreadPool& pool) {
    const auto kind = static_cast<tp::WorkloadKind>(workload_kind_);
    const tp::Priority prio = (priority_mode_ == 0)
                                  ? factory_.random_priority()
                                  : tp::priority_from_slot(priority_mode_ - 1);

    char name[64];
    std::snprintf(name, sizeof(name), "%s-%llu", tp::to_string(kind),
                  static_cast<unsigned long long>(++submitted_by_ui_));
    pool.submit(name, prio, factory_.make(kind, static_cast<double>(intensity_)));
}

void Dashboard::sample_history(const tp::PoolSnapshot& snap, double now) {
    const double interval = 0.15;
    if (last_sample_at_ <= 0.0) {
        last_sample_at_ = now - interval;  // 第一帧立刻采一个点
        last_completed_ = snap.completed;
    }
    if (now - last_sample_at_ < interval) return;

    const double elapsed = std::max(1e-3, now - last_sample_at_);
    const std::uint64_t delta = (snap.completed >= last_completed_)
                                    ? snap.completed - last_completed_
                                    : 0;
    throughput_ = static_cast<float>(static_cast<double>(delta) / elapsed);

    auto push = [](std::vector<float>& series, float value) {
        if (series.size() < Dashboard::kHistoryPoints) {
            series.push_back(value);
            return;
        }
        // 窗口满了就整体左移一格。采样是每秒 6.7 次，这点搬移可以忽略，
        // 换来的是数据始终连续、画曲线时零拷贝。
        std::copy(series.begin() + 1, series.end(), series.begin());
        series.back() = value;
    };
    push(hist_threads_, static_cast<float>(snap.threads));
    push(hist_queue_, static_cast<float>(snap.queue_size));
    push(hist_wait_, static_cast<float>(snap.avg_wait_ms));
    push(hist_throughput_, throughput_);

    last_sample_at_ = now;
    last_completed_ = snap.completed;
}

void Dashboard::refresh_events(tp::ThreadPool& pool, double now) {
    if (last_events_at_ >= 0.0 && now - last_events_at_ < 0.2) return;
    events_cache_ = pool.events().tail(tp::kEventViewLimit);
    events_total_ = pool.events().total();
    events_dropped_ = pool.events().dropped();
    last_events_at_ = now;

    // 事件日志本身在锁内可见，这里只看“最新一条的时间”有没有变
    if (!events_cache_.empty() && events_cache_.back().at != last_event_at_) {
        last_event_at_ = events_cache_.back().at;
        scroll_events_ = true;
    }
}

}  // namespace app
