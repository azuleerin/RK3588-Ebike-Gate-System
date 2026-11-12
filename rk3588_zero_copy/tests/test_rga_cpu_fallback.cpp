// test_rga_cpu_fallback.cpp —— 验证「RGA 不可用时的 CPU 兜底」数值正确
//
// 场景: 板子上 /dev/rga 打不开时 librga 会自己段错误, 程序要能自动降级到
//       OpenCV 的 CPU 转换。本测试在【没有 /dev/rga 的环境】里跑(容器/PC),
//       正好强制走 CPU 兜底分支, 再和 Python 参考实现逐像素对拍。
//
// 输出: <prefix>_cpu.bin  (dst 的完整内容, 按 dst.wstride 逐行)
// 编译:
//   g++ -O2 -std=c++17 -Wall -Wextra -o /tmp/trga
//       tests/test_rga_cpu_fallback.cpp src/rga_convert.cc src/safe_probe.cc
//       -Iinclude -Ithird_party/include/rga $(pkg-config --cflags --libs opencv4)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rga_convert.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <out_prefix>\n", argv[0]);
        return 1;
    }
    const std::string prefix = argv[1];

    // 故意用【带 padding 的 stride】, 模拟 MPP/FFmpeg 的对齐缓冲:
    //   wstride = 320 + 16, hstride = 240 + 16
    const int sw = 320, sh = 240;
    const int s_wstride = sw + 16, s_hstride = sh + 16;
    const int dw = 640, dh = 640;
    const int d_wstride = dw * 3 + 64;

    // ---- 造一块确定内容的 NV12 源 ----
    std::vector<uint8_t> src(static_cast<size_t>(s_wstride) * s_hstride * 3 / 2, 0);
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            src[static_cast<size_t>(y) * s_wstride + x] = static_cast<uint8_t>((x + y * 3) & 0xFF);
        }
    }
    // NV12 的 UV plane: 每行 sw 字节(= sw/2 个 chroma 采样, U/V 交织),
    //   一行 UV 对应两行 Y。⚠️ 行宽是 sw 字节, 不是 2*sw。
    uint8_t* uv = src.data() + static_cast<size_t>(s_wstride) * s_hstride;
    for (int y = 0; y < sh / 2; ++y) {
        for (int x = 0; x < sw / 2; ++x) {
            uv[static_cast<size_t>(y) * s_wstride + x * 2 + 0] = static_cast<uint8_t>((x * 2) & 0xFF);
            uv[static_cast<size_t>(y) * s_wstride + x * 2 + 1] = static_cast<uint8_t>((y * 5) & 0xFF);
        }
    }

    std::vector<uint8_t> dst(static_cast<size_t>(d_wstride) * dh, 0xAB);

    rk::RgaImage s;
    s.fd = -1;
    s.vir_addr = src.data();
    s.width = sw;
    s.height = sh;
    s.wstride = s_wstride;
    s.hstride = s_hstride;
    s.format = RK_FORMAT_YCbCr_420_SP;
    // 色度平面现在是独立字段(不再默认"紧跟在 Y 平面后面"), 必须显式给出来,
    // 否则转换层会按"没有色度平面"处理(填中性 128), 对拍会整片对不上。
    s.u_plane = uv;

    rk::RgaImage d;
    d.fd = -1;
    d.vir_addr = dst.data();
    d.width = dw;
    d.height = dh;
    d.wstride = d_wstride;
    d.hstride = dh;
    d.format = RK_FORMAT_RGB_888;

    fprintf(stderr, "RGA 可用性(调用前): %s\n", rk::rga_available() ? "true" : "false");
    const int rc = rk::rga_nv12_to_rgb888(s, d, /*letterbox=*/false, nullptr, nullptr, nullptr);
    fprintf(stderr, "rga_nv12_to_rgb888 返回 %d, RGA 可用性(调用后): %s\n",
            rc, rk::rga_available() ? "true" : "false");
    if (rc != 0) {
        fprintf(stderr, "转换失败\n");
        return 1;
    }

    // 存出 dst 的完整内容(含 stride padding), 供 Python 对拍
    std::string out = prefix + "_cpu.bin";
    FILE* f = fopen(out.c_str(), "wb");
    if (!f) { fprintf(stderr, "写不了 %s\n", out.c_str()); return 1; }
    fwrite(dst.data(), 1, dst.size(), f);
    fclose(f);

    // 顺便存一下元信息
    std::string meta = prefix + "_meta.txt";
    FILE* fm = fopen(meta.c_str(), "w");
    if (fm) {
        fprintf(fm, "%d %d %d %d %d %d %d\n", sw, sh, s_wstride, s_hstride, dw, dh, d_wstride);
        fclose(fm);
    }
    fprintf(stderr, "已写出 %s\n", out.c_str());
    return 0;
}
