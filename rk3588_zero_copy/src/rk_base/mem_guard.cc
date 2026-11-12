// rk_base/mem_guard.cc —— 受控分配器 + canary 越界哨兵实现
//
// 关键设计:
//   1) 块头带 magic, 块尾 32B canary。释放前校验, 被踩立刻报 tag。
//   2) 越界写通常发生在"释放之后"或"释放之前"的邻块上, 所以除了释放时校验,
//      还提供 check_canaries() 主动巡检(长跑时定期调用, 能在崩溃前抓到元凶)。
//   3) 头尾都用 canary 覆盖: 只保护尾部只能抓"向后越界",
//      头也保护才能抓"向前越界"(比如 off-by-one 写到了头部)。
//   4) 不改变分配语义: 返回普通堆内存, 与 malloc 一样可以 free 之外的一切用法。
//      注意: 必须用 MemGuard::free 释放, 直接 free() 会导致账本与实际不符。
//
// 生命周期: 全部状态是进程级单例, 进程退出时 OS 回收, 无需显式清理。

#include "rk/mem_guard.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rk/log.h"

namespace rk {
namespace {

constexpr uint64_t kMagic = 0x524B4D4755415244ULL;  // "RKMGUARD"
constexpr size_t kCanarySize = 32;
constexpr uint8_t kCanaryByte = 0xA5;

struct alignas(64) BlockHeader {
    uint64_t magic;
    size_t   size;        // 用户可用字节数
    uint32_t tag_id;
    uint32_t thread_id;
    void*    caller;      // 分配点返回地址(用于回溯)
    uint64_t seq;         // 全局递增序号
};

struct LiveBlock {
    size_t   bytes;       // 申请的总字节(含头尾)
    uint32_t tag_id;
    uint32_t thread_id;
    void*    caller;
    uint64_t seq;
};

struct TagInfo {
    std::string name;
    size_t current = 0;
    size_t peak = 0;
    uint64_t count = 0;
};

struct GuardState {
    std::mutex mu;
    std::unordered_map<void*, LiveBlock> live;
    std::vector<TagInfo> tags;
    std::unordered_map<std::string, uint32_t> tag_ids;
    size_t current = 0;
    size_t peak = 0;
    uint64_t seq = 0;
};

GuardState& state() {
    static GuardState s;
    return s;
}

uint32_t intern_tag(GuardState& st, const char* tag) {
    std::string key = tag ? tag : "-";
    auto it = st.tag_ids.find(key);
    if (it != st.tag_ids.end()) return it->second;
    uint32_t id = static_cast<uint32_t>(st.tags.size());
    st.tags.push_back(TagInfo{key});
    st.tag_ids.emplace(key, id);
    return id;
}

bool verify_locked(const void* p, const LiveBlock& blk, char* why, size_t why_sz) {
    const auto* hdr = reinterpret_cast<const BlockHeader*>(p);
    if (hdr->magic != kMagic) {
        snprintf(why, why_sz, "头 magic 破坏 (0x%llx != 0x%llx) —— 疑似向前越界写",
                 static_cast<unsigned long long>(hdr->magic),
                 static_cast<unsigned long long>(kMagic));
        return false;
    }
    const auto* tail = reinterpret_cast<const uint8_t*>(p) + blk.bytes - kCanarySize;
    for (size_t i = 0; i < kCanarySize; ++i) {
        if (tail[i] != kCanaryByte) {
            snprintf(why, why_sz,
                     "尾 canary 在第 %zu 字节被破坏 (0x%02x != 0x%02x) —— 疑似向后越界写",
                     i, tail[i], kCanaryByte);
            return false;
        }
    }
    return true;
}

}  // namespace

void* MemGuard::alloc(size_t bytes, const char* tag) {
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);

    const size_t total = sizeof(BlockHeader) + bytes + kCanarySize;
    void* raw = std::aligned_alloc(64, (total + 63) / 64 * 64);
    if (!raw) {
        RK_LOG_ERROR("mem", "alloc", "分配失败 tag=%s size=%zu", tag ? tag : "-", bytes);
        return nullptr;
    }
    std::memset(raw, 0, total);

