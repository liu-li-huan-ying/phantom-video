#pragma once
#include <string>
#include <cstdint>

struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
struct AVFrame;

class ThumbnailExtractor {
public:
    ~ThumbnailExtractor();
    bool open(const std::string& path);
    void close();

    // seek 到指定秒数，解码一帧并转换为 RGB
    // 返回像素数据（RGB24），用完调 freePixels() 释放
    bool getFrame(double seconds, uint8_t** outPixels, int& outW, int& outH);
    static void freePixels(uint8_t* pixels);

    bool isOpen() const { return ctx_ != nullptr; }

private:
    AVFormatContext* ctx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    int videoStreamIdx_ = -1;
    int srcW_ = 0, srcH_ = 0;
};
