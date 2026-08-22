#include "audio/audio_output.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

AudioOutput::AudioOutput() {
    inLayout_ = {};
}

AudioOutput::~AudioOutput() {
    if (dev_) {
        SDL_PauseAudioDevice(dev_, 1);
        SDL_CloseAudioDevice(dev_);
    }
    if (sonic_) sonicDestroyStream(sonic_);
    {
        std::lock_guard<std::mutex> lock(swrMutex_);
        if (swr_) swr_free(&swr_);
        av_channel_layout_uninit(&inLayout_);
    }
}

bool AudioOutput::open(const AVCodecParameters* par, double ptsScale) {
    ptsScale_ = ptsScale;

    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &AudioOutput::sdlCallback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &spec_, 0);
    if (!dev_) return false;

    {
        std::lock_guard<std::mutex> lock(swrMutex_);
        av_channel_layout_uninit(&inLayout_);
        av_channel_layout_copy(&inLayout_, &par->ch_layout);

        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, spec_.channels);
        swr_ = swr_alloc();
        if (!swr_) return false;
        av_opt_set_chlayout(swr_, "in_chlayout", &par->ch_layout, 0);
        av_opt_set_int(swr_, "in_sample_rate", par->sample_rate, 0);
        av_opt_set_sample_fmt(swr_, "in_sample_fmt", (AVSampleFormat)par->format, 0);
        av_opt_set_chlayout(swr_, "out_chlayout", &outLayout, 0);
        av_opt_set_int(swr_, "out_sample_rate", spec_.freq, 0);
        av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        int ret = swr_init(swr_);
        av_channel_layout_uninit(&outLayout);
        if (ret < 0) return false;
    }

    sonic_ = sonicCreateStream(spec_.freq, spec_.channels);
    if (!sonic_) return false;
    sonicSetSampleRate(sonic_, spec_.freq);
    sonicSetNumChannels(sonic_, spec_.channels);
    sonicSetSpeed(sonic_, 1.0f);
    sonicSetPitch(sonic_, 1.0f);
    sonicSetRate(sonic_, 1.0f);
    sonicSetQuality(sonic_, 1);

    ok_ = true;
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void AudioOutput::clearQueue() {
    queue_.clear();
}

void AudioOutput::setClock(double t) {
    std::lock_guard<std::mutex> lock(clockMutex_);
    writeHead_ = t;
}

double AudioOutput::clock() const {
    std::lock_guard<std::mutex> lock(clockMutex_);
    return writeHead_;
}

void AudioOutput::rebuildSonic(float spd) {
    if (spd <= 0.01f) spd = 0.01f;
    std::lock_guard<std::mutex> lock(sonicMutex_);
    if (sonic_) sonicDestroyStream(sonic_);
    sonic_ = sonicCreateStream(spec_.freq, spec_.channels);
    sonicSetSampleRate(sonic_, spec_.freq);
    sonicSetNumChannels(sonic_, spec_.channels);
    sonicSetSpeed(sonic_, spd);
    sonicSetPitch(sonic_, 1.0f);
    sonicSetRate(sonic_, 1.0f);
    sonicSetQuality(sonic_, 1);
    lastSpeed_ = spd;
    speed_.store(spd, std::memory_order_relaxed);
}

void AudioOutput::setSpeed(float spd) {
    if (spd <= 0.01f) spd = 0.01f;
    speed_.store(spd, std::memory_order_relaxed);
}

void AudioOutput::requestSpeedChange(float spd) {
    if (spd <= 0.01f) spd = 0.01f;
    pendingSpeed_.store(spd, std::memory_order_release);
}

float AudioOutput::pendingSpeed() const {
    return pendingSpeed_.load(std::memory_order_acquire);
}

bool AudioOutput::hasPendingSpeed() const {
    return pendingSpeed_.load(std::memory_order_acquire) >= 0.0f;
}

void AudioOutput::requestSeek(double t) {
    pendingSeek_.store(t, std::memory_order_release);
}

