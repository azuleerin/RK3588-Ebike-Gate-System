// rk/log.h —— 分级运行日志（落盘优先，为 24h 长跑取证设计）
//
// 设计约束（来自 AGENT_RULES_FOR_CRASH_DEBUGGING.md 第四节）:
//   1) 分级: INFO(正常流转) / WARN(降级重试) / ERROR(核心错误)
//   2) 每条日志必须带 [时间戳][线程ID][函数名][动作]
//   3) 必须同步落盘到本地文件, SSH 断连/整机挂死后重启仍能查到断连前最后几秒
//   4) 写满磁盘会拖死整机 -> 必须限容轮转
//
// 并发模型:
//   - 进程内单例, 内部一把 mutex 串行化写入
//   - 每条日志一次 write(2) 系统调用(单次 write <= PIPE_BUF 不会与其他写交错),
//     保证多线程日志行不串行错乱
//   - WARN/ERROR 额外 fdatasync, 保证进程被 SIGKILL 后这几行已经落盘
//
// 生命周期:
//   - log_init() 在 main() 最早处调用一次
//   - 日志对象是静态单例, 进程退出时由 __attribute__((destructor)) 关闭 fd
//   - 任何线程任何时刻都可调用写日志接口, 无需担心初始化顺序
#ifndef RK_LOG_H_
#define RK_LOG_H_

#include <cstdarg>
#include <cstdint>
#include <string>

namespace rk {

enum class LogLevel : int {
    kInfo = 0,
    kWarn = 1,
    kError = 2,
};

// 初始化日志文件。
//   path        : 日志文件路径, 父目录需已存在
//   also_stdout : 是否同时输出到 stdout(长跑时建议关掉, 避免管道阻塞)
//   max_bytes   : 单文件上限, 超过后轮转; 0 表示用默认 64MB
//   keep_files  : 轮转保留份数; 0 表示用默认 5
// 返回 false 表示文件打不开(此时日志降级为 stderr, 不影响主流程)
bool log_init(const std::string& path, bool also_stdout = true,
              uint64_t max_bytes = 0, int keep_files = 0);

void log_shutdown();

// 写一条日志。module/func 用于快速定位, fmt 支持 printf 风格。
void log_write(LogLevel level, const char* module, const char* func,
               const char* fmt, ...) __attribute__((format(printf, 4, 5)));

// 各级别累计条数(用于统计与自检)
uint64_t log_count(LogLevel level);
uint64_t log_total();

// 当前日志文件路径(空表示未初始化)
const std::string& log_path();

// 已写入的字节数(轮转累计)
uint64_t log_bytes_written();

}  // namespace rk

// 使用方式:
//   RK_LOG_INFO("mpp", "decode", "frame#%u %dx%d fd=%d", n, w, h, fd);
//
// 约定: module 用模块名(3~6 字符), func 用函数名, 动作写在 fmt 里。
// 这样一条日志就能回答"哪个模块、哪个函数、干了什么"。
#define RK_LOG_INFO(module, func, ...) \
    ::rk::log_write(::rk::LogLevel::kInfo, (module), (func), __VA_ARGS__)
#define RK_LOG_WARN(module, func, ...) \
    ::rk::log_write(::rk::LogLevel::kWarn, (module), (func), __VA_ARGS__)
#define RK_LOG_ERROR(module, func, ...) \
    ::rk::log_write(::rk::LogLevel::kError, (module), (func), __VA_ARGS__)

#endif  // RK_LOG_H_
