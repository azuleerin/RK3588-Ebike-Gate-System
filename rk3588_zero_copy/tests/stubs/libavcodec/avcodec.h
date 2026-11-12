#pragma once
#include <cstdint>
#include <cstddef>
#include "libavutil/avutil.h"
#define AV_CODEC_FLAG_LOW_DELAY (1 << 19)
typedef enum AVCodecID { AV_CODEC_ID_NONE = 0, AV_CODEC_ID_H264 = 27, AV_CODEC_ID_HEVC = 173 } AVCodecID;
typedef struct AVCodec { const char* name; enum AVCodecID id; } AVCodec;
typedef struct AVCodecParameters { int width, height; enum AVCodecID codec_id; } AVCodecParameters;
typedef struct AVCodecContext { int width, height; int thread_count; int flags; int extradata_size; uint8_t* extradata; AVCodecParameters* codecpar; } AVCodecContext;
typedef struct AVPacket { uint8_t* data; int size; int stream_index; int64_t pts; } AVPacket;
typedef struct AVBuffer { void* data; } AVBuffer;
typedef struct AVBufferRef { AVBuffer* buffer; } AVBufferRef;
typedef struct AVFrame { uint8_t** data; int* linesize; int width, height; int format; AVBufferRef** buf; int64_t pts; } AVFrame;
enum AVMediaType { AVMEDIA_TYPE_VIDEO = 0 };
extern "C" {
const AVCodec* avcodec_find_decoder_by_name(const char* name);
const AVCodec* avcodec_find_decoder(AVCodecID id);
const char* avcodec_get_name(AVCodecID id);
AVCodecContext* avcodec_alloc_context3(const AVCodec* c);
int avcodec_parameters_to_context(AVCodecContext* ctx, const AVCodecParameters* par);
int avcodec_open2(AVCodecContext* ctx, const AVCodec* c, void** opts);
int avcodec_send_packet(AVCodecContext* ctx, const AVPacket* pkt);
int avcodec_receive_frame(AVCodecContext* ctx, AVFrame* frame);
AVPacket* av_packet_alloc(void);
void av_packet_free(AVPacket** p);
void av_packet_unref(AVPacket* p);
AVFrame* av_frame_alloc(void);
void av_frame_free(AVFrame** f);
void av_frame_unref(AVFrame* f);
void* av_buffer_get_opaque(const AVBufferRef* ref);
int avcodec_receive_frame_stub(void);
}