bool AudioOutput::hasPendingSeek() const {
    return pendingSeek_.load(std::memory_order_acquire) >= 0.0;
}

bool AudioOutput::push(const FramePtr& frame) {
    if (!ok_) return false;
    AudioChunk chunk;
    if (!convert(frame, chunk)) return true;
    return queue_.push(std::move(chunk));
}

bool AudioOutput::tryPush(const FramePtr& frame) {
    if (!ok_) return false;
    AudioChunk chunk;
    if (!convert(frame, chunk)) return true;
    return queue_.tryPush(std::move(chunk));
}

bool AudioOutput::convert(const FramePtr& frame, AudioChunk& chunk) {
    std::lock_guard<std::mutex> swrLock(swrMutex_);
    std::lock_guard<std::mutex> sLock(sonicMutex_);
    if (!swr_ || !sonic_) return false;

    int outSamples = (int)av_rescale_rnd(
        swr_get_delay(swr_, frame->sample_rate) + frame->nb_samples,
        spec_.freq, frame->sample_rate, AV_ROUND_UP);

    int channels = spec_.channels;
    int outBytes = outSamples * channels * (int)sizeof(int16_t);
    uint8_t* s16Buf = (uint8_t*)av_malloc(outBytes);
    if (!s16Buf) return false;

    uint8_t* outPlanes[1] = { s16Buf };
    int converted = swr_convert(swr_, outPlanes, outSamples,
                                (const uint8_t**)frame->extended_data,
                                frame->nb_samples);
    if (converted <= 0) { av_free(s16Buf); return false; }

    int validBytes = converted * channels * (int)sizeof(int16_t);

    float spd = speed_.load(std::memory_order_relaxed);
    if (std::abs(spd - 1.0f) < 0.001f) {
        chunk.data.assign(s16Buf, s16Buf + validBytes);
        av_free(s16Buf);
    } else {
        sonicWriteShortToStream(sonic_, (int16_t*)s16Buf, converted);
        av_free(s16Buf);

        int available = sonicSamplesAvailable(sonic_);
        if (available <= 0) return false;

        uint8_t* sonicOut = (uint8_t*)av_malloc(available * channels * (int)sizeof(int16_t));
        if (!sonicOut) return false;

        int readSamples = sonicReadShortFromStream(sonic_, (int16_t*)sonicOut, available);
        if (readSamples <= 0) { av_free(sonicOut); return false; }

        int readBytes = readSamples * channels * (int)sizeof(int16_t);
        chunk.data.assign(sonicOut, sonicOut + readBytes);
        av_free(sonicOut);
    }

    chunk.outRate = spec_.freq;
    chunk.pts = frame->pts == AV_NOPTS_VALUE ? 0.0 : frame->pts * ptsScale_;
    if (chunk.pts < 0.0) chunk.pts = 0.0;
    lastPts_ = chunk.pts;

    return !chunk.data.empty();
}

void AudioOutput::closeQueue() { queue_.close(); }

void AudioOutput::pauseDevice() { SDL_PauseAudioDevice(dev_, 1); }

void AudioOutput::resumeDevice() { SDL_PauseAudioDevice(dev_, 0); }

void SDLCALL AudioOutput::sdlCallback(void* userdata, Uint8* stream, int len) {
    static_cast<AudioOutput*>(userdata)->fill(stream, len);
}

