#include "core/decoder.h"

#include <libavutil/hwcontext.h>

Decoder::~Decoder() {
    if (ctx_) avcodec_free_context(&ctx_);
}

bool Decoder::supportsHwType(const AVCodec* codec, enum AVHWDeviceType type) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) break;
        if (cfg->device_type == type &&
            (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0)
            return true;
    }
    return false;
}

bool Decoder::open(const AVCodecParameters* par) {
    return open(par, nullptr);
}

bool Decoder::open(const AVCodecParameters* par, AVBufferRef* hwDeviceCtx) {
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    if (avcodec_parameters_to_context(ctx_, par) < 0) return false;

    hw_ = false;
    if (hwDeviceCtx) {
        const AVHWDeviceContext* hwctx = (const AVHWDeviceContext*)hwDeviceCtx->data;
        if (supportsHwType(codec, hwctx->type)) {
            ctx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
            hw_ = true;
        }
    }

    if (avcodec_open2(ctx_, codec, nullptr) < 0) return false;
    return true;
}

bool Decoder::send(const AVPacket* pkt) {
    return avcodec_send_packet(ctx_, pkt) >= 0;
}

FramePtr Decoder::receive() {
    AVFrame* src = av_frame_alloc();
    int ret = avcodec_receive_frame(ctx_, src);
    if (ret < 0) {
        av_frame_free(&src);
        return nullptr;
    }

    if (src->hw_frames_ctx) {
        AVFrame* sw = av_frame_alloc();
        if (av_hwframe_transfer_data(sw, src, 0) < 0) {
            av_frame_free(&sw);
            av_frame_free(&src);
            return nullptr;
        }
        sw->pts = src->pts;
        sw->best_effort_timestamp = src->best_effort_timestamp;
        av_frame_free(&src);
        src = sw;
    }

    AVFrame* out = av_frame_alloc();
    av_frame_move_ref(out, src);
    av_frame_free(&src);
    return makeFramePtr(out);
}

void Decoder::flush() {
    if (!ctx_) return;
    avcodec_send_packet(ctx_, nullptr);
}

void Decoder::flushBuffers() {
    if (ctx_) avcodec_flush_buffers(ctx_);
}