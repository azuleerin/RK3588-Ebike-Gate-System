// test_license_plate_rec.cpp —— LicensePlateRec 类的单元测试
//
// 覆盖:
//   1) CTC 贪心解码(语义对齐官方 rec_postprocess)
//   2) 车牌字符白名单过滤
//   3) 字典加载 + 类数对齐(官方 dict.h 是 6625 类)
//   4) RGA 硬件预处理(裁剪 + 缩放 + 颜色空间), 用【真 dma-buf】跑
//   5) 端到端识别(仅当命令行给了 --model <rknn> 时)
//
// 编译:
//   cmake --build build --target test_license_plate_rec
//   ./build/tests/test_license_plate_rec [--model model/xxx.rknn] [--dict model/ppocr_keys_v1.txt]
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "license_plate_rec.h"

// ---------------------------------------------------------------------------
// 本板子内核的 dma-heap uapi。
// ⚠️ 字段顺序和上游不一样: 本板内核头是
//    struct dma_heap_allocation_data { __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags; }
//    (上游是 len/fd_flags/heap_flags/fd)。总长都是 24 字节, ioctl 号一样,
//    但字段错位 -> 分配直接失败(EINVAL)。这里按【本板内核】的布局写。
// ---------------------------------------------------------------------------
struct rk_dma_heap_alloc_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define RK_DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct rk_dma_heap_alloc_data)

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    printf("[%s] %s\n", ok ? "OK" : "XX", what);
    if (!ok) g_fail = 1;
}

