#pragma once
#include <string>
#include <vector>

#include "core/types.h"

struct SubtitleStreamInfo {
    int index = -1;
    std::string language;
    std::string title;
};

class Demuxer {
public:
    ~Demuxer();
    bool open(const std::string& path);
    PacketPtr readPacket();
    bool seek(double seconds);
    bool seekAudio(double seconds);  // 音频流专用 seek（用 audioIndex_ + audio time_base）
    double audioTbSeconds() const;   // 音频流 time_base 秒值（自校准 seek 用，M31k）

    int videoIndex() const { return videoIndex_; }
    int audioIndex() const { return audioIndex_; }
    int subtitleIndex() const { return subtitleIndex_; }
    double duration() const { return duration_; }
    AVStream* videoStream() const;
    AVStream* audioStream() const;
    AVStream* subtitleStream() const;
    const AVCodecParameters* videoCodecpar() const;
    const AVCodecParameters* audioCodecpar() const;
    const AVCodecParameters* subtitleCodecpar() const;

    // M31b: 多字幕轨枚举
    const std::vector<SubtitleStreamInfo>& subtitleStreams() const { return subtitleStreams_; }
    int subtitleStreamCount() const { return (int)subtitleStreams_.size(); }
    const AVCodecParameters* subtitleCodecparByIndex(int idx) const;

private:
    AVFormatContext* ctx_ = nullptr;
    int videoIndex_ = -1;
    int audioIndex_ = -1;
    int subtitleIndex_ = -1;
    double duration_ = 0.0;
    std::vector<SubtitleStreamInfo> subtitleStreams_;  // M31b
};