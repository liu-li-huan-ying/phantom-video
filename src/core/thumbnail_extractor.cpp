#include "core/thumbnail_extractor.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

ThumbnailExtractor::~ThumbnailExtractor() { close(); }

bool ThumbnailExtractor::open(const std::string& path) {
    close();

    // 用最小 probe size 加速打开（只读够识别容器格式的数据）
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "analyzeduration", "500000", 0);  // 500ms max
    av_dict_set(&opts, "probesize", "32768", 0);         // 32KB probe

    if (avformat_open_input(&ctx_, path.c_str(), nullptr, &opts) < 0) {
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);

    // 不调 avformat_find_stream_info（太慢），直接读头部找视频流
    if (avformat_find_stream_info(ctx_, nullptr) < 0) { close(); return false; }

    videoStreamIdx_ = -1;
    for (unsigned i = 0; i < ctx_->nb_streams; ++i) {
        if (ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIdx_ = (int)i;
            break;
        }
    }
    if (videoStreamIdx_ < 0) { close(); return false; }

    AVCodecParameters* par = ctx_->streams[videoStreamIdx_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { close(); return false; }

    codecCtx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx_, par);
    codecCtx_->thread_count = 1;

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        close();
        return false;
    }

    srcW_ = codecCtx_->width;
    srcH_ = codecCtx_->height;
    frame_ = av_frame_alloc();
    return true;
}

void ThumbnailExtractor::close() {
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (codecCtx_) { avcodec_free_context(&codecCtx_); codecCtx_ = nullptr; }
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    if (ctx_) { avformat_close_input(&ctx_); ctx_ = nullptr; }
    videoStreamIdx_ = -1;
}

bool ThumbnailExtractor::getFrame(double seconds, uint8_t** outPixels, int& outW, int& outH) {
    if (!ctx_ || !codecCtx_ || !frame_ || videoStreamIdx_ < 0)
        return false;

    *outPixels = nullptr;
    outW = outH = 0;

    AVStream* vs = ctx_->streams[videoStreamIdx_];

    // 边界保护
    double durationSec = 0;
    if (vs->duration != AV_NOPTS_VALUE && vs->duration > 0)
        durationSec = vs->duration * av_q2d(vs->time_base);
    else if (ctx_->duration != AV_NOPTS_VALUE)
        durationSec = (double)ctx_->duration / AV_TIME_BASE;
    if (seconds < 0) seconds = 0;
    if (durationSec > 0 && seconds > durationSec) seconds = durationSec * 0.95;

    // 多策略 seek
    int seekOk = -1;
    int64_t targetTs = (int64_t)(seconds / av_q2d(vs->time_base));

    seekOk = avformat_seek_file(ctx_, videoStreamIdx_,
                                targetTs - (int64_t)(0.5 / av_q2d(vs->time_base)),
                                targetTs,
                                targetTs + (int64_t)(0.5 / av_q2d(vs->time_base)), 0);
    if (seekOk < 0)
        seekOk = av_seek_frame(ctx_, videoStreamIdx_, targetTs, AVSEEK_FLAG_BACKWARD);
    if (seekOk < 0)
        seekOk = av_seek_frame(ctx_, -1, (int64_t)(seconds * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);

    avcodec_flush_buffers(codecCtx_);

    // 读包 + 解码（只取第一个视频帧）
    AVPacket* pkt = av_packet_alloc();
    bool gotFrame = false;
    int packetsRead = 0;
    const int maxPackets = 64;  // 减少最大读包数

    while (packetsRead < maxPackets) {
        int ret = av_read_frame(ctx_, pkt);
        if (ret < 0) break;
        packetsRead++;

        if (pkt->stream_index != videoStreamIdx_) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(codecCtx_, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        ret = avcodec_receive_frame(codecCtx_, frame_);
        if (ret == 0) {
            gotFrame = true;
            break;
        }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        break;
    }

    if (!gotFrame) {
        avcodec_send_packet(codecCtx_, nullptr);
        if (avcodec_receive_frame(codecCtx_, frame_) == 0)
            gotFrame = true;
    }

    av_packet_free(&pkt);
    if (!gotFrame) return false;

    // 缩略图尺寸：160px 宽（比 320 快一倍）
    int dstW = 160;
    int dstH = dstW * srcH_ / srcW_;
    if (dstH <= 0) dstH = 1;

    int bufSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, dstW, dstH, 1);
    uint8_t* buf = (uint8_t*)av_malloc(bufSize);
    if (!buf) return false;

    swsCtx_ = sws_getCachedContext(swsCtx_,
                                   srcW_, srcH_, (AVPixelFormat)frame_->format,
                                   dstW, dstH, AV_PIX_FMT_RGB24,
                                   SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) { av_free(buf); return false; }

    uint8_t* dstSlice[1] = { buf };
    int dstStride[1] = { dstW * 3 };
    sws_scale(swsCtx_, frame_->data, frame_->linesize, 0, srcH_, dstSlice, dstStride);

    *outPixels = buf;
    outW = dstW;
    outH = dstH;
    return true;
}
