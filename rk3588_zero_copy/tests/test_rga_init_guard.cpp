// test_rga_init_guard.cpp —— 验证「RGA 初始化」的异常处理与降级判断
//
// 背景: librga 1.9.1 的 rga_init() 是【第一次调 imcheck/improcess 时】才触发的,
// 它内部 open("/dev/rga")。open 失败时它不干净返回, 而是继续解引用未初始化的
// 全局句柄 -> SIGSEGV。原来的做法是把这整个调用包进 SafeProbe, 结果日志里只
// 有一句 "librga 初始化时崩溃(SIGSEGV 已捕获)" —— 到底是权限问题、节点不存在,
// 还是驱动没加载, 看不出来。
//
// 现在改成三步:
//     ① 自己 open() 设备节点做预检 -> 打不开就报明确原因, 干净降级,
//        根本不碰 librga, 也就不会崩。
//     ② 预检通过才让 librga 初始化 -> 真崩了报 "rga_init() 内部崩溃"。
//     ③ 两种情况都用同一句 "RGA 不可用，原因：xxx" 收口。
//
// 本测试覆盖三个场景(每个场景必须是一个独立进程 —— 状态机是进程级的):
//     missing   : RGA_DEVICE 指向不存在的节点   -> 期望 "设备节点不存在"
//     openable : RGA_DEVICE 指向能打开的节点     -> 期望 "rga_init() 内部崩溃"
//     default  : 不设 RGA_DEVICE(默认 /dev/rga) -> 期望 "权限不足" 或 "可以打开"
//
// 用法:
//   g++ -O2 -std=c++17 -Wall -Wextra -o /tmp/trgi tests/test_rga_init_guard.cpp
//       src/rga_convert.cc src/safe_probe.cc -Iinclude -Ithird_party/include/rga
//       third_party/lib/librga.so $(pkg-config --cflags --libs opencv4)
//   /tmp/trgi missing
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "rga_convert.h"

