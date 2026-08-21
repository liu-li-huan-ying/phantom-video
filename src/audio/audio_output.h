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
    void setSpeed(float spd);
    void setClock(double t);
    double clock() const;

    // 所有状态变更必须在 pause/resume 之间进行
    // pauseDevice 停止 SDL 回调，保证 fill() 不会在修改期间运行
    void clearAndReset(double newClock);
    void setSpeedAndReset(float spd);

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
    AudioChunk current_;
    size_t offset_ = 0;

    mutable std::mutex clockMutex_;
    double writeHead_ = -1.0;
    double lastPts_ = 0.0;

    std::atomic<float> volume_{ 0.8f };
    std::atomic<float> speed_{ 1.0f };
    float lastSpeed_ = 1.0f;
    std::atomic<bool> normalization_{ false };
    std::atomic<float> normGain_{ 1.0f };
    float peakTracker_ = 0.0f;
    Uint32 peakDecayTime_ = 0;
    std::atomic<bool> devicePaused_{ false };
};
