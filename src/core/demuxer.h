#pragma once
#include <string>

#include "core/types.h"

class Demuxer {
public:
    ~Demuxer();
    bool open(const std::string& path);
    PacketPtr readPacket();
    bool seek(double seconds);

    int videoIndex() const { return videoIndex_; }
    int audioIndex() const { return audioIndex_; }
    double duration() const { return duration_; }
    AVStream* videoStream() const;
    AVStream* audioStream() const;
    const AVCodecParameters* videoCodecpar() const;
    const AVCodecParameters* audioCodecpar() const;

private:
    AVFormatContext* ctx_ = nullptr;
    int videoIndex_ = -1;
    int audioIndex_ = -1;
    double duration_ = 0.0;
};