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
inline FramePtr makeFramePtr(AVFrame* f) { return FramePtr(f, FrameDeleter{}); }

struct PacketDeleter {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
using PacketPtr = std::shared_ptr<AVPacket>;
inline PacketPtr makePacketPtr(AVPacket* p) { return PacketPtr(p, PacketDeleter{}); }

struct AudioChunk {
    double pts = 0.0;
    int outRate = 44100;  // 变速时该 chunk 的输出采样率（内容采样/内容秒）
    std::vector<uint8_t> data;
};

struct AudioChunkSize {
    size_t operator()(const AudioChunk& c) const { return c.data.size(); }
};