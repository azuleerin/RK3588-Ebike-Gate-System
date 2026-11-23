// acc_batch.cc —— 精度回归的离线批量推理工具(独立程序, 不碰主程序源码)
//
// 两种子命令:
//   yolo : 对一批整帧跑 YOLO, 把检测框写成 CSV
//          acc_batch yolo --model M.rknn --list frames.csv --out boxes.csv
//   rec  : 对一批车牌裁剪图跑识别模型(LPRNet / PP-OCR), 写 CSV
//          acc_batch rec --list plates.csv --out rec.csv \
//                        --lpr lprnet.rknn --charset "0123..." \
//                        --ppocr ppocr.rknn --dict ppocr_keys_v1.txt
//
// ★ 为什么要独立: 精度回归要反复换模型/换量化配置, 主程序是交付件不能改。
//
// 编译(板端):
//   g++ -O2 -std=c++17 -o acc_batch acc_batch.cc \
//       ../rk3588_zero_copy/src/lprnet.cc \
//       ../rk3588_zero_copy/src/license_plate_rec.cc \
//       ../rk3588_zero_copy/src/yolo_post.cc \
//       -I../rk3588_zero_copy/include -I../rk3588_zero_copy/third_party/include \
//       ../rk3588_zero_copy/third_party/lib/librknnrt.so \
//       $(pkg-config --cflags --libs opencv4)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "license_plate_rec.h"
#include "lprnet.h"
#include "rknn_api.h"
#include "yolo_post.h"