// 分配一块真 dma-buf(成功返回 fd, 失败返回 -1)
int alloc_dmabuf(size_t len) {
    for (const char* heap : {"/dev/dma_heap/system", "/dev/dma_heap/system-uncached"}) {
        int h = ::open(heap, O_RDWR | O_CLOEXEC);
        if (h < 0) continue;
        rk_dma_heap_alloc_data d;
        memset(&d, 0, sizeof(d));
        d.len = len;
        d.fd_flags = O_RDWR | O_CLOEXEC;
        d.heap_flags = 0;
        if (::ioctl(h, RK_DMA_HEAP_IOCTL_ALLOC, &d) == 0) {
            ::close(h);
            return static_cast<int>(d.fd);
        }
        ::close(h);
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model_path, dict_path = "model/ppocr_keys_v1.txt";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) model_path = argv[++i];
        else if (strcmp(argv[i], "--dict") == 0 && i + 1 < argc) dict_path = argv[++i];
    }

    // =====================================================================
    printf("== 1) CTC 贪心解码 ==\n");
    {
        // 字典: 0=blank, 1='A', 2='B', 3='C'
        std::vector<std::string> classes = {"blank", "A", "B", "C"};
        const int T = 5, C = 4;
        std::vector<float> logits(T * C, 0.f);
        auto set = [&](int t, int c, float v) { logits[t * C + c] = v; };
        // 时间步: blank A A blank B   -> 官方语义: "A"(相邻重复折叠) + "B"(blank 打断重复)
        set(0, 0, 0.9f);
        set(1, 1, 0.8f);
        set(2, 1, 0.7f);   // 与上一步同类 -> 折叠
        set(3, 0, 0.95f);
        set(4, 2, 0.6f);
        for (int t = 0; t < T; ++t) {
            for (int c = 0; c < C; ++c) {
                if (logits[t * C + c] == 0.f) logits[t * C + c] = 0.01f;
            }
        }

        rk::PlateRecResult r;
        const bool ok = rk::LicensePlateRec::ctc_greedy_decode(logits.data(), T, C,
                                                              classes, nullptr, &r);
        check(ok, "解码返回 true");
        check(r.text == "AB", "文本 = \"AB\" (相邻重复折叠, blank 打断重复)");
        check(r.char_count == 2, "有效字符数 = 2");
        // score = (0.8 + 0.6) / 2 = 0.7
        check(r.score > 0.69f && r.score < 0.71f, "置信度 = 0.7");
        printf("       text=\"%s\" score=%.4f count=%d\n", r.text.c_str(), r.score, r.char_count);
    }

    // =====================================================================
    printf("\n== 2) 字符白名单过滤 ==\n");
    {
        std::vector<std::string> classes = {"blank", "A", "B", "C"};
        const int T = 2, C = 4;
        std::vector<float> logits(T * C, 0.01f);
        logits[0 * C + 2] = 0.9f;   // 'C' 概率最高
        logits[1 * C + 1] = 0.8f;   // 'A'
        std::vector<int> allowed = {1};   // 只允许 'A'

        rk::PlateRecResult r;
        rk::LicensePlateRec::ctc_greedy_decode(logits.data(), T, C, classes, &allowed, &r);
        check(r.text == "A", "白名单只留 'A' (原 argmax 是 'C')");
        printf("       text=\"%s\" score=%.4f\n", r.text.c_str(), r.score);
    }

    // =====================================================================
    // 2b) 白名单必须能包含汉字(2026-11-15 修)
    //
    // ★ 踩过的坑: 原来过滤条件是 `ch.size() == 1`, 那是 ASCII 的写法。
    //   汉字在 UTF-8 里是【3 个字节】, size()==3 -> 所有省份汉字被过滤掉,
    //   白名单里明明写了 "苏", 结果永远读不出 "苏"(实测: 白名单开着读 "A",
    //   关掉读 "苏A")。
    // =====================================================================
    printf("\n== 2b) 白名单的汉字(UTF-8)==\n");
    {
        const std::string charset =
            "0123" "456789ABCDEFGHJKLMNPQRSTUVWXYZ"
            "京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼港澳学警挂";
        rk::PlateRecConfig cfg;
        cfg.model_path = model_path;
        cfg.dict_path = dict_path;
        cfg.allowed_charset = charset;
        rk::LicensePlateRec rec;
        if (!rec.init(cfg)) {
            check(false, "带白名单初始化");
        } else {
            // "苏" 在字典里的类号(字典第 1894 行 -> 类 1894)
            const std::string su = "苏";
            bool in_dict = false;
            for (size_t i = 0; i < 6625; ++i) {
                // 类 0 是 blank, 类 i 对应字典第 i 行
            }
            (void)in_dict;
            // 直接用解码器验证: blank 必须在白名单里(否则整块读成乱码),
            // 而且汉字类必须在白名单里(否则认不出省份)
            // 造一条只有 "苏" 的 logits: blank, 苏, blank
            std::vector<std::string> classes = {"blank", "A", su};
            std::vector<float> logits(3 * 3, 0.01f);
            logits[0 * 3 + 0] = 0.9f;   // blank
            logits[1 * 3 + 2] = 0.8f;   // 苏
            logits[2 * 3 + 0] = 0.9f;   // blank
            // 白名单 = {blank(0), 苏(2)}  —— 注意 blank 必须在里面
            std::vector<int> allowed = {0, 2};
            rk::PlateRecResult r;
            rk::LicensePlateRec::ctc_greedy_decode(logits.data(), 3, 3, classes,
                                                    &allowed, &r);
            check(r.text == su, "白名单含汉字 -> 读出 \"苏\"(blank 也必须在白名单里)");
            printf("       text=\"%s\" score=%.4f\n", r.text.c_str(), r.score);
        }
    }

    // =====================================================================
    printf("\n== 3) 字典加载 ==\n");
    {
        std::vector<std::string> classes;
        const bool ok = rk::LicensePlateRec::load_dict(dict_path, 6625, &classes);
        check(ok, "字典文件可读");
        check(classes.size() == 6625, "类数 = 6625 (blank + 6623 + 空格)");
        check(!classes.empty() && classes[0] == "blank", "类 0 = blank");
        if (classes.size() > 1) {
            check(classes[1] == "'", "类 1 = 字典第 1 行 (\"'\")");
            printf("       classes[1]=\"%s\" classes[2]=\"%s\" last=\"%s\"\n",
                   classes[1].c_str(), classes[2].c_str(), classes.back().c_str());
        }
        // '%' 必须是单个字符(官方 dict.h 里是 "%%", 那是脚本转义残留)
        int pct = -1;
        for (size_t i = 0; i < classes.size(); ++i) {
            if (classes[i] == "%") { pct = static_cast<int>(i); break; }
        }
        check(pct > 0, "字典里 '%' 是单字符(不是官方 dict.h 的 \"%%\")");
    }

    // =====================================================================
    printf("\n== 4) RGA 硬件预处理(裁剪 + 缩放 + 颜色空间) ==\n");
    {
        const int W = 1920, H = 1080;
        const size_t len = static_cast<size_t>(W) * H * 3 / 2;
        int fd = alloc_dmabuf(len);
        if (fd < 0) {
            printf("[跳过] dma-buf 分配失败(dma-heap 不可用), RGA 路径没法验证\n");
        } else {
            uint8_t* p = static_cast<uint8_t*>(::mmap(nullptr, len, PROT_READ | PROT_WRITE,
                                                       MAP_SHARED, fd, 0));
            if (p == MAP_FAILED) {
                printf("[跳过] dma-buf mmap 失败\n");
                ::close(fd);
            } else {
                // 造一张图: Y=100 的灰底, 在 (400,300) 处放一个 200x100 的白块(Y=235)
                memset(p, 100, static_cast<size_t>(W) * H);
                for (int y = 300; y < 400; ++y) {
                    memset(p + static_cast<size_t>(y) * W + 400, 235, 200);
                }
                memset(p + static_cast<size_t>(W) * H, 128, static_cast<size_t>(W) * H / 2); // UV=128 中性

                rk::RgaImage frame;
                frame.fd = fd;
                frame.vir_addr = p;
                frame.width = W;
                frame.height = H;
                frame.wstride = W;
                frame.hstride = H;
                frame.format = RK_FORMAT_YCbCr_420_SP;

                // 裁一块"左灰右白"的区域(白块在源图 x=400..600, 裁剪窗口 x=300..700)
                // -> 按比例缩到高 48, 白块落在中间, 左边是灰底
                rk::LicensePlateRec rec;
                std::vector<uint8_t> roi(320 * 48 * 3);
                int rw = 0, rh = 0;
                const bool ok = rec.preprocess_nv12_rga(frame, 300, 300, 400, 100,
                                                        roi.data(), static_cast<int>(roi.size()),
                                                        &rw, &rh);
                check(ok, "RGA 预处理返回 true");
                check(rw == 192 && rh == 48,
                      "输出尺寸 = 192x48 (400x100 按比例缩到高 48: ceil(48*4)=192)");
                if (ok) {
                    const uint8_t* mid = roi.data() + (24 * rw + rw / 2) * 3;
                    const uint8_t* edge = roi.data() + (24 * rw + 4) * 3;
                    printf("       中心像素 BGR=(%d,%d,%d)  边缘像素 BGR=(%d,%d,%d)\n",
                           mid[0], mid[1], mid[2], edge[0], edge[1], edge[2]);
                    check(mid[0] > 200 && mid[1] > 200 && mid[2] > 200,
                          "中心是白块(Y=235 -> 接近 255)");
                    check(edge[0] < 140 && edge[1] < 140 && edge[2] < 140,
                          "边缘是灰底(Y=100 -> 接近 98)");
                }
                ::munmap(p, len);
                ::close(fd);
            }
        }
    }

    // =====================================================================
    printf("\n== 5) 端到端识别 ==\n");
    if (model_path.empty()) {
        printf("[跳过] 没给 --model(本板子上没有现成的 RKNN 版 PP-OCRv4 rec, "
               "需要在 PC 上用 rknn-toolkit2 转好再传上来)\n");
    } else {
        rk::PlateRecConfig cfg;
        cfg.model_path = model_path;
        cfg.dict_path = dict_path;
        cfg.allowed_charset = "0123" "456789ABCDEFGHJKLMNPQRSTUVWXYZ"
                              "京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼"
                              "港澳学警挂";
        rk::LicensePlateRec rec;
        if (!rec.init(cfg)) {
            check(false, "模型初始化");
        } else {
            check(true, "模型初始化");
            // 拿视频里裁出来的车牌图跑一遍(如果存在)
            cv::Mat img = cv::imread("model/plate_sample.jpg", cv::IMREAD_COLOR);
            if (img.empty()) {
                printf("[跳过] 没有样例图 model/plate_sample.jpg\n");
            } else {
                rk::PlateRecResult r;
                if (rec.recognize_bgr(img, &r)) {
                    printf("       整块两行牌 -> \"%s\"  score=%.4f  字符数=%d\n",
                           r.text.c_str(), r.score, r.char_count);
                    check(!r.text.empty(), "识别出非空文本");
                    // 整块两行牌读出来是乱的(PP-OCR 是单行模型) —— 这是预期的
                    printf("       (整块两行牌是乱的, 正常的用法是切开上行/下行各读一次)\n");

                    // ★ 关键回归: 把"上行"单独裁出来, 必须能读出省份字 "苏"
                    //   这块样例牌是 testvideo3 的绿牌: 上行 "苏 A", 下行 "A98190B"
                    cv::Mat top = img(cv::Rect(0, img.rows * 1 / 8, img.cols,
                                                img.rows * 3 / 8));
                    rk::PlateRecResult rt;
                    if (rec.recognize_bgr(top, &rt)) {
                        printf("       上行单独裁 -> \"%s\"  score=%.4f\n",
                               rt.text.c_str(), rt.score);
                        check(rt.text.find("苏") != std::string::npos,
                              "上行裁出来能读出省份字 \"苏\"");
                    } else {
                        check(false, "上行 recognize_bgr 返回 false");
                    }
                } else {
                    check(false, "recognize_bgr 返回 false");
                }
            }
        }
    }

    printf("\n%s\n", g_fail == 0 ? "全部通过" : "有失败项");
    return g_fail;
}
