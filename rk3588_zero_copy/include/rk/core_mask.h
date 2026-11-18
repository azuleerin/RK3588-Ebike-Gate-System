// rk/core_mask.h —— NPU 核掩码(core mask)的解析与描述
//
// ★ RK3588 的 NPU 核掩码约定(rknn_set_core_mask 的入参)
//
//     1  = CORE0   2 = CORE1   4 = CORE2
//     3  = CORE0+CORE1        5 = CORE0+CORE2        6 = CORE1+CORE2
//     7  = 三核全开            0 = 自动(交给驱动)
//
// ★ 两种写法, 两种语义(必须分清, 否则会把实例绑到错的核上)
//
//   ① 【掩码写法】"0x3" / "3"  -> 直接就是掩码值
//        0x3 = CORE0+CORE1
//      这是 rknn_set_core_mask 的原生入参形式, 老脚本 --npu-core 1 用的就是它
//      (1 = CORE0, 不是 CORE1!)。
//
//   ② 【核序号写法】"0,1,2" / "1" / "core0,core1" -> 核的【序号】列表
//        "0"   -> CORE0   -> 掩码 0x1
//        "1"   -> CORE1   -> 掩码 0x2
//        "0,1" -> CORE0+CORE1 -> 掩码 0x3
//      新选项 --det-core / --ocr-core 用的是这一种(人读的序号)。
//
//   区分办法: 带 "0x" 前缀的按【掩码】解; 其余按【核序号列表】解。
//   为了不给人留坑, 每个选项解析完都会把 "原文 -> 掩码 -> 核列表" 打出来。
#ifndef RK_CORE_MASK_H_
#define RK_CORE_MASK_H_

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rk {

// 解析一个核规格字符串, 返回掩码; 失败返回 0(= 自动)。
//
//   "0x3"    -> 0x3 (掩码写法)
//   "0,1,2"  -> 0x7 (核序号写法)
//   "1"      -> 0x2 (核序号写法: CORE1)
//   "core0"  -> 0x1
//   ""/NULL  -> 0  (自动)
//   "9"      -> 0  (非法核号)
inline int parse_core_mask(const char* spec) {
    if (!spec || !*spec) return 0;

    // ---- 掩码写法: 0x 开头 ----
    if (spec[0] == '0' && (spec[1] == 'x' || spec[1] == 'X')) {
        char* end = nullptr;
        const long v = strtol(spec, &end, 16);
        if (!end || *end != '\0' || v <= 0 || v > 0x7) return 0;
        return static_cast<int>(v);
    }

    // ---- 核序号列表 ----
    int mask = 0;
    const char* p = spec;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == ';') ++p;
        if (!*p) break;

        int core = -1;
        if (strncmp(p, "core", 4) == 0) {
            p += 4;
            core = 0;
            while (*p >= '0' && *p <= '9') { core = core * 10 + (*p - '0'); ++p; }
        } else if (*p >= '0' && *p <= '9') {
            core = 0;
            while (*p >= '0' && *p <= '9') { core = core * 10 + (*p - '0'); ++p; }
        } else {
            return 0;   // 非法字符
        }
        if (core < 0 || core > 2) return 0;   // RK3588 只有 3 个 NPU 核
        mask |= (1 << core);

        while (*p == ' ' || *p == ',') ++p;
    }
    return mask;
}

// 把掩码翻译成人读的核列表, 例如 0x5 -> "Core0+Core2"。
// buf 至少 64 字节。
inline const char* core_mask_to_string(int mask, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return "";
    buf[0] = '\0';
    if (mask == 0) {
        snprintf(buf, buf_size, "自动");
        return buf;
    }
    size_t pos = 0;
    for (int c = 0; c < 3; ++c) {
        if (!(mask & (1 << c))) continue;
        const int n = snprintf(buf + pos, buf_size - pos, "%sCore%d",
                               pos ? "+" : "", c);
        if (n < 0) break;
        pos += static_cast<size_t>(n);
        if (pos >= buf_size) break;
    }
    return buf;
}

// 把掩码拆成【核序号】列表(升序)。
//   0x5 -> {0, 2}   (Core0 + Core2)
//   0   -> {}       (自动, 没有指定核)
// NpuPool 用它决定"每颗核起几个 worker"。
inline std::vector<int> core_mask_to_indices(int mask) {
    std::vector<int> v;
    for (int c = 0; c < 3; ++c)
        if (mask & (1 << c)) v.push_back(c);
    return v;
}

// ---------------------------------------------------------------------------
// ★★ 送进驱动【之前】的掩码消毒 —— 0x7 三核全开是禁用的
//
//   为什么要消毒: 掩码 0x7 = RKNN_NPU_CORE_0_1_2, 语义是
//   "把【一次】推理拆到 3 颗核上跑", 不是"3 颗核各跑一份"。
//   独立压测台(thread_bench)实测: 峰值吞吐 45~51 fps vs 均匀分核的 134 fps
//   (只有 1/2.6), e2e P99 723~913 ms vs 38 ms(高 25 倍), 而且线程数越多越差。
//
//   ★ 但要分清两种"0,1,2":
//     · "--det-core 0,1,2" 给【池子】 —— 合法, 意思是"3 个 worker, 每个独占
//       一颗核"(池子绑的是 1/2/4, 永远不会把 0x7 交给驱动)。
//     · 把 0x7 直接交给 rknn_set_core_mask —— 禁用, 就是上面那个坏语义。
//   所以禁令要落在【驱动边界】上, 而不是 CLI 上 —— 否则会误伤上面那种合法用法。
//
//   行为: 发现 0x7 就退回集合里最低的那颗核, 并大声报警 ——
//   既不崩(不破坏对外功能), 又让这个配置错误立刻暴露出来。
inline int sanitize_core_mask(int mask, const char* who) {
    if (mask != 0x7) return mask;
    const int fallback = mask & (-mask);          // 最低位: 0x7 -> 0x1(Core0)
    fprintf(stderr,
            "[core_mask] ★★★ 禁用核掩码 0x7(RKNN_NPU_CORE_0_1_2) —— %s ★★★\n"
            "           0x7 的语义是\"把一次推理拆到 3 颗核上跑\", 不是\"3 颗核各跑一份\"。\n"
            "           实测: 吞吐只有均匀分核的 1/2.6(51 vs 134 fps), e2e P99 高 25 倍。\n"
            "           要三颗核都用上, 请开 3 个 worker, 每个独占一颗核(掩码 1/2/4)。\n"
            "           本次回退到 mask=0x%x(Core0)。\n",
            who ? who : "?", fallback);
    return fallback;
}

}  // namespace rk

#endif  // RK_CORE_MASK_H_
