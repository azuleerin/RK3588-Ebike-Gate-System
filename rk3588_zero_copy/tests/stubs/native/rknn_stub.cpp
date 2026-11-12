// x86 版本的 librknnrt stub —— 只为在非 RK3588 机器上完成链接并跑启动流程。
// 板端构建不使用它（板端用 third_party/lib 里真正的 aarch64 librknnrt.so）。
#include <cstddef>

extern "C" {
int rknn_init(void*, void*, unsigned int, unsigned int, void*) { return -1; }
int rknn_query(void*, int, void*, unsigned int) { return -1; }
int rknn_run(void*, void*) { return -1; }
int rknn_inputs_set(void*, unsigned int, void*) { return -1; }
int rknn_outputs_get(void*, unsigned int, void*, void*) { return -1; }
int rknn_outputs_release(void*, unsigned int, void*) { return 0; }
int rknn_destroy(void*) { return 0; }
int rknn_set_io_mem(void*, void*, void*) { return -1; }
void* rknn_create_mem_from_fd(void*, int, void*, unsigned int, int) { return nullptr; }
int rknn_destroy_mem(void*, void*) { return 0; }
}
