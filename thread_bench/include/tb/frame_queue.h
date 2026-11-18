// tb/frame_queue.h —— 公共帧队列(定长环形)
//
// ★ 为什么不用 std::deque
//   libstdc++ 的 deque 在 pop_front 的时候, 如果最前面的内存块空了会把整块
//   释放掉(deque::_M_pop_front_aux)。那就是【热循环里的 free】——
//   直接违反"内存平线"红线。所以自己实现定长环形, 循环里一个字节都不分配。
//
// ★ 丢帧策略: 满了丢【最旧】的, 生产者永不阻塞
//   边缘设备的真实场景是网络抖动/处理不及: 采集端不能因为下游算不过来就
//   停下来。反过来, 丢掉最旧的帧能保证"被处理的永远是最新画面",
//   端到端延迟不会随着积压线性增长(实测见 README)。
#ifndef TB_FRAME_QUEUE_H_
#define TB_FRAME_QUEUE_H_

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace tb {

class FrameQueue {
public:
    struct Item {
        uint32_t channel = 0;      // 来自第几路摄像头
        uint64_t seq = 0;          // 全局序号
        int64_t t_enqueue_ns = 0;  // 入队时刻(单调时钟 ns) —— e2e 延迟的起点
    };

    explicit FrameQueue(size_t capacity);

    // 生产者侧: 【永不阻塞】。队列满时丢最旧的一帧并计数, 然后入队。
    void push(const Item& it);

    // 消费者侧: 阻塞等待非空。
    // stop() 之后返回 false(不再吐出残余帧) —— 收尾时立刻停下, 不再处理积压。
    bool pop(Item* out);

    // 唤醒所有等待者并标记停止。
    void stop();

    size_t size() const;
    size_t capacity() const { return cap_; }
    uint64_t pushed() const;
    uint64_t dropped() const;

    // 统计复位(预热结束时调用, 把预热期的计数清掉)
    void reset_stats();

private:
    std::vector<Item> buf_;   // 启动时一次性预分配, 循环里零分配
    size_t cap_ = 0;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    uint64_t pushed_ = 0;
    uint64_t dropped_ = 0;

    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

}  // namespace tb

#endif  // TB_FRAME_QUEUE_H_
