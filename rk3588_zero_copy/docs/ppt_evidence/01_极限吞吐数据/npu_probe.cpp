// npu_probe —— 调用【应用自己的】 read_hw_load() 取硬件负载, 用来抓"应用侧口径"。
// 不是重新实现一遍解析, 而是直接链接 rk_zero_copy 里的 engine_ctl.cc.o ——
// 口径与应用完全一致(同一份 parse_core_pcts + 同样的 max 聚合)。
// CPU% 是差分量, 所以连续调两次取第二次(与应用内 200ms 一跳的用法同源)。
#include "engine_ctl.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
int main() {
    rk::read_hw_load();                 // 第一次: 建立差分基线
    sleep(1);
    const rk::HwLoad ld = rk::read_hw_load();   // 第二次: CPU% 有效
    std::time_t t = std::time(nullptr);
    struct tm tm_buf{}; localtime_r(&t, &tm_buf);
    char ts[32]; strftime(ts, sizeof(ts), "%F %T", &tm_buf);
    printf("%s,npu=%.1f,npu_core=%.1f,c0=%.1f,c1=%.1f,c2=%.1f,cpu=%.1f,vpu=%.1f,rga=%.1f,src=%s\n",
           ts, ld.npu, ld.npu_core, ld.npu_c0, ld.npu_c1, ld.npu_c2,
           ld.cpu, ld.vpu, ld.rga, ld.npu_src.c_str());
    return 0;
}
