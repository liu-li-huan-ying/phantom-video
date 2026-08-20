#include "core/demuxer.h"

#include <libavutil/mathematics.h>

Demuxer::~Demuxer() {
    if (ctx_) avformat_close_input(&ctx_);
}

bool Demuxer::open(const std::string& path) {
    if (avformat_open_input(&ctx_, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(ctx_, nullptr) < 0) return false;

    videoIndex_ = av_find_best_stream(ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioIndex_ = av_find_best_stream(ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    subtitleIndex_ = av_find_best_stream(ctx_, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);

    if (ctx_->duration != AV_NOPTS_VALUE) duration_ = (double)ctx_->duration / AV_TIME_BASE;
    return true;
}

PacketPtr Demuxer::readPacket() {
    PacketPtr pkt = makePacketPtr(av_packet_alloc());
    if (av_read_frame(ctx_, pkt.get()) < 0) return nullptr;
    return pkt;
}

bool Demuxer::seek(double seconds) {
    int idx = videoIndex_ >= 0 ? videoIndex_ : audioIndex_;
    if (idx < 0) return false;
    AVStream* st = ctx_->streams[idx];
    int64_t ts = av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
    return av_seek_frame(ctx_, idx, ts, AVSEEK_FLAG_BACKWARD) >= 0;
}

AVStream* Demuxer::videoStream() const {
    return videoIndex_ >= 0 ? ctx_->streams[videoIndex_] : nullptr;
}

AVStream* Demuxer::audioStream() const {
    return audioIndex_ >= 0 ? ctx_->streams[audioIndex_] : nullptr;
}

AVStream* Demuxer::subtitleStream() const {
    return subtitleIndex_ >= 0 ? ctx_->streams[subtitleIndex_] : nullptr;
}

const AVCodecParameters* Demuxer::videoCodecpar() const {
    AVStream* st = videoStream();
    return st ? st->codecpar : nullptr;
}

const AVCodecParameters* Demuxer::audioCodecpar() const {
    AVStream* st = audioStream();
    return st ? st->codecpar : nullptr;
}

const AVCodecParameters* Demuxer::subtitleCodecpar() const {
    if (subtitleIndex_ < 0) return nullptr;
    return ctx_->streams[subtitleIndex_]->codecpar;
}