void AudioOutput::fill(Uint8* stream, int len) {
    // 原子处理待处理的速度变更（在 SDL 回调线程内，零竞态）
    float newSpeed = pendingSpeed_.exchange(-1.0f, std::memory_order_acq_rel);
    if (newSpeed >= 0.0f) {
        // 清除 current_（旧速度数据），清空队列，重建 Sonic，保持时钟
        current_.data.clear();
        offset_ = 0;
        queue_.clear();
        {
            std::lock_guard<std::mutex> lock(sonicMutex_);
            if (sonic_) sonicDestroyStream(sonic_);
            sonic_ = sonicCreateStream(spec_.freq, spec_.channels);
            sonicSetSampleRate(sonic_, spec_.freq);
            sonicSetNumChannels(sonic_, spec_.channels);
            sonicSetSpeed(sonic_, newSpeed);
            sonicSetPitch(sonic_, 1.0f);
            sonicSetRate(sonic_, 1.0f);
            sonicSetQuality(sonic_, 1);
            lastSpeed_ = newSpeed;
        }
        speed_.store(newSpeed, std::memory_order_relaxed);
    }

    // 原子处理待处理的 seek（清 current_ + 清队列 + 设时钟）
    double seekTarget = pendingSeek_.exchange(-1.0, std::memory_order_acq_rel);
    if (seekTarget >= 0.0) {
        current_.data.clear();
        offset_ = 0;
        queue_.clear();
        setClock(seekTarget);
        reanchor_ = true;  // 首块到达时重锚 writeHead_
    }

    SDL_memset(stream, 0, len);
    int space = len;
    Uint8* dst = stream;

    while (space > 0) {
        if (current_.data.empty()) {
            AudioChunk chunk;
            if (!queue_.tryPop(chunk)) {
                // 队列空：输出静音，按实际播放时间推进时钟（speed 不影响时钟）
                std::lock_guard<std::mutex> lock(clockMutex_);
                if (writeHead_ >= 0.0) {
                    writeHead_ += (double)space / 4.0 / spec_.freq;
                }
                break;
            }
            current_ = std::move(chunk);
            offset_ = 0;
            {
                std::lock_guard<std::mutex> lock(clockMutex_);
                if (writeHead_ < 0.0 || reanchor_) {
                    writeHead_ = current_.pts;
                    reanchor_ = false;
                }
            }
        }
        size_t n = std::min<size_t>(current_.data.size() - offset_, (size_t)space);
        SDL_memcpy(dst, current_.data.data() + offset_, n);
        offset_ += n;
        dst += n;
        space -= n;
        if (offset_ >= current_.data.size()) current_.data.clear();
        {
            std::lock_guard<std::mutex> lock(clockMutex_);
            writeHead_ += (double)n / 4.0 / current_.outRate;
        }
    }

    applyVolume(stream, len);
}

void AudioOutput::applyVolume(Uint8* stream, int len) {
    float v = volume_.load(std::memory_order_relaxed);
    bool norm = normalization_.load(std::memory_order_relaxed);

    float normGain = 1.0f;
    if (norm) {
        int16_t* samples = (int16_t*)stream;
        int numSamples = len / 2;
        float maxPeak = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            float s = std::abs((float)samples[i]);
            if (s > maxPeak) maxPeak = s;
        }

        Uint32 now = SDL_GetTicks();
        if (now - peakDecayTime_ > 1000) {
            peakTracker_ *= 0.5f;
            peakDecayTime_ = now;
        }
        if (maxPeak > peakTracker_) peakTracker_ = maxPeak;

        float targetPeak = 28672.0f;
        if (peakTracker_ > 100.0f) {
            normGain = targetPeak / peakTracker_;
            if (normGain < 0.25f) normGain = 0.25f;
            if (normGain > 4.0f) normGain = 4.0f;
        }
        normGain_.store(normGain, std::memory_order_relaxed);
    }

    float totalGain = v * normGain;
    if (totalGain >= 1.0f && !norm) return;
    if (totalGain <= 0.0f) {
        SDL_memset(stream, 0, len);
        return;
    }

    int16_t* p = (int16_t*)stream;
    int n = len / 2;
    for (int i = 0; i < n; ++i) {
        float s = p[i] * totalGain;
        if (s > 32767.0f || s < -32768.0f) {
            s = s / 32768.0f;
            s = std::tanh(s) * 32768.0f;
        }
        p[i] = (int16_t)s;
    }
}
