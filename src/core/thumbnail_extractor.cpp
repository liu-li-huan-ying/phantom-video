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

    // 打开文件（不读音频流，减少开销）
    if (avformat_open_input(&ctx_, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(ctx_, nullptr) < 0) { close(); return false; }

    // 找视频流
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

    // Seek 到目标时间（AVSEEK_FLAG_BACKWARD 回退到最近关键帧）
    AVStream* vs = ctx_->streams[videoStreamIdx_];
    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    av_seek_frame(ctx_, -1, ts, AVSEEK_FLAG_BACKWARD);

    // 清空解码器缓冲
    avcodec_flush_buffers(codecCtx_);

    // 读包 + 解码，直到拿到一帧
    AVPacket* pkt = av_packet_alloc();
    bool gotFrame = false;
    int tries = 0;

    while (tries < 30) {
        int ret = av_read_frame(ctx_, pkt);
        if (ret < 0) break;
        if (pkt->stream_index != videoStreamIdx_) { av_packet_unref(pkt); continue; }

        ret = avcodec_send_packet(codecCtx_, pkt);
        av_packet_unref(pkt);
        if (ret < 0) { tries++; continue; }

        ret = avcodec_receive_frame(codecCtx_, frame_);
        if (ret == 0) { gotFrame = true; break; }
        tries++;
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
