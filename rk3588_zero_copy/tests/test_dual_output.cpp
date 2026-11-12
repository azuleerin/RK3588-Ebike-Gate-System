// test_dual_output.cpp —— 「形态 C」box/cls 双输出后处理的数值对拍入口
//
// 输入是 RKNN 模拟器 dump 的原始张量(float32, CHW 平铺):
//   <prefix>_box.bin   [1, 4, N]  cx,cy,w,h (像素)
//   <prefix>_cls.bin   [1, nc, N] 概率
//   <prefix>_meta.txt  "原图宽 原图高 box通道数 num_anchors"
//
// 输出每行: cls score x1 y1 x2 y2   (已按拉伸缩放映射回原图坐标)
//
// 编译:
//   g++ -O2 -std=c++17 -Wall -o /tmp/td tests/test_dual_output.cpp
//       src/yolo_post.cc -Iinclude
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "yolo_post.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <prefix> [conf] [nms] [--zero-cls]\n", argv[0]);
        fprintf(stderr, "  --zero-cls  把 cls 张量整体清零, 验证「形态C 数值自检」\n"
                        "             能抓住概率被量化清零的坏数据(应当输出 0 个框)\n");
        return 1;
    }
    const std::string prefix = argv[1];
    const float conf = (argc > 2) ? static_cast<float>(atof(argv[2])) : 0.25f;
    const float nms  = (argc > 3) ? static_cast<float>(atof(argv[3])) : 0.45f;
    bool zero_cls = false;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--zero-cls") == 0) zero_cls = true;

    char path[512];
    snprintf(path, sizeof(path), "%s_meta.txt", prefix.c_str());
    FILE* fm = fopen(path, "r");
    if (!fm) { fprintf(stderr, "打不开 %s\n", path); return 1; }
    int img_w = 0, img_h = 0, box_ch = 0, num_anchors = 0;
    if (fscanf(fm, "%d %d %d %d", &img_w, &img_h, &box_ch, &num_anchors) != 4) {
        fprintf(stderr, "%s 格式不对\n", path);
        fclose(fm);
        return 1;
    }
    fclose(fm);

    snprintf(path, sizeof(path), "%s_box.bin", prefix.c_str());
    FILE* fb = fopen(path, "rb");
    snprintf(path, sizeof(path), "%s_cls.bin", prefix.c_str());
    FILE* fc = fopen(path, "rb");
    if (!fb || !fc) { fprintf(stderr, "打不开 bin\n"); return 1; }

    std::vector<float> boxbuf(static_cast<size_t>(box_ch) * num_anchors);
    (void)!fread(boxbuf.data(), sizeof(float), boxbuf.size(), fb);
    fclose(fb);

    // cls 的通道数从文件大小反推
    fseek(fc, 0, SEEK_END);
    const long cls_bytes = ftell(fc);
    fseek(fc, 0, SEEK_SET);
    const int nc = static_cast<int>(cls_bytes / (sizeof(float) * num_anchors));
    std::vector<float> clsbuf(static_cast<size_t>(nc) * num_anchors);
    (void)!fread(clsbuf.data(), sizeof(float), clsbuf.size(), fc);
    fclose(fc);

    fprintf(stderr, "图 %dx%d  box通道 %d  num_anchors %d  nc %d  conf %.2f nms %.2f\n",
            img_w, img_h, box_ch, num_anchors, nc, conf, nms);

    if (zero_cls) {
        // 模拟「int8 单输出量化把 cls 压成 0」的静默故障
        memset(clsbuf.data(), 0, clsbuf.size() * sizeof(float));
        fprintf(stderr, "[测试] 已把 cls 张量整体清零, 期望自检拦截并输出 0 个框\n");
    }

    OutTensor bt, ct;   // 成员都有默认初值, 不要 memset(非平凡类型)
    bt.data = boxbuf.data();
    bt.n_dims = 3;
    bt.dims[0] = 1; bt.dims[1] = static_cast<uint32_t>(box_ch); bt.dims[2] = static_cast<uint32_t>(num_anchors);
    bt.n_elems = static_cast<uint32_t>(boxbuf.size());
    ct.data = clsbuf.data();
    ct.n_dims = 3;
    ct.dims[0] = 1; ct.dims[1] = static_cast<uint32_t>(nc); ct.dims[2] = static_cast<uint32_t>(num_anchors);
    ct.n_elems = static_cast<uint32_t>(clsbuf.size());

    // 拉伸缩放映射回原图(与板端默认一致: --letterbox 才换算法)
    BoxMapper mapper;
    mapper.mode = BoxMapper::STRETCH;
    mapper.scale_x = static_cast<float>(img_w) / 640.0f;
    mapper.scale_y = static_cast<float>(img_h) / 640.0f;
    mapper.img_w = img_w;
    mapper.img_h = img_h;

    std::vector<YoloBox> boxes =
        post_process_dual_output(bt, ct, static_cast<uint32_t>(num_anchors), nc, conf, nms);
    mapper.apply(boxes);

    for (const auto& b : boxes) {
        printf("%d %.6f %.2f %.2f %.2f %.2f\n",
               b.cls_id, b.score, b.x1, b.y1, b.x2, b.y2);
    }
    if (zero_cls) {
        // 自检必须拦住: 0 个框 + 明确的失败提示
        if (boxes.empty()) {
            fprintf(stderr, "[测试] OK: 自检拦住了清零的 cls, 输出 0 个框\n");
            return 0;
        }
        fprintf(stderr, "[测试] 失败: 自检没拦住, 仍然输出了 %zu 个框\n", boxes.size());
        return 1;
    }
    return 0;
}
