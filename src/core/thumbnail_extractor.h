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
    // 返回像素数据（RGB24），调用者负责 av_free(*outPixels)
    bool getFrame(double seconds, uint8_t** outPixels, int& outW, int& outH);

    bool isOpen() const { return ctx_ != nullptr; }

private:
    AVFormatContext* ctx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    int videoStreamIdx_ = -1;
    int srcW_ = 0, srcH_ = 0;
};
