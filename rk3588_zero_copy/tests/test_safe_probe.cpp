// test_safe_probe.cpp —— 验证 SafeProbe 真的能在段错误时不弄死进程
//
// 这是 MPP 初始化崩溃保护的核心机制: 板子上 MPP 版本不匹配时 mpp_create()
// 会直接 SIGSEGV, 包在 SafeProbe 里才能退回软解而不是整个进程挂掉。
//
// 用法: g++ -O2 -std=c++17 -Wall -o /tmp/tsp tests/test_safe_probe.cpp src/safe_probe.cc -Iinclude
#include <cstdio>
#include <cstdlib>

#include "safe_probe.h"

namespace {

volatile int* g_null_ptr = nullptr;

// 故意段错误
bool crash_fn(void*) {
    volatile int* p = g_null_ptr;
    *p = 42;              // SIGSEGV
    return true;
}

// 正常返回 true
bool ok_fn(void*) { return true; }

// 正常返回 false
bool nok_fn(void*) { return false; }

}  // namespace

int main() {
    int fail = 0;

    const bool r_ok = rk::SafeProbe::call(&ok_fn, nullptr);
    printf("正常函数返回 true  -> SafeProbe 返回 %s  %s\n",
           r_ok ? "true" : "false", r_ok ? "[OK]" : "[X]");
    if (!r_ok) fail = 1;

    const bool r_nok = rk::SafeProbe::call(&nok_fn, nullptr);
    printf("正常函数返回 false -> SafeProbe 返回 %s  %s\n",
           r_nok ? "true" : "false", (!r_nok) ? "[OK]" : "[X]");
    if (r_nok) fail = 1;

    const bool r_crash = rk::SafeProbe::call(&crash_fn, nullptr);
    printf("崩溃函数(段错误)  -> SafeProbe 返回 %s  %s\n",
           r_crash ? "true" : "false", (!r_crash) ? "[OK]" : "[X]");
    if (r_crash) fail = 1;

    // 能走到这里说明进程没被段错误带走
    printf("进程存活, 继续执行  [OK]\n");

    if (fail == 0) {
        printf("\n全部通过\n");
        return 0;
    }
    printf("\n有失败项\n");
    return 1;
}