namespace {

// ---------------------------------------------------------------------------
// 读一个"路径列表"文件(每行一个路径, 可带逗号分隔的其它字段, 只取第一列)
// ---------------------------------------------------------------------------
std::vector<std::string> read_list(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const size_t c = line.find(',');
        out.push_back(c == std::string::npos ? line : line.substr(0, c));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 标准 ultralytics 导出的 9 输出解码 (yolov8n_optimized.onnx 这种)
//
//   每个尺度 3 个输出: [box_dist(64ch,H,W), cls(nc ch,H,W), cls_max(1ch,H,W)]
//   ★ 与工程里 yolo_post.cc 的"形态B"(瑞芯微 fork 导出)的关系(2026-11-22 订正):
//     ① 【DFL 的内存布局】两者现在【完全一致】—— 都是跨步读:
//            第 (side*16+bin) 个 bin 的地址 = off + (side*16+bin)*grid_len
//        因为 box 张量是 [1,64,H,W] 的 NCHW 布局, 64 个通道的含义是
//        c = box*16 + bin(与 ultralytics 的 DFL 模块
//        x.view(b,4,self.c1,a).transpose(2,1).softmax(1) 一致),
//        同一个 anchor 的 16 个 bin 在内存里【相隔 grid_len】, 不是连续的 16 个。
//
//        ⚠ 本函数原来写的是【连续读法】(box + side*16*grid_len + off + i),
//          读到的是"同一个 channel 的连续 16 个 anchor 的 logits", 算出来的框
//          是错的 —— 它和生产 compute_dfl 对不上, 一度让人以为生产代码有 bug。
//          2026-11-22 已改成跨步读, 与生产一致。
//
//     ② 【单位】DFL 解出来的距离是【格子单位】, 要乘 stride 才是像素。
//        这里乘了, 工程里 post_process_fork_9out 现在也乘了(两边一致)。
//        ★ 纠正一条旧注释: "fork 导出的模型把 stride 烤进图里了, 所以工程里
//          那一路不乘" —— 这句【是错的】。DFL 是在 16 个 bin 上做 softmax,
//          期望值天然落在 [0,15], 而把 logits 整体乘一个常数【不会改变
//          softmax 分布】(softmax 平移不变) —— 所以"把 stride 烤进 logits"
//          在数学上不可能。stride 只能在解码之后乘。
//
//     ③ cls 的通道数 = 类别数(COCO 是 80)。
//
// 这个解码器只在本测试工具里用, 不动工程源码。
// ---------------------------------------------------------------------------
static void decode_dfl_9out(const float* box, uint32_t grid_len, uint32_t off, float* d) {
    for (int side = 0; side < 4; side++) {
        // ★ 跨步读(NCHW, c = box*16 + bin): 第 bin 个 bin 的地址
        //   = off + (side*16 + bin) * grid_len
        auto logit = [&](int bin) -> float {
            return box[(size_t)(side * 16 + bin) * grid_len + off];
        };
        float mx = logit(0);
        for (int i = 1; i < 16; ++i) if (logit(i) > mx) mx = logit(i);
        float sum = 0.f;
        float e[16];
        for (int i = 0; i < 16; ++i) { e[i] = expf(logit(i) - mx); sum += e[i]; }
        float acc = 0.f;
        for (int i = 0; i < 16; ++i) acc += (e[i] / sum) * static_cast<float>(i);
        d[side] = acc;
    }
}

static std::vector<YoloBox> decode_9out_standard(const std::vector<OutTensor>& outs,
                                                   float conf_thr, float nms_thr,
                                                   int nc, float img_w, float img_h,
                                                   float model_w, float model_h) {
    static const int kStride[3] = {8, 16, 32};
    std::vector<YoloBox> objs;
    for (int sc = 0; sc < 3; ++sc) {
        const size_t ib = static_cast<size_t>(sc) * 3;
        if (ib + 2 >= outs.size()) break;
        const OutTensor& bt = outs[ib + 0];
        const OutTensor& ct = outs[ib + 1];
        const OutTensor& st = outs[ib + 2];
        if (bt.n_dims < 3) continue;
        const uint32_t H = bt.dims[bt.n_dims - 2];
        const uint32_t W = bt.dims[bt.n_dims - 1];
        const uint32_t grid = H * W;
        const int stride = kStride[sc];
        if (grid == 0) continue;

        for (uint32_t i = 0; i < H; i++) {
            for (uint32_t j = 0; j < W; j++) {
                const uint32_t off = i * W + j;
                const float score = st.at(off);
                if (score <= conf_thr) continue;
                int best_cls = -1;
                float best = -1.f;
                for (int c = 0; c < nc; c++) {
                    const float v = ct.at(static_cast<uint32_t>(c) * grid + off);
                    if (v > best) { best = v; best_cls = c; }
                }
                if (best_cls < 0 || best <= conf_thr) continue;
                float d[4];
                decode_dfl_9out(static_cast<const float*>(bt.data), grid, off, d);
                const float cx = (static_cast<float>(j) + 0.5f) * stride;
                const float cy = (static_cast<float>(i) + 0.5f) * stride;
                YoloBox o;
                o.cls_id = best_cls;
                o.score = best;
                o.x1 = cx - d[0] * stride;
                o.y1 = cy - d[1] * stride;
                o.x2 = cx + d[2] * stride;
                o.y2 = cy + d[3] * stride;
                objs.push_back(o);
            }
        }
    }
    // 映射回原图坐标(模型输入是拉伸缩放到 model_w x model_h)
    const float sx = img_w / model_w, sy = img_h / model_h;
    for (auto& o : objs) {
        o.x1 *= sx; o.x2 *= sx; o.y1 *= sy; o.y2 *= sy;
        if (o.x1 < 0) o.x1 = 0;
        if (o.y1 < 0) o.y1 = 0;
        if (o.x2 > img_w) o.x2 = img_w;
        if (o.y2 > img_h) o.y2 = img_h;
    }
    nms_per_class(objs, conf_thr, nms_thr);
    return objs;
}

// ---------------------------------------------------------------------------
// YOLO: 加载模型 + 跑一批图
// ---------------------------------------------------------------------------
int run_yolo(int argc, char** argv) {
    std::string model, list, out;
    float conf = 0.25f, nms = 0.45f;
    int model_w = 640, model_h = 640;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--list") && i + 1 < argc) list = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--conf") && i + 1 < argc) conf = atof(argv[++i]);
        else if (!strcmp(argv[i], "--nms") && i + 1 < argc) nms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &model_w, &model_h);
        }
    }
    if (model.empty() || list.empty() || out.empty()) {
        fprintf(stderr, "用法: acc_batch yolo --model M.rknn --list list.csv --out out.csv\n");
        return 2;
    }

    rknn_context ctx = 0;
    if (rknn_init(&ctx, const_cast<char*>(model.c_str()), 0, RKNN_FLAG_PRIOR_HIGH, nullptr) < 0) {
        fprintf(stderr, "rknn_init 失败: %s\n", model.c_str());
        return 1;
    }
    rknn_input_output_num io_num{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));

    // 输入属性(决定喂法: uint8 / float / 布局)
    rknn_tensor_attr ia{};
    ia.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &ia, sizeof(ia));
    const bool in_nchw = (ia.fmt == RKNN_TENSOR_NCHW);
    const int in_type = static_cast<int>(ia.type);
    // 量化模型 vs 真浮点模型: 看【逻辑】attr
    //   (rknn_tensor_type: FLOAT32=0, FLOAT16=1, INT8=2, UINT8=3)
    const bool model_input_quantized =
        (ia.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC);
    const bool model_is_fp = (ia.type == RKNN_TENSOR_FLOAT32 || ia.type == RKNN_TENSOR_FLOAT16);
    const bool use_float_input = (!model_input_quantized && model_is_fp);
    if (ia.n_dims >= 4) {
        if (in_nchw) { model_h = ia.dims[2]; model_w = ia.dims[3]; }
        else        { model_h = ia.dims[1]; model_w = ia.dims[2]; }
    }
    fprintf(stderr, "[acc_batch] 模型输入 %dx%d %s type=%d qnt=%d -> 喂 %s\n",
            model_w, model_h, in_nchw ? "NCHW" : "NHWC", in_type,
            static_cast<int>(ia.qnt_type),
            use_float_input ? "float32 NCHW /255" : "uint8 NHWC 原始[0,255]");

    std::vector<rknn_tensor_attr> out_attrs(io_num.n_output);
    for (int i = 0; i < io_num.n_output; i++) {
        memset(&out_attrs[i], 0, sizeof(out_attrs[i]));
        out_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attrs[i], sizeof(out_attrs[i]));
    }

    // 把输出张量属性打出来(FP16 与 INT8 模型的输出结构可能不一样:
    // 量化时 toolkit 会优化图, 输出从原始 ONNX 形状变成 [1,64,8400] 之类)
    for (int i = 0; i < io_num.n_output; i++) {
        fprintf(stderr, "  输出[%d] %s type=%d fmt=%d dims=[", i, out_attrs[i].name,
                static_cast<int>(out_attrs[i].type), static_cast<int>(out_attrs[i].fmt));
        for (uint32_t k = 0; k < out_attrs[i].n_dims && k < 4; k++) {
            fprintf(stderr, "%u,", out_attrs[i].dims[k]);
        }
        fprintf(stderr, "] n_elems=%u\n", out_attrs[i].n_elems);
    }

    const std::vector<std::string> files = read_list(list);
    FILE* fo = fopen(out.c_str(), "w");
    if (!fo) { fprintf(stderr, "打不开输出文件 %s\n", out.c_str()); return 1; }
    fprintf(fo, "img,n_boxes,cls,score,x1,y1,x2,y2\n");

    std::vector<uint8_t> u8_buf(static_cast<size_t>(model_w) * model_h * 3);
    std::vector<float> f32_buf(static_cast<size_t>(model_w) * model_h * 3);
    int done = 0;
    for (size_t fi = 0; fi < files.size(); fi++) {
        cv::Mat img = cv::imread(files[fi], cv::IMREAD_COLOR);
        if (img.empty()) continue;
        cv::Mat small;
        cv::resize(img, small, cv::Size(model_w, model_h), 0, 0, cv::INTER_LINEAR);
        cv::cvtColor(small, small, cv::COLOR_BGR2RGB);

        // ★ 喂法必须和主程序一致(src/main.cc:1490-1524 的约定):
        //   量化模型(逻辑 attr 的 qnt_type = AFFINE_ASYMMETRIC)
        //       -> 喂 uint8 NHWC 原始[0,255], type=UINT8, 由运行时自己做量化。
        //          ★ 千万别喂 type=INT8 + 原始字节: 运行时会把它们当成量化码,
        //            等于喂了一堆垃圾(实测: 输出全乱, 每帧几千个假框)。
        //   真浮点模型(FLOAT32 / FLOAT16)
        //       -> 喂 float32 NCHW, 喂原始[0,255]/255(ONNX 契约是[0,1])。
        //          ★ 判定要看【逻辑】attr 的 type, FLOAT16 也要算浮点,
        //            漏判就会把 FP16 模型当量化模型喂 uint8 -> 输出全乱。
        //   (工程里踩过: "每帧 6000+ 个假框", 见 main.cc 的注释)
        rknn_input in{};
        in.index = 0;
        in.pass_through = 0;
        if (use_float_input) {
            // NCHW float32, /255
            const int HW = model_w * model_h;
            for (int y = 0; y < model_h; y++) {
                for (int x = 0; x < model_w; x++) {
                    const uint8_t* p = small.ptr(y) + x * 3;
                    float* q = f32_buf.data() + static_cast<size_t>(y) * model_w * 3 + x * 3;
                    for (int c = 0; c < 3; c++) {
                        q[c] = p[c] * (1.0f / 255.0f);
                    }
                }
            }
            // 转成 CHW
            static thread_local std::vector<float> chw;
            chw.resize(f32_buf.size());
            for (int y = 0; y < model_h; y++) {
                for (int x = 0; x < model_w; x++) {
                    for (int c = 0; c < 3; c++) {
                        chw[static_cast<size_t>(c) * HW + y * model_w + x] =
                            f32_buf[(static_cast<size_t>(y) * model_w + x) * 3 + c];
                    }
                }
            }
            f32_buf.swap(chw);
            in.buf = f32_buf.data();
            in.size = static_cast<uint32_t>(f32_buf.size() * sizeof(float));
            in.type = RKNN_TENSOR_FLOAT32;
            in.fmt = RKNN_TENSOR_NCHW;
        } else {
            memcpy(u8_buf.data(), small.data, u8_buf.size());
            in.buf = u8_buf.data();
            in.size = static_cast<uint32_t>(u8_buf.size());
            in.type = RKNN_TENSOR_UINT8;
            in.fmt = RKNN_TENSOR_NHWC;
        }

        if (rknn_inputs_set(ctx, 1, &in) != 0) continue;
        if (rknn_run(ctx, nullptr) != 0) continue;

        std::vector<rknn_output> outs(io_num.n_output);
        memset(outs.data(), 0, sizeof(rknn_output) * outs.size());
        for (int i = 0; i < io_num.n_output; i++) {
            outs[i].want_float = 1;
            outs[i].is_prealloc = 0;
            outs[i].index = i;
        }
        if (rknn_outputs_get(ctx, io_num.n_output, outs.data(), nullptr) != 0) continue;

        std::vector<OutTensor> ts(outs.size());
        for (size_t i = 0; i < outs.size(); i++) {
            ts[i].data = outs[i].buf;
            ts[i].is_int8 = false;    // 一律用 float 输出(量化模型由驱动反量化)
            ts[i].zp = 0; ts[i].scale = 1.f;
            ts[i].n_elems = out_attrs[i].n_elems;
            ts[i].n_dims = out_attrs[i].n_dims;
            for (uint32_t k = 0; k < out_attrs[i].n_dims && k < 4; k++) {
                ts[i].dims[k] = out_attrs[i].dims[k];
            }
        }
        std::vector<YoloBox> boxes;
        if (io_num.n_output == 9) {
            // 标准 ultralytics 导出: 用自带解码器(DFL 距离要乘 stride)
            //
            // ★★ nc 必须从 cls 张量自动推导(形态B/fork9: cls 是 [1, nc, H, W]
            //    -> nc = dims[n_dims-3]), 与生产 src/yolo_post.cc 的做法一致。
            //    原来这里写死 80(COCO 口径): 对 nc=2 的 ebike fork9 模型,
            //    decode_9out_standard 会按 c=0..79 去读只有 2 个通道的 cls 张量
            //    -> 越界读 -> 垃圾框 / 直接段错误(2026-11-23 实测: models/best.rknn
            //    跑 acc_batch 直接 Segmentation fault)。
            //    ★ 形态C(2 输出)那条路不用在这里推导: 它走 yolov8_postprocess,
            //      内部按 cls.dims[n_dims-2] 推 nc(与 fork9 的 n_dims-3 差一维,
            //      别搞混)。
            int nc9 = 80;
            if (ts.size() > 1 && ts[1].n_dims >= 3) {
                nc9 = static_cast<int>(ts[1].dims[ts[1].n_dims - 3]);
            }
            boxes = decode_9out_standard(ts, conf, nms, nc9, img.cols, img.rows,
                                         model_w, model_h);
        } else {
            BoxMapper mapper;
            mapper.mode = BoxMapper::STRETCH;
            mapper.scale_x = static_cast<float>(img.cols) / model_w;
            mapper.scale_y = static_cast<float>(img.rows) / model_h;
            mapper.img_w = img.cols;
            mapper.img_h = img.rows;
            boxes = yolov8_postprocess(ts.data(), static_cast<int>(ts.size()), conf, nms, mapper);
        }

        fprintf(fo, "%s,%zu", files[fi].c_str(), boxes.size());
        for (const auto& b : boxes) {
            fprintf(fo, ",%d,%.6f,%.6f,%.6f,%.6f,%.6f", b.cls_id, b.score,
                    b.x1 / img.cols, b.y1 / img.rows, b.x2 / img.cols, b.y2 / img.rows);
        }
        fprintf(fo, "\n");
        rknn_outputs_release(ctx, io_num.n_output, outs.data());

        if (++done % 200 == 0) fprintf(stderr, "  yolo: %d/%zu\n", done, files.size());
    }
    fclose(fo);
    rknn_destroy(ctx);
    fprintf(stderr, "[acc_batch] yolo 完成: %d 张 -> %s\n", done, out.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// 识别: LPRNet + PP-OCR
// ---------------------------------------------------------------------------
int run_rec(int argc, char** argv) {
    std::string list, out, lpr_model, charset, ppocr_model, dict;
    bool ppocr_baked = false;
    bool ppocr_all = false;      // 不做字符白名单过滤(看模型的原始输出)
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--list") && i + 1 < argc) list = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--lpr") && i + 1 < argc) lpr_model = argv[++i];
        else if (!strcmp(argv[i], "--charset") && i + 1 < argc) charset = argv[++i];
        else if (!strcmp(argv[i], "--ppocr") && i + 1 < argc) ppocr_model = argv[++i];
        else if (!strcmp(argv[i], "--dict") && i + 1 < argc) dict = argv[++i];
        else if (!strcmp(argv[i], "--ppocr-baked")) ppocr_baked = true;
        else if (!strcmp(argv[i], "--ppocr-all")) ppocr_all = true;
    }
    if (list.empty() || out.empty()) {
        fprintf(stderr, "用法: acc_batch rec --list plates.csv --out rec.csv "
                        "[--lpr M --charset CS] [--ppocr M --dict D]\n");
        return 2;
    }

    // ---- LPRNet ----
    rk::LprNet lpr;
    bool have_lpr = false;
    if (!lpr_model.empty()) {
        have_lpr = lpr.init(lpr_model, charset);
        if (!have_lpr) fprintf(stderr, "[acc_batch] LPRNet 初始化失败: %s\n", lpr_model.c_str());
    }

    // ---- PP-OCR ----
    rk::LicensePlateRec ppocr;
    bool have_ppocr = false;
    if (!ppocr_model.empty()) {
        rk::PlateRecConfig cfg;
        cfg.model_path = ppocr_model;
        cfg.dict_path = dict;
        cfg.norm_baked_in = ppocr_baked;   // 归一化已烤进模型 -> 只喂原始像素
        if (!ppocr_all) {
            cfg.allowed_charset =
                "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ"
                "京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼港澳学警挂";
        }
        have_ppocr = ppocr.init(cfg);
        if (!have_ppocr) fprintf(stderr, "[acc_batch] PP-OCR 初始化失败: %s\n", ppocr_model.c_str());
    }
    if (!have_lpr && !have_ppocr) {
        fprintf(stderr, "[acc_batch] 两个识别模型都没加载成功\n");
        return 1;
    }

    // 清单: 路径,GT,...(第二列是 GT, 没有就空)
    std::ifstream in(list);
    FILE* fo = fopen(out.c_str(), "w");
    if (!fo) { fprintf(stderr, "打不开 %s\n", out.c_str()); return 1; }
    fprintf(fo, "crop_path,gt,lpr_text,lpr_score,ppocr_text,ppocr_score\n");

    std::string line;
    int done = 0;
    std::vector<uint8_t> rgb(static_cast<size_t>(rk::kLprW) * rk::kLprH * 3);
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> cols;
        size_t pos = 0;
        while (true) {
            const size_t c = line.find(',', pos);
            cols.push_back(c == std::string::npos ? line.substr(pos) : line.substr(pos, c - pos));
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        const std::string path = cols[0];
        const std::string gt = (cols.size() > 2 && !cols[2].empty()) ? cols[2] :
                              (cols.size() > 1 ? cols[1] : "");
        cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
        if (img.empty()) continue;

        std::string lpr_text, pp_text;
        float lpr_score = 0.f, pp_score = 0.f;

        if (have_lpr) {
            // ★ 喂原始 uint8 RGB 就行 —— LprNet::infer() 自己按模型 attr 里的
            //   zp/scale 生成 int8 量化码(/255 也由它内部做)。
            //   ★ 别在这里预先做 /255 或 (x-0.5)/0.5: 重复归一化会把输入毁掉,
            //     表现就是"几乎全解不出来"(已踩过)。
            cv::Mat small;
            cv::resize(img, small, cv::Size(rk::kLprW, rk::kLprH), 0, 0, cv::INTER_LINEAR);
            cv::cvtColor(small, small, cv::COLOR_BGR2RGB);
            for (int y = 0; y < rk::kLprH; y++) {
                memcpy(rgb.data() + static_cast<size_t>(y) * rk::kLprW * 3,
                       small.ptr(y), static_cast<size_t>(rk::kLprW) * 3);
            }
            lpr.infer(rgb.data(), &lpr_text);
            // 分数: LprNet::infer 不返回分数, 后面用"是否非空/长度"做辅助判据
        }
        if (have_ppocr) {
            rk::PlateRecResult r;
            if (ppocr.recognize_bgr(img, &r)) {
                pp_text = r.text;
                pp_score = r.score;
            }
        }
        fprintf(fo, "%s,%s,%s,%.6f,%s,%.6f\n", path.c_str(), gt.c_str(),
                lpr_text.c_str(), lpr_score, pp_text.c_str(), pp_score);
        if (++done % 100 == 0) fprintf(stderr, "  rec: %d\n", done);
    }
    fclose(fo);
    fprintf(stderr, "[acc_batch] rec 完成: %d 张 -> %s\n", done, out.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: acc_batch <yolo|rec> [选项]\n");
        return 2;
    }
    if (!strcmp(argv[1], "yolo")) return run_yolo(argc, argv);
    if (!strcmp(argv[1], "rec")) return run_rec(argc, argv);
    fprintf(stderr, "未知子命令: %s\n", argv[1]);
    return 2;
}
