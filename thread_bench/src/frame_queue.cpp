// tb/frame_queue.cpp —— 定长环形队列实现
#include "tb/frame_queue.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace tb {

FrameQueue::FrameQueue(size_t capacity) {
    // 容量必须 >= 1。cap == 1 的环算术也是对的:
    //   push 时 count_(1) == cap_(1) -> 丢最旧(head_ 前进, count_ 回到 0)
    //   -> 再写 tail_, count_ 回到 1。
    if (capacity < 1) capacity = 1;
    cap_ = capacity;
    buf_.resize(cap_);   // 一次性分配, 之后循环里零分配
}

void FrameQueue::push(const Item& it) {
    std::lock_guard<std::mutex> lk(m_);
    if (count_ == cap_) {
        // 队列满: 丢【最旧】的一帧。生产者绝不等待 —— 这是"防雪崩"的核心。
        head_ = (head_ + 1) % cap_;
        --count_;
        ++dropped_;
    }
    buf_[tail_] = it;
    tail_ = (tail_ + 1) % cap_;
    ++count_;
    ++pushed_;
    cv_.notify_one();
}

bool FrameQueue::pop(Item* out) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return stopped_ || count_ > 0; });
    if (stopped_) return false;   // 收尾: 残余帧不再处理
    *out = buf_[head_];
    head_ = (head_ + 1) % cap_;
    --count_;
    return true;
}

void FrameQueue::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stopped_ = true;
    }
    cv_.notify_all();
}

size_t FrameQueue::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return count_;
}

uint64_t FrameQueue::pushed() const {
    std::lock_guard<std::mutex> lk(m_);
    return pushed_;
}

uint64_t FrameQueue::dropped() const {
    std::lock_guard<std::mutex> lk(m_);
    return dropped_;
}

void FrameQueue::reset_stats() {
    std::lock_guard<std::mutex> lk(m_);
    pushed_ = 0;
    dropped_ = 0;
}

}  // namespace tb
