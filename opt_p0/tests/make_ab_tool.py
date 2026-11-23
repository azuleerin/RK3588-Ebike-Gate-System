#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_ab_tool.py —— 从 accuracy/acc_batch.cc 生成对拍专用工具 acc_batch_ab.cc

在 acc_batch.cc 的基础上加三件事:
  1) --ab-mode 0/1/2: 0=自带参考解码器 1=生产形态B 2=生产形态C(由 fork9 的原始输出喂)
  2) --dump-raw: 把原始输出张量 dump 成二进制(用来跟 onnxruntime 逐元素比对布局)
  3) build_dual_from_fork9: 把 9 输出解成形态C 的双输出张量
     (box[1,4,8400] cx,cy,w,h + cls[1,nc,8400]), 用来做【同源对拍】:
     生产代码的形态B 路径 vs 生产代码的形态C 路径, 吃同一份原始输出。

★ 生成的工具跟 accuracy/acc_batch 分开编译, 不会动到既有的精度回归工具。
"""
import os
import re
import sys

SRC = "/home/seeed/rk_deploy/accuracy/acc_batch.cc"
DST = "/home/seeed/rk_deploy/opt_p0/tests/acc_batch_ab.cc"

DUAL_FN = r'''
// ---------------------------------------------------------------------------
// P0 对拍专用: 把 9 输出(fork9)解成【形态C 的双输出张量】
//   box[1,4,8400] = cx,cy,w,h (模型输入像素)
//   cls[1,nc,8400] = 概率
// 目的: 让【生产代码的形态C 分支】和【生产代码的形态B 分支】吃【同一份原始输出】,
//       逐框对比坐标 —— 这就是 "形态B vs 形态C" 的同源对拍。
// 解码用 acc_batch.cc 自带的 decode_dfl_9out(2026-11-22 已改成与生产
// compute_dfl 一致的跨步读法), 跟生产代码是两套独立实现。
// ---------------------------------------------------------------------------
static std::vector<OutTensor> build_dual_from_fork9(const std::vector<OutTensor>& outs) {
    static const int kStride[3] = {8, 16, 32};
    uint32_t n_anchor = 0;
    for (int sc = 0; sc < 3; sc++) {
        const OutTensor& bt = outs[sc * 3 + 0];
        if (bt.n_dims < 3) continue;
        n_anchor += bt.dims[bt.n_dims - 2] * bt.dims[bt.n_dims - 1];
    }
    const OutTensor& cls0 = outs[1];
    const int nc = (int)cls0.dims[cls0.n_dims - 2];

    // ★ static: 返回的张量要指住这两块内存, 局部 vector 出函数就析构 -> 悬垂指针
    static std::vector<float> box;
    static std::vector<float> cls;
    box.assign((size_t)4 * n_anchor, 0.f);
    cls.assign((size_t)nc * n_anchor, 0.f);

    uint32_t off = 0;
    for (int sc = 0; sc < 3; sc++) {
        const OutTensor& bt = outs[sc * 3 + 0];
        const OutTensor& ct = outs[sc * 3 + 1];
        if (bt.n_dims < 3 || ct.n_dims < 3) continue;
        const uint32_t H = bt.dims[bt.n_dims - 2], W = bt.dims[bt.n_dims - 1];
        const uint32_t grid = H * W;
        const int stride = kStride[sc];
        const float st = (float)stride;
        for (uint32_t i = 0; i < H; i++) {
            for (uint32_t j = 0; j < W; j++) {
                const uint32_t o = i * W + j;
                const uint32_t a = off + o;
                for (int c = 0; c < nc; c++)
                    cls[(size_t)c * n_anchor + a] = ct.at((uint32_t)c * grid + o);
                float d[4];
                decode_dfl_9out(static_cast<const float*>(bt.data), grid, o, d);
                const float cx = ((float)j + 0.5f) * st;
                const float cy = ((float)i + 0.5f) * st;
                const float x1 = cx - d[0] * st, y1 = cy - d[1] * st;
                const float x2 = cx + d[2] * st, y2 = cy + d[3] * st;
                box[0 * n_anchor + a] = (x1 + x2) * 0.5f;
                box[1 * n_anchor + a] = (y1 + y2) * 0.5f;
                box[2 * n_anchor + a] = x2 - x1;
                box[3 * n_anchor + a] = y2 - y1;
            }
        }
        off += grid;
    }

    OutTensor tb, tc;
    tb.data = box.data(); tb.is_int8 = false; tb.n_elems = (uint32_t)box.size();
    tb.n_dims = 3; tb.dims[0] = 1; tb.dims[1] = 4; tb.dims[2] = n_anchor;
    tc.data = cls.data(); tc.is_int8 = false; tc.n_elems = (uint32_t)cls.size();
    tc.n_dims = 3; tc.dims[0] = 1; tc.dims[1] = (uint32_t)nc; tc.dims[2] = n_anchor;
    return {tb, tc};
}

'''

DUMP_SNIPPET = '''        // ★ P0: dump 原始输出张量 (与 onnxruntime 的同名输出逐元素对比,
        //   用来确定 RKNN 运行时返回的物理布局)
        if (g_dump_raw && fi == 0) {
            FILE* fd = fopen(g_dump_raw, "wb");
            if (fd) {
                uint32_t n = (uint32_t)outs.size();
                fwrite(&n, 4, 1, fd);
                for (size_t i = 0; i < outs.size(); i++) {
                    uint32_t nd = out_attrs[i].n_dims;
                    fwrite(&nd, 4, 1, fd);
                    for (uint32_t dd = 0; dd < nd; dd++) fwrite(&out_attrs[i].dims[dd], 4, 1, fd);
                    uint32_t ne = out_attrs[i].n_elems;
                    fwrite(&ne, 4, 1, fd);
                    fwrite(outs[i].buf, 4, ne, fd);
                }
                fclose(fd);
                fprintf(stderr, "[dump] %s 已写出 (%u 个输出张量)\\n", g_dump_raw, n);
            }
        }
'''


def main():
    src = open(SRC).read()
    lines = src.split("\n")

    # ---- 找 9 输出分支 ----
    start = None
    for i, l in enumerate(lines):
        if "std::vector<YoloBox> boxes;" in l and "io_num.n_output == 9" in lines[i+1]:
            start = i
            break
    if start is None:
        sys.exit("找不到 9 输出分支")
    end = None
    for j in range(start, start + 30):
        if lines[j].strip() == "}":
            end = j
            break

    new_block = """        std::vector<YoloBox> boxes;
        BoxMapper mapper;
        mapper.mode = BoxMapper::STRETCH;
        mapper.scale_x = static_cast<float>(img.cols) / model_w;
        mapper.scale_y = static_cast<float>(img.rows) / model_h;
        mapper.img_w = img.cols;
        mapper.img_h = img.rows;
        if (io_num.n_output == 9) {
            if (g_ab_mode == 1) {
                // 模式1: 【生产代码】的形态B (瑞芯微 fork 9 输出)
                boxes = yolov8_postprocess(ts.data(), static_cast<int>(ts.size()), conf, nms, mapper);
            } else if (g_ab_mode == 2) {
                // 模式2: 参考解码 -> 形态C 张量 -> 【生产代码】的形态C 分支
                std::vector<OutTensor> dual = build_dual_from_fork9(ts);
                boxes = yolov8_postprocess(dual.data(), static_cast<int>(dual.size()), conf, nms, mapper);
            } else {
                // 模式0: 本文件自带的参考解码器(独立实现)
                boxes = decode_9out_standard(ts, conf, nms, 80, img.cols, img.rows,
                                             model_w, model_h);
            }
        } else {
            boxes = yolov8_postprocess(ts.data(), static_cast<int>(ts.size()), conf, nms, mapper);
        }""".split("\n")
    lines[start:end+1] = new_block
    src = "\n".join(lines)

    # ---- 插入 build_dual_from_fork9 ----
    anchor = ("// ---------------------------------------------------------------------------\n"
              "// YOLO: 加载模型 + 跑一批图\n"
              "// ---------------------------------------------------------------------------")
    assert anchor in src, "找不到插入点"
    src = src.replace(anchor, DUAL_FN + anchor, 1)

    # ---- dump raw ----
    old = '        fprintf(fo, "%s,%zu", files[fi].c_str(), boxes.size());'
    assert old in src
    src = src.replace(old, DUMP_SNIPPET + old, 1)

    # ---- 全局变量 + CLI ----
    src = src.replace("int run_yolo(int argc, char** argv) {",
                      "// P0 对拍模式: 0=参考解码 1=生产形态B 2=生产形态C(由 fork9 喂)\n"
                      "static int g_ab_mode = 0;\n"
                      "static const char* g_dump_raw = nullptr;\n\n"
                      "int run_yolo(int argc, char** argv) {", 1)
    oldcli = '        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {'
    assert oldcli in src
    src = src.replace(oldcli,
                      '        else if (!strcmp(argv[i], "--ab-mode") && i + 1 < argc) g_ab_mode = atoi(argv[++i]);\n'
                      '        else if (!strcmp(argv[i], "--dump-raw") && i + 1 < argc) g_dump_raw = argv[++i];\n'
                      + oldcli, 1)

    os.makedirs(os.path.dirname(DST), exist_ok=True)
    open(DST, "w").write(src)
    print(f"已生成 {DST}")


if __name__ == "__main__":
    main()
