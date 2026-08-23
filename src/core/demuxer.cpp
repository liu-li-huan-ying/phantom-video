#include "core/demuxer.h"
#include "core/logger.h"

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

    // M31b: 枚举所有字幕流
    subtitleStreams_.clear();
    for (unsigned i = 0; i < ctx_->nb_streams; ++i) {
        if (ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            SubtitleStreamInfo info;
            info.index = (int)i;
            AVDictionaryEntry* lang = av_dict_get(ctx_->streams[i]->metadata, "language", nullptr, 0);
            if (lang && lang->value) info.language = lang->value;
            AVDictionaryEntry* title = av_dict_get(ctx_->streams[i]->metadata, "title", nullptr, 0);
            if (title && title->value) info.title = title->value;
            subtitleStreams_.push_back(std::move(info));
        }
    }

    if (ctx_->duration != AV_NOPTS_VALUE) {
        duration_ = (double)ctx_->duration / AV_TIME_BASE;
    } else {
        // SWF 等流格式 ctx_->duration 无效，尝试从流 duration 推算
        int vidIdx = videoIndex_ >= 0 ? videoIndex_ : audioIndex_;
        if (vidIdx >= 0) {
            AVStream* st = ctx_->streams[vidIdx];
            if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
                duration_ = st->duration * av_q2d(st->time_base);
            } else {
                // 最后手段：扫描所有包找最大 PTS
                double maxPts = 0;
                AVPacket* pkt = av_packet_alloc();
                while (av_read_frame(ctx_, pkt) >= 0) {
                    if (pkt->stream_index == vidIdx && pkt->pts != AV_NOPTS_VALUE) {
                        double t = pkt->pts * av_q2d(st->time_base);
                        if (t > maxPts) maxPts = t;
                    }
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
                if (maxPts > 0) duration_ = maxPts;
                // 重新 seek 回开头
                av_seek_frame(ctx_, vidIdx, 0, AVSEEK_FLAG_BACKWARD);
            }
        }
    }
    return true;
}

PacketPtr Demuxer::readPacket() {
    PacketPtr pkt = makePacketPtr(av_packet_alloc());
    int ret = av_read_frame(ctx_, pkt.get());
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        LOG_WARN("DEMUX", "readPacket fail: %s (%d)", err, ret);
        return nullptr;
    }
    return pkt;
}

bool Demuxer::seek(double seconds) {
    int idx = videoIndex_ >= 0 ? videoIndex_ : audioIndex_;
    if (idx < 0) return false;
    AVStream* st = ctx_->streams[idx];
    int64_t ts = av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
    LOG_DBG("DEMUX", "seek: seconds=%.3f idx=%d time_base=%d/%d ts=%ld",
            seconds, idx, st->time_base.num, st->time_base.den, (long)ts);
    return av_seek_frame(ctx_, idx, ts, AVSEEK_FLAG_BACKWARD) >= 0;
}

bool Demuxer::seekAudio(double seconds) {
    if (audioIndex_ < 0) return false;
    AVStream* st = ctx_->streams[audioIndex_];
    // M31k: 回归规范公式（ts 用音频流自身 time_base）。实测不同 mp4 的 seek 内部路径
    // 行为不一致（合成文件 videoTb=1/15360 时 M31h 公式落点精准，用户文件 1/90000 时
    // 却放大 2.04 倍触发末尾截断），静态公式无法通吃。落点误差由 ALOOP 的
    // 自校准机制用 packet dts 实测并加法修正（见 player.cpp）。
    int64_t ts = av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
    LOG_DBG("DEMUX", "seekAudio: seconds=%.3f idx=%d audio_tb=%d/%d ts=%ld",
            seconds, audioIndex_, st->time_base.num, st->time_base.den, (long)ts);
    return av_seek_frame(ctx_, audioIndex_, ts, AVSEEK_FLAG_BACKWARD) >= 0;
}

double Demuxer::audioTbSeconds() const {
    if (audioIndex_ < 0) return 0.0;
    AVStream* st = ctx_->streams[audioIndex_];
    if (st->time_base.num <= 0) return 0.0;
    return av_q2d(st->time_base);
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

const AVCodecParameters* Demuxer::subtitleCodecparByIndex(int idx) const {
    if (idx < 0 || idx >= (int)ctx_->nb_streams) return nullptr;
    if (ctx_->streams[idx]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) return nullptr;
    return ctx_->streams[idx]->codecpar;
}