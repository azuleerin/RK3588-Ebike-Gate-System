// src/minimal/unit_common.cc —— 最小化单元的公共工具
//
// 这里只放"观测"和"纯 CPU 工具", 不含任何硬件调用,
// 保证单元之间的隔离边界清晰可查。

#include "rk/minimal/unit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rk/fd_ledger.h"
#include "rk/log.h"
#include "rk/mem_guard.h"
#include "rk/stack_guard.h"

namespace rk {
namespace minimal {

int run_rga_unit(const UnitOptions& opt) {
    RK_LOG_ERROR("unit", "rga",
                 "rga-only 尚未实现 —— 见方案 step/04 "
                 "(届时会在本单元里做 wstride 单位 A/B 对照, 见方案 7.2)");
    (void)opt;
    return 2;
}

void audit_now(const char* why) {
    RK_LOG_INFO("audit", "run", "===== 巡检开始 (%s) =====", why ? why : "-");
    // ① fd 台账对账: 抓"未登记"与"已消失"的 fd
    FdLedger::dump_open();
    // ② canary 巡检: 抓越界写
    const size_t bad = MemGuard::check_canaries();
    if (bad > 0) {
        RK_LOG_ERROR("audit", "run", "发现 %zu 个块被越界破坏!", bad);
        MemGuard::dump_blocks();
    }
    // ③ 内存与栈
    MemGuard::dump_by_tag();
    StackGuard::dump();
    RK_LOG_INFO("audit", "run", "===== 巡检结束 =====");
}

uint8_t* load_file(const std::string& path, size_t* out_size, const char* tag) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        RK_LOG_ERROR("file", "load", "打不开 %s", path.c_str());
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        RK_LOG_ERROR("file", "load", "%s 大小非法 (%ld)", path.c_str(), sz);
        fclose(f);
        return nullptr;
    }
    uint8_t* buf = static_cast<uint8_t*>(MemGuard::alloc(static_cast<size_t>(sz), tag));
    if (!buf) {
        fclose(f);
        return nullptr;
    }
    const size_t rd = fread(buf, 1, static_cast<size_t>(sz), f);
    fclose(f);
    if (rd != static_cast<size_t>(sz)) {
        RK_LOG_ERROR("file", "load", "%s 读取不完整 (%zu/%ld)", path.c_str(), rd, sz);
        MemGuard::free_ref(buf);
        return nullptr;
    }
    if (out_size) *out_size = rd;
    RK_LOG_INFO("file", "load", "读取 %s -> %zu 字节 (受控内存)", path.c_str(), rd);
    return buf;
}

uint8_t* nv12_to_rgb888(const uint8_t* nv12, int width, int height, const char* tag) {
    if (!nv12 || width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        RK_LOG_ERROR("img", "nv12", "参数非法 w=%d h=%d", width, height);
        return nullptr;
    }
    const size_t y_size = static_cast<size_t>(width) * height;
    const size_t out_size = y_size * 3;

    uint8_t* rgb = static_cast<uint8_t*>(MemGuard::alloc(out_size, tag));
    if (!rgb) return nullptr;

    // NV12: Y 平面 (w*h) + 交错 UV 平面 (w*h/2, 4:2:0)
// BT.601 有限范围换算(与 OpenCV COLOR_YUV2RGB_I420 一致):
    //   R = 1.164*(Y-16)                 + 1.596*(V-128)
//   G = 1.164*(Y-16) - 0.392*(U-128) - 0.813*(V-128)
//   B = 1.164*(Y-16) + 2.017*(U-128)
    for (int y = 0; y < height; ++y) {
        const uint8_t* yrow = nv12 + static_cast<size_t>(y) * width;
        uint8_t* orow = rgb + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            const int Y = yrow[x];
            const int U = nv12[y_size + (static_cast<size_t>(y / 2) * width) + (x & ~1)] - 128;
            const int V = nv12[y_size + (static_cast<size_t>(y / 2) * width) + (x & ~1) + 1] - 128;
            int r = (298 * (Y - 16) + 409 * V + 128) >> 8;
            int g = (298 * (Y - 16) - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * (Y - 16) + 516 * U + 128) >> 8;
            orow[x * 3 + 0] = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
            orow[x * 3 + 1] = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
            orow[x * 3 + 2] = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
    return rgb;
}

uint8_t* make_test_pattern(int width, int height, int frame_index, const char* tag) {
    const size_t n = static_cast<size_t>(width) * height * 3;
    uint8_t* rgb = static_cast<uint8_t*>(MemGuard::alloc(n, tag));
    if (!rgb) return nullptr;
    for (int y = 0; y < height; ++y) {
        uint8_t* row = rgb + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            // 棋盘 + 渐变 + 帧号色带: 肉眼一看就知道颜色对不对、有没有花屏
            const bool chk = ((x / 32) + (y / 32) + frame_index) & 1;
            row[x * 3 + 0] = chk ? 230 : 30;
            row[x * 3 + 1] = static_cast<uint8_t>(x * 255 / (width > 1 ? width - 1 : 1));
            row[x * 3 + 2] = static_cast<uint8_t>(y * 255 / (height > 1 ? height - 1 : 1));
        }
    }
    return rgb;
}

void TimingStat::push(double v) {
    if (n == 0) {
        min = max = v;
    } else {
        if (v < min) min = v;
        if (v > max) max = v;
    }
    sum += v;
    ++n;
    // 只保留最近 4096 个样本算分位数, 防止长跑时内存无限增长
    if (samples.size() < 4096) samples.push_back(v);
    else samples[n % 4096] = v;
}

double TimingStat::pct(double p) const {
    if (samples.empty()) return 0;
    std::vector<double> t = samples;
    std::sort(t.begin(), t.end());
    size_t i = static_cast<size_t>(p / 100.0 * t.size());
    if (i >= t.size()) i = t.size() - 1;
    return t[i];
}

void TimingStat::reset() {
    sum = 0;
    n = 0;
    min = 0;
    max = 0;
    samples.clear();
}

}  // namespace minimal
}  // namespace rk
