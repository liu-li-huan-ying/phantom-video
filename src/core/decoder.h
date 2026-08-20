#pragma once
#include "core/types.h"

class Decoder {
public:
    ~Decoder();
    bool open(const AVCodecParameters* par);
    bool open(const AVCodecParameters* par, AVBufferRef* hwDeviceCtx);
    bool send(const AVPacket* pkt);
    FramePtr receive();
    void flush();
    void flushBuffers();
    const AVCodecContext* ctx() const { return ctx_; }
    bool usingHardware() const { return hw_; }

private:
    static bool supportsHwType(const AVCodec* codec, enum AVHWDeviceType type);
    AVCodecContext* ctx_ = nullptr;
    bool hw_ = false;
};