#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

#include <SDL.h>

extern "C" {
#include <libswresample/swresample.h>
#include <sonic.h>
}

#include "core/blocking_queue.h"
#include "core/types.h"

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    bool open(const AVCodecParameters* par, double ptsScale);
    bool push(const FramePtr& frame);
    bool tryPush(const FramePtr& frame);
    void closeQueue();
    void pauseDevice();
    void resumeDevice();
    void setVolume(float v) { volume_.store(v, std::memory_order_relaxed); }
    float volume() const { return volume_.load(std::memory_order_relaxed); }
    void setNormalization(bool on) { normalization_.store(on, std::memory_order_relaxed); }
    bool normalization() const { return normalization_.load(std::memory_order_relaxed); }
    float normalizationGain() const { return normGain_.load(std::memory_order_relaxed); }

    // 线程安全操作（可从任何线程调用，无需暂停设备）：
    void clearQueue();                          // 清空 BlockingQueue（current_/offset_ 不碰）
    void setClock(double t);                    // 重置时钟（clockMutex_ 保护）
    double clock() const;                       // 读时钟（clockMutex_ 保护）
    void rebuildSonic(float spd);              // 重建 Sonic + 更新速度（sonicMutex_ 保护）
    void setSpeed(float spd);                   // 仅更新速度原子量

    // 延迟操作：由 fill() 在 SDL 回调线程内原子处理，消除竞态
    void requestSpeedChange(float spd, double anchor = -1.0); // 设置待处理速度变更+锚点
    float pendingSpeed() const;                 // 读取待处理速度（-1 = 无）
    bool hasPendingSpeed() const;               // 是否有待处理速度变更
    void requestSeek(double t);                 // 设置待处理 seek（fill() 内原子清队列+设时钟）
    bool hasPendingSeek() const;                // 是否有待处理 seek

    // seek 阻断：doSeek() 设 true，audioLoop 处理完 seek 后设 false
    // 期间 push()/tryPush() 返回 false，阻止旧帧入队
    void setSeeking(bool s);

private:
    static void SDLCALL sdlCallback(void* userdata, Uint8* stream, int len);
    void fill(Uint8* stream, int len);
    void applyVolume(Uint8* stream, int len);
    bool convert(const FramePtr& frame, AudioChunk& chunk);

    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec spec_{};
    SwrContext* swr_ = nullptr;
    double ptsScale_ = 1.0;
    bool ok_ = false;
    std::mutex swrMutex_;
    AVChannelLayout inLayout_{};

    sonicStream sonic_ = nullptr;
    std::mutex sonicMutex_;

    BlockingQueue<AudioChunk, AudioChunkSize> queue_{ 1764000 };
    // current_ 和 offset_ 仅由 fill()（SDL 回调线程）访问，无锁
    AudioChunk current_;
    size_t offset_ = 0;

    mutable std::mutex clockMutex_;
    double writeHead_ = -1.0;
    double lastPts_ = 0.0;

    std::atomic<float> volume_{ 0.8f };
    std::atomic<float> speed_{ 1.0f };
    float lastSpeed_ = 1.0f;

    // 延迟速度变更：fill() 在 SDL 回调线程内原子处理
    std::atomic<float> pendingSpeed_{ -1.0f };  // -1 = 无待处理变更
    std::atomic<double> pendingSpeedAnchor_{ -1.0 }; // 速度变更时时钟锚点（-1 = 无）

    // 延迟 seek：fill() 在 SDL 回调线程内原子处理
    std::atomic<double> pendingSeek_{ -1.0 };   // -1 = 无待处理 seek
    bool reanchor_ = false;                     // seek 后首块到达时重锚 writeHead_
    bool reanchorSpeed_ = false;                // true = 变速重锚（无阈值）; false = seek 重锚（2s 阈值）
    std::atomic<bool> normalization_{ false };
    std::atomic<float> normGain_{ 1.0f };
    float peakTracker_ = 0.0f;
    Uint32 peakDecayTime_ = 0;
    std::atomic<bool> seeking_{ false };  // seek 期间阻断 push
};
