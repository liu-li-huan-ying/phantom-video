#pragma once
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

struct FrameDeleter {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
using FramePtr = std::shared_ptr<AVFrame>;

struct PacketDeleter {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
using PacketPtr = std::shared_ptr<AVPacket>;

struct AudioChunk {
    double pts = 0.0;
    std::vector<uint8_t> data;
};

struct AudioChunkSize {
    size_t operator()(const AudioChunk& c) const { return c.data.size(); }
};