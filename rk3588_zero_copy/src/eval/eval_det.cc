// eval_det.cc —— 检测精度评测工具: 把一个 .rknn 模型跑遍一批图片, 输出逐图检测结果(JSON)
//
// 用途: 量化回归 —— 同一个 ONNX 编出来的 INT8 / FP16 两个模型, 喂【完全相同】的
//       图片, 比较检测框的一致性(mAP / IoU / 置信度漂移)。
//
// 用法:
//   rk_eval --model <model.rknn> --images <dir> --out <out.json> \
//           [--limit N] [--conf 0.25] [--nms 0.45] [--labels labels.txt] \
//           [--input-mode auto|uint8|float_raw|float_scaled] [--no-rga]
//
// 输出 JSON:
//   {"model":"...","input_mode":"uint8","results":[
//      {"file":"frame_000000.jpg","dets":[{"c":0,"s":0.87,"b":[x1,y1,x2,y2]}]}]}
//
// 预处理链和生产一致: BGR(JPEG) -> NV12(OpenCV) -> RGB888(RGA, 写进模型输入的
// dma-buf) -> NPU。两个模型走完全一样的像素, 差异只来自量化。
#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <rknn_api.h>

#include "dmabuf.h"
#include "rga_convert.h"
#include "safe_probe.h"
#include "yolo_post.h"

namespace {

// float32 -> IEEE754 binary16(自己实现, 不依赖 OpenCV 的 half 扩展)
inline uint16_t float_to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x007FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) {          // inf / nan
        return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    }
    if (exp >= 0x1F) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x00800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        const uint32_t half = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1);
        const uint32_t h = half + ((rem > (1u << (shift - 1))) ? 1u : 0u);
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    if ((mant & 0x1FFFu) > 0x1000u) h += 1;
    return static_cast<uint16_t>(h);
}

struct Args {
    std::string model;
    std::string images;
    std::string out;
    std::string labels;
    int   limit = 0;
    float conf = 0.25f;
    float nms = 0.45f;
    std::string input_mode = "auto";   // auto|uint8|float_raw|float_scaled
    bool  use_rga = true;
};

