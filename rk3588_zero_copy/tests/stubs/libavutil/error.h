#pragma once
// FFmpeg libavutil/error.h 的最小 stub(只覆盖本工程用到的部分)
#define AV_ERROR_MAX_STRING_SIZE 64
#ifdef __cplusplus
extern "C" {
#endif
void av_strerror(int errnum, char* errbuf, size_t errbuf_size);
const char* av_err2str(int errnum);
#ifdef __cplusplus
}
#endif
