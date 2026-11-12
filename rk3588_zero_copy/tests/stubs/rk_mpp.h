#pragma once
// MPP 最小 stub: 只暴露 main.cc 用到的符号
typedef void* MPP_BUFFER;
typedef int MPP_RET;
#define MPP_SUCCESS 0
extern "C" {
MPP_RET mpp_buffer_get_ptr(MPP_BUFFER buffer, void** ptr);
}