std::vector<std::string> list_images(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".jpg") == 0 ||
                             n.compare(n.size() - 4, 4, ".png") == 0 ||
                             n.compare(n.size() - 4, 4, ".jpeg") == 0)) {
            out.push_back(dir + "/" + n);
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (k == "--model") a.model = next();
        else if (k == "--images") a.images = next();
        else if (k == "--out") a.out = next();
        else if (k == "--labels") a.labels = next();
        else if (k == "--limit") a.limit = atoi(next().c_str());
        else if (k == "--conf") a.conf = atof(next().c_str());
        else if (k == "--nms") a.nms = atof(next().c_str());
        else if (k == "--input-mode") a.input_mode = next();
        else if (k == "--no-rga") a.use_rga = false;
        else { fprintf(stderr, "未知参数: %s\n", k.c_str()); return 1; }
    }
    if (a.model.empty() || a.images.empty()) {
        fprintf(stderr, "用法: rk_eval --model <rknn> --images <dir> --out <json>\n");
        return 1;
    }

    // ---- 读模型 ----
    FILE* f = fopen(a.model.c_str(), "rb");
    if (!f) { fprintf(stderr, "打不开模型: %s\n", a.model.c_str()); return 1; }
    fseek(f, 0, SEEK_END);
    const size_t msize = static_cast<size_t>(ftell(f));
    fseek(f, 0, SEEK_SET);
    unsigned char* mdata = static_cast<unsigned char*>(malloc(msize));
    if (fread(mdata, 1, msize, f) != msize) { fprintf(stderr, "读模型失败\n"); return 1; }
    fclose(f);

    rknn_context rctx = 0;
    int ret = rknn_init(&rctx, mdata, static_cast<uint32_t>(msize), RKNN_FLAG_PRIOR_HIGH, nullptr);
    free(mdata);
    if (ret != 0) { fprintf(stderr, "rknn_init 失败: %d\n", ret); return 1; }

    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    rknn_query(rctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    rknn_tensor_attr attr_logic;
    memset(&attr_logic, 0, sizeof(attr_logic));
    attr_logic.index = 0;
    rknn_query(rctx, RKNN_QUERY_INPUT_ATTR, &attr_logic, sizeof(attr_logic));
    rknn_tensor_attr attr_native;
    memset(&attr_native, 0, sizeof(attr_native));
    attr_native.index = 0;
    bool use_native = false;
    if (rknn_query(rctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &attr_native,
                   sizeof(attr_native)) == 0 &&
        attr_native.type == RKNN_TENSOR_UINT8 && attr_native.fmt == RKNN_TENSOR_NHWC &&
        attr_native.n_dims == 4 && attr_native.dims[3] == 3) {
        use_native = true;
    }
    std::vector<rknn_tensor_attr> out_attrs(io_num.n_output);
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        memset(&out_attrs[i], 0, sizeof(rknn_tensor_attr));
        out_attrs[i].index = i;
        rknn_query(rctx, RKNN_QUERY_OUTPUT_ATTR, &out_attrs[i], sizeof(out_attrs[i]));
    }

    int model_w = 0, model_h = 0;
    if (use_native) {
        model_h = static_cast<int>(attr_native.dims[1]);
        model_w = static_cast<int>(attr_native.dims[2]);
    } else if (attr_logic.fmt == RKNN_TENSOR_NHWC && attr_logic.n_dims == 4) {
        model_h = static_cast<int>(attr_logic.dims[1]);
        model_w = static_cast<int>(attr_logic.dims[2]);
    } else if (attr_logic.n_dims == 4) {
        model_h = static_cast<int>(attr_logic.dims[2]);
        model_w = static_cast<int>(attr_logic.dims[3]);
    }
    fprintf(stderr, "[eval] 模型=%s 输入=%dx%d type=%d qnt=%d fmt=%d native=%s in/out=%u/%u\n",
            a.model.c_str(), model_w, model_h, (int)attr_logic.type,
            (int)attr_logic.qnt_type, (int)attr_logic.fmt, use_native ? "是" : "否",
            io_num.n_input, io_num.n_output);

    // ---- 输入模式 ----
    //   量化模型(qnt=AFFINE_ASYMMETRIC): 喂 uint8 NHWC 原始[0,255]
    //   浮点模型(FP16/FP32):              喂 float32 NCHW /255
    // ⚠️ 量化模型喂 /255 = 双重归一化 -> 检测全废(见 main.cc 的注释), 这里
    //    允许 --input-mode 强制覆盖, 用来实证哪种喂法对。
    // ★ 输入契约(实测确定, 别按 qnt_type 猜):
    //   逻辑输入是 INT8/UINT8 的模型 -> 喂 uint8 原始[0,255]
    //       (运行时自己按张量的 zp/scale 量化, 图里的 /255 也已经固化)
    //   逻辑输入是 FLOAT32/FLOAT16 的模型 -> 喂 float32 [0,1]  (ONNX 契约)
    //       喂 [0,255] 会让输出彻底乱掉(实测: 每帧 6000+ 个假框)
    // ⚠️ rknn_tensor_type: FLOAT32=0 FLOAT16=1 INT8=2 UINT8=3
    //   FP16 模型的逻辑输入是 FLOAT16(1), 不是 FLOAT32 —— 只判 FLOAT32 会漏。
    const bool quantized = (attr_logic.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC);
    const bool is_float = (attr_logic.type == RKNN_TENSOR_FLOAT32 ||
                           attr_logic.type == RKNN_TENSOR_FLOAT16);
    std::string mode = a.input_mode;
    if (mode == "auto") {
        // FLOAT16 的逻辑输入要喂 FLOAT16 数据(见下面 float16 分支的说明)
        mode = (attr_logic.type == RKNN_TENSOR_FLOAT16) ? "float16"
             : (is_float ? "float_scaled" : "uint8");
    }
    fprintf(stderr, "[eval] 输入模式: %s (模型量化=%s 浮点=%s)\n", mode.c_str(),
            quantized ? "是" : "否", is_float ? "是" : "否");

    // ---- 输入缓冲 ----
    const size_t buf_size = static_cast<size_t>(model_w) * model_h * 3;
    int model_fd = rk::dma_buf_alloc(buf_size, true);
    void* model_ptr = nullptr;
    if (model_fd >= 0) {
        model_ptr = rk::dma_buf_map(model_fd, buf_size);
        if (model_ptr == MAP_FAILED) { model_ptr = nullptr; ::close(model_fd); model_fd = -1; }
    }
    if (!model_ptr) {
        model_ptr = aligned_alloc(64, buf_size);
        memset(model_ptr, 114, buf_size);
        model_fd = -1;
    }
    // ★★★ 关键: 只把像素写进 dma-buf 是不够的, 必须把这段内存【绑成 NPU 的
    //   输入张量】(rknn_create_mem_from_fd + rknn_set_io_mem), 否则
    //   rknn_run(ctx, nullptr) 读的是一块没绑过的内存 -> 输出全是噪声,
    //   检测框一个都没有(实测: 街景那张图上连生产模型都是 0 检测)。
    //   绑定方式和生产链路完全一致(见 main.cc 的 4.x 节)。
    bool bound = false;
    rknn_tensor_mem* in_mem = nullptr;
    if (model_fd >= 0 && mode == "uint8") {
        in_mem = rknn_create_mem_from_fd(rctx, model_fd, model_ptr, buf_size, 0);
        if (!in_mem) {
            fprintf(stderr, "[eval] rknn_create_mem_from_fd 失败, 退回 rknn_inputs_set\n");
        } else {
            rknn_tensor_attr bind_attr;
            memset(&bind_attr, 0, sizeof(bind_attr));
            if (use_native) {
                bind_attr = attr_native;
                bind_attr.pass_through = 1;
            } else {
                bind_attr = attr_logic;
                bind_attr.pass_through = 0;
                bind_attr.type = RKNN_TENSOR_UINT8;
                bind_attr.fmt = RKNN_TENSOR_NHWC;
                bind_attr.n_dims = 4;
                bind_attr.dims[0] = 1;
                bind_attr.dims[1] = static_cast<uint32_t>(model_h);
                bind_attr.dims[2] = static_cast<uint32_t>(model_w);
                bind_attr.dims[3] = 3;
            }
            const int bret = rknn_set_io_mem(rctx, in_mem, &bind_attr);
            if (bret != 0) {
                fprintf(stderr, "[eval] rknn_set_io_mem 失败(%d), 退回 rknn_inputs_set\n", bret);
                rknn_destroy_mem(rctx, in_mem);
                in_mem = nullptr;
            } else {
                bound = true;
                fprintf(stderr, "[eval] NPU 输入已绑定 dma-buf (pass_through=%d) -> 零拷贝\n",
                        bind_attr.pass_through);
            }
        }
    }
    const bool zero_copy = bound;

    // 浮点模式要额外一块 float 缓冲(float32 或 float16)
    std::vector<float> fp32;
    std::vector<uint16_t> fp16;
    if (mode == "float_scaled" || mode == "float_raw" || mode == "float16") {
        fp32.assign(static_cast<size_t>(model_w) * model_h * 3, 0.f);
    }
    if (mode == "float16") fp16.assign(fp32.size(), 0);
    rknn_input rin;
    memset(&rin, 0, sizeof(rin));
    rin.index = 0;
    rin.pass_through = 0;

    // ---- 跑图 ----
    std::vector<std::string> files = list_images(a.images);
    if (a.limit > 0 && static_cast<int>(files.size()) > a.limit) files.resize(a.limit);
    if (files.empty()) { fprintf(stderr, "目录里没有图片: %s\n", a.images.c_str()); return 1; }

    FILE* outf = a.out.empty() ? nullptr : fopen(a.out.c_str(), "w");
    if (outf) {
        fprintf(outf, "{\"model\":\"%s\",\"input_mode\":\"%s\",\"results\":[",
                a.model.c_str(), mode.c_str());
    }
    int n_img = 0, n_det = 0;
    double sum_ms = 0;
    for (const auto& path : files) {
        cv::Mat bgr = cv::imread(path);
        if (bgr.empty()) continue;
        const int bw = bgr.cols, bh = bgr.rows;

        // BGR -> NV12(OpenCV)
        // ⚠️ 色彩范围: 必须用【有限范围(studio range)】的 YUV, 也就是
        //    COLOR_BGR2YUV (Y 落在 16~235), 不能用 COLOR_BGR2YUV_I420
        //    (那是全范围/JPEG 风格, Y 落在 0~255)。
        //    生产链路的源是相机的 NV12(有限范围), RGA 按 full_range=false
        //    做 16~235 -> 0~255 的扩展; 这里如果塞进去全范围的 NV12,
        //    RGA 会再扩一次 -> 画面被拉灰, 模型基本检测不到东西
        //    (实测: 生产模型在街上那张图上 0 检测, 换成有限范围就正常了)。
        cv::Mat yuv;
        cv::cvtColor(bgr, yuv, cv::COLOR_BGR2YUV);
        std::vector<uint8_t> nv12(static_cast<size_t>(bw) * bh * 3 / 2);
        {
            const size_t ysz = static_cast<size_t>(bw) * bh;
            const size_t csz = ysz / 4;
            memcpy(nv12.data(), yuv.data, ysz);
            uint8_t* uv = nv12.data() + ysz;
            const uint8_t* u = yuv.data + ysz;
            const uint8_t* v = u + csz;
            for (size_t p = 0; p < csz; p++) { uv[p * 2] = u[p]; uv[p * 2 + 1] = v[p]; }
        }

        // NV12 -> RGB888(模型输入缓冲)
        rk::RgaImage src;
        src.fd = -1;
        src.vir_addr = nv12.data();
        src.width = bw;
        src.height = bh;
        src.wstride = bw;
        src.hstride = bh;
        src.format = RK_FORMAT_YCbCr_420_SP;
        src.planar_yuv = false;
        src.full_range = false;
        rk::RgaImage dst;
        dst.fd = model_fd;
        dst.vir_addr = model_ptr;
        dst.width = model_w;
        dst.height = model_h;
        dst.wstride = model_w * 3;
        dst.hstride = model_h;
        dst.format = RK_FORMAT_RGB_888;
        float scale = 1.f;
        int pad_x = 0, pad_y = 0;
        if (a.use_rga) {
            if (rk::rga_nv12_to_rgb888(src, dst, false, &scale, &pad_x, &pad_y) != 0) {
                fprintf(stderr, "[eval] %s 预处理失败, 跳过\n", path.c_str());
                continue;
            }
        } else {
            cv::Mat rgb;
            cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
            cv::Mat resized;
            cv::resize(rgb, resized, cv::Size(model_w, model_h));
            memcpy(model_ptr, resized.data, buf_size);
        }

        // ---- NPU ----
        const auto t0 = std::chrono::steady_clock::now();
        if (mode == "uint8" && zero_copy) {
            ret = rknn_run(rctx, nullptr);
        } else {
            const uint8_t* s8 = static_cast<const uint8_t*>(model_ptr);
            if (mode == "float16") {
                // ★ FP16 模型的逻辑输入是 FLOAT16 —— 直接喂 FLOAT32 时
                //   运行时对"带量化参数的输入张量"处理不对, 输出全是噪声
                //   (实测: 每帧 8874 个恒定的假框)。喂 FLOAT16 就正常。
                const uint8_t* s8 = static_cast<const uint8_t*>(model_ptr);
                const bool nhwc = (attr_logic.fmt == RKNN_TENSOR_NHWC);
                for (int y = 0; y < model_h; y++)
                    for (int x = 0; x < model_w; x++)
                        for (int c = 0; c < 3; c++) {
                            const float v = s8[(y * model_w + x) * 3 + c] * (1.0f / 255.0f);
                            if (nhwc) fp32[(y * model_w + x) * 3 + c] = v;
                            else      fp32[(c * model_h + y) * model_w + x] = v;
                        }
                for (size_t i = 0; i < fp32.size(); i++) fp16[i] = float_to_half(fp32[i]);
                rin.buf = fp16.data();
                rin.size = static_cast<uint32_t>(fp16.size() * sizeof(uint16_t));
                rin.type = RKNN_TENSOR_FLOAT16;
                rin.fmt = (attr_logic.fmt == RKNN_TENSOR_NHWC) ? RKNN_TENSOR_NHWC : RKNN_TENSOR_NCHW;
            } else if (mode == "float_scaled" || mode == "float_raw") {
                const float sc = (mode == "float_scaled") ? (1.0f / 255.0f) : 1.0f;
                // ★ 布局必须跟模型【逻辑输入】的 fmt 一致(NHWC 还是 NCHW),
                //   填错的话 inputs_set 会失败, 而模型拿到全 0 ->
                //   每张图输出都一样(看着像"模型只输出固定框")。
                const bool nhwc = (attr_logic.fmt == RKNN_TENSOR_NHWC);
                for (int y = 0; y < model_h; y++)
                    for (int x = 0; x < model_w; x++)
                        for (int c = 0; c < 3; c++) {
                            const float v = s8[(y * model_w + x) * 3 + c] * sc;
                            if (nhwc)
                                fp32[(y * model_w + x) * 3 + c] = v;
                            else
                                fp32[(c * model_h + y) * model_w + x] = v;
                        }
                rin.buf = fp32.data();
                rin.size = static_cast<uint32_t>(fp32.size() * sizeof(float));
                rin.type = RKNN_TENSOR_FLOAT32;
                rin.fmt = nhwc ? RKNN_TENSOR_NHWC : RKNN_TENSOR_NCHW;
            } else {
                rin.buf = const_cast<uint8_t*>(s8);
                rin.size = static_cast<uint32_t>(buf_size);
                rin.type = RKNN_TENSOR_UINT8;
                rin.fmt = RKNN_TENSOR_NHWC;
            }
            ret = rknn_inputs_set(rctx, 1, &rin);
            if (ret != 0) {
                fprintf(stderr, "[eval] %s inputs_set 失败(%d) mode=%s\n",
                        path.c_str(), ret, mode.c_str());
                continue;
            }
            ret = rknn_run(rctx, nullptr);
        }
        std::vector<rknn_output> outs(io_num.n_output > 0 ? io_num.n_output : 1);
        memset(outs.data(), 0, sizeof(rknn_output) * outs.size());
        for (size_t i = 0; i < outs.size(); i++) {
            outs[i].want_float = 1;
            outs[i].is_prealloc = 0;
            outs[i].index = static_cast<uint32_t>(i);
        }
        if (ret == 0) {
            ret = rknn_outputs_get(rctx, static_cast<uint32_t>(outs.size()), outs.data(), nullptr);
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        sum_ms += ms;
        if (ret != 0) { fprintf(stderr, "[eval] %s 推理失败(%d)\n", path.c_str(), ret); continue; }

        // ---- 后处理 ----
        std::vector<OutTensor> ts(outs.size());
        for (size_t i = 0; i < outs.size(); i++) {
            ts[i].data    = outs[i].buf;
            ts[i].is_int8 = false;      // 直接要 float 输出, 省得再量化回去
            ts[i].zp      = out_attrs[i].zp;
            ts[i].scale   = out_attrs[i].scale;
            ts[i].n_elems = out_attrs[i].n_elems;
            ts[i].n_dims  = out_attrs[i].n_dims;
            for (uint32_t k = 0; k < out_attrs[i].n_dims && k < 4; k++)
                ts[i].dims[k] = out_attrs[i].dims[k];
        }
        BoxMapper mapper;
        mapper.img_w = bw;
        mapper.img_h = bh;
        mapper.mode = BoxMapper::STRETCH;
        mapper.scale_x = static_cast<float>(bw) / model_w;
        mapper.scale_y = static_cast<float>(bh) / model_h;
        std::vector<YoloBox> boxes =
            yolov8_postprocess(ts.data(), static_cast<int>(ts.size()), a.conf, a.nms, mapper);
        rknn_outputs_release(rctx, static_cast<uint32_t>(outs.size()), outs.data());

        // ---- 写 JSON ----
        n_img++;
        n_det += static_cast<int>(boxes.size());
        if (outf) {
            const size_t slash = path.find_last_of('/');
            const std::string fname =
                (slash == std::string::npos) ? path : path.substr(slash + 1);
            fprintf(outf, "%s{\"file\":\"%s\",\"dets\":[", n_img > 1 ? "," : "", fname.c_str());
            for (size_t i = 0; i < boxes.size(); i++) {
                const YoloBox& b = boxes[i];
                fprintf(outf, "%s{\"c\":%d,\"s\":%.4f,\"b\":[%.1f,%.1f,%.1f,%.1f]}",
                        i ? "," : "", b.cls_id, b.score, b.x1, b.y1, b.x2, b.y2);
            }
            fprintf(outf, "]}");
        }
        if (n_img % 200 == 0) fprintf(stderr, "[eval] %d/%d 张, 检测 %d 个\n", n_img,
                                      static_cast<int>(files.size()), n_det);
    }
    if (outf) {
        fprintf(outf, "]}\n");
        fclose(outf);
    }
    fprintf(stderr, "[eval] 完成: %d 张图, %d 个检测, 平均 %.2f ms/帧(仅 NPU)\n",
            n_img, n_det, n_img ? sum_ms / n_img : 0.0);
    if (in_mem) rknn_destroy_mem(rctx, in_mem);
    if (model_fd >= 0) {
        rk::dma_buf_unmap(model_ptr, buf_size);
        ::close(model_fd);
    } else {
        free(model_ptr);
    }
    rknn_destroy(rctx);
    return 0;
}
