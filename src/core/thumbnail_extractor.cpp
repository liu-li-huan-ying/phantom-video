#include "core/thumbnail_extractor.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

ThumbnailExtractor::~ThumbnailExtractor() { close(); }

bool ThumbnailExtractor::open(const std::string& path) {
    close();

    if (avformat_open_input(&ctx_, path.c_str(), nullptr, nullptr) < 0)
        return false;
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
    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) { close(); return false; }

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

    // Seek 到目标时间
    AVStream* vs = ctx_->streams[videoStreamIdx_];
    double durationSec = (double)vs->duration / AV_TIME_BASE;
    // 边界保护：不 seek 到超出范围的位置
    if (seconds < 0) seconds = 0;
    if (durationSec > 0 && seconds > durationSec) seconds = durationSec * 0.95;

    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    av_seek_frame(ctx_, -1, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codecCtx_);

    // 读包 + 解码，限制总读包数防止死循环
    AVPacket* pkt = av_packet_alloc();
    bool gotFrame = false;
    int packetsRead = 0;
    const int maxPackets = 128;  // 最多读 128 个包

    while (packetsRead < maxPackets) {
        int ret = av_read_frame(ctx_, pkt);
        if (ret < 0) break;  // EOF 或错误

        packetsRead++;

        if (pkt->stream_index != videoStreamIdx_) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(codecCtx_, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        // 循环接收帧（处理 B 帧等情况）
        while (true) {
            ret = avcodec_receive_frame(codecCtx_, frame_);
            if (ret == 0) { gotFrame = true; break; }
            if (ret == AVERROR(EAGAIN)) break;  // 需要更多包
            if (ret == AVERROR_EOF) break;       // 流结束
            break;
        }
        if (gotFrame) break;
    }

    // 尝试 flush 解码器获取剩余帧
    if (!gotFrame) {
        avcodec_send_packet(codecCtx_, nullptr);
        if (avcodec_receive_frame(codecCtx_, frame_) == 0)
            gotFrame = true;
    }

    av_packet_free(&pkt);
    if (!gotFrame) return false;

    // 转换为 RGB24
    int dstW = srcW_ > 320 ? 320 : srcW_;
    int dstH = dstW * srcH_ / srcW_;
    if (dstH <= 0) dstH = 1;

    int bufSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, dstW, dstH, 1);
    uint8_t* buf = (uint8_t*)av_malloc(bufSize);
    if (!buf) return false;

    swsCtx_ = sws_getCachedContext(swsCtx_,
                                   srcW_, srcH_, (AVPixelFormat)frame_->format,
                                   dstW, dstH, AV_PIX_FMT_RGB24,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) { av_free(buf); return false; }

    uint8_t* dstSlice[1] = { buf };
    int dstStride[1] = { dstW * 3 };
    sws_scale(swsCtx_, frame_->data, frame_->linesize, 0, srcH_, dstSlice, dstStride);

    *outPixels = buf;
    outW = dstW;
    outH = dstH;
    return true;
}