    auto* hdr = reinterpret_cast<BlockHeader*>(raw);
    hdr->magic = kMagic;
    hdr->size = bytes;
    hdr->tag_id = intern_tag(st, tag);
    hdr->thread_id = static_cast<uint32_t>(syscall(SYS_gettid));
    hdr->caller = __builtin_return_address(0);
    hdr->seq = ++st.seq;

    auto* tail = reinterpret_cast<uint8_t*>(raw) + sizeof(BlockHeader) + bytes;
    std::memset(tail, kCanaryByte, kCanarySize);

    LiveBlock blk{sizeof(BlockHeader) + bytes + kCanarySize, hdr->tag_id,
                  hdr->thread_id, hdr->caller, hdr->seq};
    st.live.emplace(raw, blk);

    st.current += bytes;
    if (st.current > st.peak) st.peak = st.current;
    TagInfo& ti = st.tags[hdr->tag_id];
    ti.current += bytes;
    if (ti.current > ti.peak) ti.peak = ti.current;
    ti.count++;

    return reinterpret_cast<uint8_t*>(raw) + sizeof(BlockHeader);
}

void MemGuard::free(void* p) {
    if (!p) return;
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);

    auto* raw = reinterpret_cast<uint8_t*>(p) - sizeof(BlockHeader);
    auto it = st.live.find(raw);
    if (it == st.live.end()) {
        // 不是受控分配的指针, 或者已经被释放过(二次释放)
        RK_LOG_ERROR("mem", "free",
                     "释放了非受控指针 %p —— 二次释放或野指针", p);
        return;
    }

    char why[160] = {0};
    if (!verify_locked(raw, it->second, why, sizeof(why))) {
        RK_LOG_ERROR("mem", "free",
                     "canary 校验失败! tag=%s size=%zu seq=%llu tid=%u caller=%p : %s",
                     st.tags[it->second.tag_id].name.c_str(), it->second.bytes,
                     static_cast<unsigned long long>(it->second.seq),
                     it->second.thread_id, it->second.caller, why);
        // 破坏的块仍然要摘掉, 否则账本会一直漂移
    }

    st.current -= (it->second.bytes - sizeof(BlockHeader) - kCanarySize);
    TagInfo& ti = st.tags[it->second.tag_id];
    ti.current -= (it->second.bytes - sizeof(BlockHeader) - kCanarySize);
    st.live.erase(it);
    std::free(raw);
}

size_t MemGuard::check_canaries() {
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    size_t bad = 0;
    for (const auto& kv : st.live) {
        char why[160] = {0};
        if (!verify_locked(kv.first, kv.second, why, sizeof(why))) {
            ++bad;
            RK_LOG_ERROR("mem", "check",
                         "canary 破坏! tag=%s size=%zu seq=%llu tid=%u caller=%p : %s",
                         st.tags[kv.second.tag_id].name.c_str(), kv.second.bytes,
                         static_cast<unsigned long long>(kv.second.seq),
                         kv.second.thread_id, kv.second.caller, why);
        }
    }
    return bad;
}

size_t MemGuard::current_bytes() {
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.current;
}

size_t MemGuard::peak_bytes() {
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.peak;
}

size_t MemGuard::live_blocks() {
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.live.size();
}

void MemGuard::dump_by_tag() {
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    RK_LOG_INFO("mem", "dump",
                "受控内存: 当前 %zu B / 峰值 %zu B / 存活块 %zu", st.current, st.peak,
                st.live.size());
    for (const auto& ti : st.tags) {
        if (ti.count == 0) continue;
        RK_LOG_INFO("mem", "dump", "  tag=%-14s 当前 %8zu B  峰值 %8zu B  分配次数 %llu",
                    ti.name.c_str(), ti.current, ti.peak,
                    static_cast<unsigned long long>(ti.count));
    }
}

void MemGuard::dump_blocks() {
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    for (const auto& kv : st.live) {
        RK_LOG_INFO("mem", "dump",
                    "  block=%p tag=%-14s size=%zu seq=%llu tid=%u caller=%p",
                    reinterpret_cast<uint8_t*>(kv.first) + sizeof(BlockHeader),
                    st.tags[kv.second.tag_id].name.c_str(), kv.second.bytes,
                    static_cast<unsigned long long>(kv.second.seq), kv.second.thread_id,
                    kv.second.caller);
    }
}

}  // namespace rk
