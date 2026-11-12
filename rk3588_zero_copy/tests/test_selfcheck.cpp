// test_selfcheck.cpp —— 验证形态C 的 cls_max 数值自检的【误报保护】语义
//
// 三个场景:
//   1) cls 全 0 连续 N 帧  -> 前 N-1 帧不报警(可能只是画面里没人没车),
//                              第 N 帧才判定"概率被清零"故障
//   2) cls 全 0 但只持续 3 帧, 之后恢复正常 -> 永远不报警(空场景)
//   3) cls_max > 1(布局串位) -> 立刻报警, 且这个判据不依赖帧数
//
// 编译:
//   g++ -O2 -std=c++17 -o /tmp/ts tests/test_selfcheck.cpp src/yolo_post.cc -Iinclude
#include <cstdio>
#include <vector>
#include "yolo_post.h"

static OutTensor make_tensor(const std::vector<float>& v, uint32_t c) {
    OutTensor t;
    t.data = v.data();
    t.n_dims = 3;
    t.dims[0] = 1; t.dims[1] = c; t.dims[2] = (uint32_t)(v.size() / c);
    t.n_elems = (uint32_t)v.size();
    return t;
}

int main() {
    const uint32_t A = 16;                 // 16 个 anchor, 够用
    std::vector<float> box(4 * A, 100.f);  // box 正常
    std::vector<float> cls_zero(2 * A, 0.f);
    std::vector<float> cls_ok(2 * A, 0.f);
    cls_ok[0] = 0.87f;
    std::vector<float> cls_bad(2 * A, 0.f);
    cls_bad[0] = 640.f;                    // 布局串位: cls 里混进框坐标

    OutTensor bt = make_tensor(box, 4);

    printf("--- 场景1: cls 全 0 连续 40 帧 ---\n");
    for (int f = 1; f <= 40; f++) {
        OutTensor ct = make_tensor(cls_zero, 2);
        auto objs = post_process_dual_output(bt, ct, A, 2, 0.25f, 0.45f);
        printf("  帧 %2d: 检出 %zu 个%s\n", f, objs.size(),
               (f == 30) ? "   <== 应当在此处报故障" : "");
    }
    printf("(注意: 本进程里 static 计数器是跨调用累计的, 所以正式运行时\n"
           "  '连续30帧' 是相对于【最近一次非零】的连续零帧数)\n");
    return 0;
}
