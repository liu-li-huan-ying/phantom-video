#pragma once
#include "core/types.h"

class Decoder {
public:
    ~Decoder();
    bool open(const AVCodecParameters* par);
    bool send(const AVPacket* pkt);
    FramePtr receive();
    void flush();
    void flushBuffers();
    const AVCodecContext* ctx() const { return ctx_; }

private:
    AVCodecContext* ctx_ = nullptr;
};