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
    ~AudioOutput();
    bool open(const AVCodecParameters* par, double ptsScale);
    bool push(const FramePtr& frame);
    void closeQueue();
    void clearQueue();
    void pauseDevice();
    void resumeDevice();
    void setVolume(float v) { volume_.store(v, std::memory_order_relaxed); }
    float volume() const { return volume_.load(std::memory_order_relaxed); }
    void resetClock();
    double clock() const;

private:
    static void SDLCALL sdlCallback(void* userdata, Uint8* stream, int len);
    void fill(Uint8* stream, int len);
    void applyVolume(Uint8* stream, int len);

    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec spec_{};
    SwrContext* swr_ = nullptr;
    double ptsScale_ = 1.0;
    double bytesPerSec_ = 1.0;
    bool ok_ = false;

    BlockingQueue<AudioChunk, AudioChunkSize> queue_{ 70560 };
    AudioChunk current_;
    size_t offset_ = 0;

    mutable std::mutex clockMutex_;
    double writeHead_ = -1.0;

    std::atomic<float> volume_{ 0.8f };
    Uint8* mixTemp_ = nullptr;
};