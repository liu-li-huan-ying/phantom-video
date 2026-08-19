#include "core/decoder.h"

Decoder::~Decoder() {
    if (ctx_) avcodec_free_context(&ctx_);
}

bool Decoder::open(const AVCodecParameters* par) {
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    if (avcodec_parameters_to_context(ctx_, par) < 0) return false;
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