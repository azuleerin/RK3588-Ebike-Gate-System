// src/minimal/main_minimal.cc —— 最小化测试单元的唯一入口
//
// 隔离原则（AGENT_RULES_FOR_CRASH_DEBUGGING.md 第一节）:
//   绝对禁止把解码、RGA、NPU、LPRNet、MQTT 串在一起跑。
//   启动时强制校验: 三个模式必须且只能选一个, 违反直接退出(退出码 2)。
//
// 用法:
//   rk_minimal --npu-only --src nv12:<file> --model <model.rknn> [选项]
//   rk_minimal --decoder-only --src <mp4|rtsp>            (step/03 实现)
//   rk_minimal --rga-only --src nv12:<file>               (step/04 实现)
//
// 选项:
//   --frames N          跑 N 帧后退出
//   --duration SEC      跑 SEC 秒后退出
//   --loop              到达末尾回到开头
//   --log FILE          日志文件(默认 logs/minimal_<mode>.log)
//   --stats-interval N  每 N 秒巡检一次
//   --core-mask N       NPU 核掩码(0=AUTO 1=CORE0 2=CORE1 4=CORE2 3=0+1 7=三核)
//   --batch N           NPU batch
//   --save-frame FILE   存一帧 PNG(肉眼验证)
//   --no-legacy-rga     RGA 硬件路径对照开关(rga-only 用)

#include <cstdio>
#include <cstring>
#include <string>

#include "rk/fd_ledger.h"
#include "rk/log.h"
#include "rk/mem_guard.h"
#include "rk/minimal/unit.h"
#include "rk/stack_guard.h"

namespace {

void usage(const char* argv0) {
    fprintf(stderr,
        "用法: %s (--decoder-only | --rga-only | --npu-only) [选项]\n"
        "\n"
        "  --npu-only --src nv12:<file>|rgb:<file> --model <model.rknn>\n"
        "\n"
        "  --frames N          跑 N 帧后退出\n"
        "  --duration SEC      跑 SEC 秒后退出\n"
        "  --loop              到达末尾回到开头\n"
        "  --log FILE          日志文件\n"
        "  --stats-interval N  每 N 秒巡检一次(默认 60)\n"
        "  --core-mask N       NPU 核掩码\n"
        "  --batch N           NPU batch\n"
        "  --save-frame FILE   存一帧 PNG\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    rk::minimal::UnitOptions opt;
    bool want_decoder = false;
    bool want_rga = false;
    bool want_npu = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "参数 %s 缺少取值\n", name);
                exit(2);
            }
            return argv[++i];
        };
        if (!strcmp(a, "--decoder-only")) { want_decoder = true; }
        else if (!strcmp(a, "--rga-only")) { want_rga = true; }
        else if (!strcmp(a, "--npu-only")) { want_npu = true; }
        else if (!strcmp(a, "--src")) { opt.src = next(a); }
        else if (!strcmp(a, "--dump")) { opt.dump = next(a); }
        else if (!strcmp(a, "--model")) { opt.model = next(a); }
        else if (!strcmp(a, "--save-frame")) { opt.save_frame = next(a); }
        else if (!strcmp(a, "--frames")) { opt.frames = atoi(next(a)); }
        else if (!strcmp(a, "--duration")) { opt.duration_sec = atoi(next(a)); }
        else if (!strcmp(a, "--loop")) { opt.loop = true; }
        else if (!strcmp(a, "--log")) { opt.log_path = next(a); }
        else if (!strcmp(a, "--stats-interval")) { opt.stats_interval = atoi(next(a)); }
        else if (!strcmp(a, "--core-mask")) { opt.core_mask = atoi(next(a)); }
        else if (!strcmp(a, "--batch")) { opt.batch = atoi(next(a)); }
        else if (!strcmp(a, "--no-legacy-rga")) { opt.no_legacy_rga = true; }
        else if (!strcmp(a, "--wstride-mode")) { opt.wstride_mode = next(a); }
        else if (!strcmp(a, "--import-mode")) { opt.import_mode = next(a); }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "未知参数: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    // ---- 模式互斥校验（隔离原则的硬约束）----
    const int n_modes = (want_decoder ? 1 : 0) + (want_rga ? 1 : 0) + (want_npu ? 1 : 0);
    if (n_modes != 1) {
        fprintf(stderr,
                "[FATAL] 必须且只能指定一个模式: --decoder-only / --rga-only / --npu-only\n"
                "         当前指定了 %d 个。绝对禁止同时跑多个模块 —— "
                "那样无法定位死机来自哪个环节。\n", n_modes);
        usage(argv[0]);
        return 2;
    }
    opt.mode = want_npu ? "npu-only" : (want_decoder ? "decoder-only" : "rga-only");

    if (opt.src.empty()) {
        fprintf(stderr, "[FATAL] 必须指定 --src\n");
        usage(argv[0]);
        return 2;
    }
    if (opt.model.empty()) {
        // 给一个默认值, 方便直接跑
        opt.model = "/home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn";
    }
    if (opt.log_path.empty()) {
        opt.log_path = "/home/seeed/rk_deploy/logs/minimal_" + opt.mode + ".log";
    }

    // ---- 日志初始化（最先做, 之后所有输出都进文件）----
    const bool ok = rk::log_init(opt.log_path, /*also_stdout=*/true);
    if (!ok) {
        fprintf(stderr, "[WARN] 日志文件打开失败, 日志降级到 stderr\n");
    }

    RK_LOG_INFO("main", "boot",
                "===== rk_minimal 启动 =====\n"
                "  模式      : %s\n"
                "  输入源    : %s\n"
                "  模型      : %s\n"
                "  帧数上限  : %d\n"
                "  时长上限  : %d 秒\n"
                "  循环      : %d\n"
                "  日志      : %s",
                opt.mode.c_str(), opt.src.c_str(), opt.model.c_str(), opt.frames,
                opt.duration_sec, opt.loop ? 1 : 0, opt.log_path.c_str());

    // ---- 观测基建 ----
    rk::StackGuard::register_thread("main");
    rk::StackGuard::start_watchdog(/*min_free_bytes=*/256 * 1024, /*interval_ms=*/500);

    // 先把 stdio 与日志文件记入 fd 基线, 后续对账只报真正新增的 fd
    rk::FdLedger::capture_baseline();

    // 开局先对一次账: 记录"进入时的基线", 死机后能对比
    rk::minimal::audit_now("启动时基线");

    int rc = 2;
    if (want_npu) {
        rc = rk::minimal::run_npu_unit(opt);
    } else if (want_decoder) {
        rc = rk::minimal::run_decoder_unit(opt);
    } else {
        rc = rk::minimal::run_rga_unit(opt);
    }

    rk::minimal::audit_now("退出前");

    RK_LOG_INFO("main", "exit", "退出码=%d  日志计数: INFO=%llu WARN=%llu ERROR=%llu",
                rc, static_cast<unsigned long long>(rk::log_count(rk::LogLevel::kInfo)),
                static_cast<unsigned long long>(rk::log_count(rk::LogLevel::kWarn)),
                static_cast<unsigned long long>(rk::log_count(rk::LogLevel::kError)));
    rk::StackGuard::stop_watchdog();
    rk::log_shutdown();
    return rc;
}
