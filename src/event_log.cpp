// event_log.cpp —— 环形缓冲实现
//
// 一个 512 条的定长数组 + 两个下标（head_ 写位置、size_ 已有多少条），
// 写满之后新的覆盖最旧的。额外开销是 O(1)，内存是固定的。
#include "threadpool/event_log.hpp"

#include <algorithm>
#include <utility>

namespace tp {

const char* to_string(EventKind kind) {
    switch (kind) {
        case EventKind::Submit: return "Submit";
        case EventKind::Start: return "Start";
        case EventKind::Finish: return "Finish";
        case EventKind::Fail: return "Fail";
        case EventKind::Cancel: return "Cancel";
        case EventKind::Reject: return "Reject";
        case EventKind::ScaleUp: return "ScaleUp";
        case EventKind::ScaleDown: return "ScaleDown";
        case EventKind::Info: return "Info";
        case EventKind::Warn: return "Warn";
    }
    return "Info";
}

const char* event_label_zh(EventKind kind) {
    switch (kind) {
        case EventKind::Submit: return "提交";
        case EventKind::Start: return "开始";
        case EventKind::Finish: return "完成";
        case EventKind::Fail: return "失败";
        case EventKind::Cancel: return "取消";
        case EventKind::Reject: return "拒绝";
        case EventKind::ScaleUp: return "扩容";
        case EventKind::ScaleDown: return "缩容";
        case EventKind::Info: return "信息";
        case EventKind::Warn: return "警告";
    }
    return "信息";
}

EventLog::EventLog(std::size_t capacity) : buffer_(capacity == 0 ? 1 : capacity) {}

void EventLog::add(EventKind kind, std::string text, TaskId task) {
    PoolEvent ev;
    ev.at = Clock::now();
    ev.kind = kind;
    ev.task = task;
    ev.text = std::move(text);

    std::lock_guard<std::mutex> lock(mutex_);
    if (size_ == buffer_.size()) {
        ++dropped_;  // 覆盖掉最旧的一条
    } else {
        ++size_;
    }
    buffer_[head_] = std::move(ev);
    head_ = (head_ + 1) % buffer_.size();
    total_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<PoolEvent> EventLog::tail(std::size_t n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t cap = buffer_.size();
    const std::size_t count = std::min(n, size_);
    std::vector<PoolEvent> out;
    out.reserve(count);
    // 最旧的一条在 head_ 往回数 count 步的位置
    const std::size_t start = (head_ + cap - count) % cap;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(buffer_[(start + i) % cap]);
    }
    return out;
}

void EventLog::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    size_ = 0;
    dropped_ = 0;
    // 累计计数也要一起归零，否则"清空"之后界面上的总数还在涨，
    // 看起来像是清空失败。
    total_.store(0, std::memory_order_relaxed);
}

std::uint64_t EventLog::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

}  // namespace tp
