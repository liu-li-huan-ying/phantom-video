#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

#include <SDL.h>

extern "C" {
#include <libswresample/swresample.h>
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
    void clearQueue();
    void pauseDevice();
    void resumeDevice();
    void setVolume(float v) { volume_.store(v, std::memory_order_relaxed); }
    float volume() const { return volume_.load(std::memory_order_relaxed); }
    void setSpeed(float spd);
    void resetClock();
    void setClock(double t);
    double clock() const;

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
    int inSampleRate_ = 0;
    AVChannelLayout inLayout_{};
    AVSampleFormat inFmt_ = AV_SAMPLE_FMT_NONE;

    BlockingQueue<AudioChunk, AudioChunkSize> queue_{ 1764000 };
    AudioChunk current_;
    size_t offset_ = 0;

    mutable std::mutex clockMutex_;
    double writeHead_ = -1.0;

    std::atomic<float> volume_{ 0.8f };
    std::atomic<float> speed_{ 1.0f };
    std::atomic<bool> clearPending_{ false };
};