namespace {

const char* g_mode = "";
const char* g_capture_path = "/tmp/trgi_stderr.txt";

// 期望在 stderr 里出现的关键字。可以由命令行第二个参数覆盖。
const char* g_expected = "";

const char* expected_keyword() {
    if (g_expected && *g_expected) return g_expected;
    if (strcmp(g_mode, "missing") == 0) return "设备节点不存在";
    if (strcmp(g_mode, "noperm") == 0) return "权限不足";
    if (strcmp(g_mode, "openable") == 0) return "rga_init() 内部崩溃";
    return "RGA 不可用";   // default: 权限不足 / 或节点可用
}

std::string read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <missing|openable|default>\n", argv[0]);
        return 2;
    }
    g_mode = argv[1];
    if (argc >= 3) g_expected = argv[2];

    // ---- 按场景设置 RGA_DEVICE (必须在任何 rga 调用之前) ----
    if (strcmp(g_mode, "missing") == 0) {
        setenv("RGA_DEVICE", "/tmp/definitely_no_such_rga_node", 1);
    } else if (strcmp(g_mode, "noperm") == 0) {
        // 造一个 mode 000 的文件: 节点在、但当前用户打不开 -> open() 返回 EACCES。
        // 这就是板子上 /dev/rga 是 crw------- root root 时的同一类错误。
        const char* p = "/tmp/rga_guard_noperm";
        FILE* f = fopen(p, "w");
        if (f) { fputs("x", f); fclose(f); }
        chmod(p, 0);
        setenv("RGA_DEVICE", p, 1);
    } else if (strcmp(g_mode, "openable") == 0) {
        // /dev/null 一定能打开 -> 预检通过, 于是会真的去调 librga 初始化。
        // 本机 librga 1.9.1 固定 open("/dev/rga"), 打不开 -> 它会自己崩,
        // 这正是要验证的 "init 内部崩溃" 分支。
        setenv("RGA_DEVICE", "/dev/null", 1);
    } else {
        unsetenv("RGA_DEVICE");
    }

    // ---- 把 stderr 抓下来, 断言日志内容 ----
    if (!freopen(g_capture_path, "w", stderr)) {
        fprintf(stdout, "打不开临时文件 %s\n", g_capture_path);
        return 2;
    }

    // ---- 造一块 NV12 源 + RGB 目标 ----
    const int sw = 320, sh = 240;
    const int s_wstride = sw + 16, s_hstride = sh + 16;
    const int dw = 640, dh = 640;
    const int d_wstride = dw * 3 + 64;

    std::vector<uint8_t> src(static_cast<size_t>(s_wstride) * s_hstride * 3 / 2, 0);
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            src[static_cast<size_t>(y) * s_wstride + x] = static_cast<uint8_t>((x + y * 3) & 0xFF);
        }
    }
    uint8_t* uv = src.data() + static_cast<size_t>(s_wstride) * s_hstride;
    for (int y = 0; y < sh / 2; ++y) {
        for (int x = 0; x < sw / 2; ++x) {
            uv[static_cast<size_t>(y) * s_wstride + x * 2 + 0] = static_cast<uint8_t>((x * 2) & 0xFF);
            uv[static_cast<size_t>(y) * s_wstride + x * 2 + 1] = static_cast<uint8_t>((y * 5) & 0xFF);
        }
    }
    std::vector<uint8_t> dst(static_cast<size_t>(d_wstride) * dh, 0xAB);

    // openable 场景要走到 ensure_rga_ready(), 就得过 "源/目标必须是 dma-buf"
    // 这一关 —— 用 /dev/zero 的 fd 冒充一下(判断只看 fd >= 0)。
    int fake_fd = -1;
    if (strcmp(g_mode, "openable") == 0) {
        fake_fd = open("/dev/zero", O_RDONLY);
    }

    rk::RgaImage s;
    s.fd = fake_fd;
    s.vir_addr = src.data();
    s.width = sw;
    s.height = sh;
    s.wstride = s_wstride;
    s.hstride = s_hstride;
    s.format = RK_FORMAT_YCbCr_420_SP;

    rk::RgaImage d;
    d.fd = fake_fd;
    d.vir_addr = dst.data();
    d.width = dw;
    d.height = dh;
    d.wstride = d_wstride;
    d.hstride = dh;
    d.format = RK_FORMAT_RGB_888;

    const int rc = rk::rga_nv12_to_rgb888(s, d, /*letterbox=*/false, nullptr, nullptr, nullptr);

    fflush(stderr);
    if (fake_fd >= 0) close(fake_fd);

    // ---- 取回 stderr, 做断言 ----
    std::string log = read_file(g_capture_path);
    fputs(log.c_str(), stdout);

    int fail = 0;
    if (rc != 0) {
        printf("[X] 转换返回 %d (期望 0)\n", rc);
        fail = 1;
    } else {
        printf("[OK] 转换返回 0 (干净降级, 没崩)\n");
    }

    if (log.find("librga 初始化时崩溃") != std::string::npos) {
        printf("[X] 日志里还有旧文案 \"librga 初始化时崩溃\"\n");
        fail = 1;
    } else {
        printf("[OK] 旧文案 \"librga 初始化时崩溃\" 已消失\n");
    }

    if (log.find("RGA 不可用，原因：") == std::string::npos) {
        printf("[X] 日志里没有 \"RGA 不可用，原因：\"\n");
        fail = 1;
    } else {
        printf("[OK] 日志里有 \"RGA 不可用，原因：\"\n");
    }

    const std::string kw = expected_keyword();
    if (log.find(kw) == std::string::npos) {
        printf("[X] 日志里没有期望的原因关键字 \"%s\"\n", kw.c_str());
        fail = 1;
    } else {
        printf("[OK] 日志里有期望的原因关键字 \"%s\"\n", kw.c_str());
    }

    const char* reason = rk::rga_unavailable_reason();
    if (reason && *reason) {
        printf("[OK] rga_unavailable_reason() = \"%s\"\n", reason);
    } else {
        printf("[OK] rga_unavailable_reason() 为空 (RGA 可用)\n");
    }

    printf("\n%s\n", fail == 0 ? "全部通过" : "有失败项");
    return fail == 0 ? 0 : 1;
}
