#pragma once
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
typedef struct AVRational { int num, den; } AVRational;
typedef struct AVStream {
    AVCodecParameters* codecpar;
    AVRational avg_frame_rate;
} AVStream;
typedef struct AVFormatContext { AVStream** streams; unsigned int nb_streams; } AVFormatContext;
extern "C" {
int avformat_open_input(AVFormatContext** ps, const char* url, void* fmt, AVDictionary** options);
int avformat_find_stream_info(AVFormatContext* ic, void** opts);
int av_find_best_stream(AVFormatContext* ic, int type, int wanted, int related, const AVCodec** decoder_ret, int flags);
void avformat_close_input(AVFormatContext** s);
int avformat_network_init(void);
int avformat_network_deinit(void);
double av_q2d(AVRational a);
int av_read_frame(AVFormatContext* s, AVPacket* pkt);
#define AV_LOG_ERROR 16
void av_log_set_level(int level);
}
