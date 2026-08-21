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

// 必须在 pauseDevice() 之后、resumeDevice() 之前调用
// 因为 fill() 不会运行，所以 current_/offset_/queue_/clock 无竞态
void AudioOutput::clearAndReset(double newClock) {
    queue_.clear();
    current_.data.clear();
    offset_ = 0;
    {
        std::lock_guard<std::mutex> lock(clockMutex_);
        writeHead_ = newClock;
    }
}

// 必须在 pauseDevice() 之后、resumeDevice() 之前调用
void AudioOutput::setSpeedAndReset(float spd) {
    if (spd <= 0.01f) spd = 0.01f;
    {
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
    }
    speed_.store(spd, std::memory_order_relaxed);
}

// 仅更新 speed（不清队列不重建 Sonic），用于微调
void AudioOutput::setSpeed(float spd) {
    if (spd <= 0.01f) spd = 0.01f;
    speed_.store(spd, std::memory_order_relaxed);
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

void AudioOutput::pauseDevice() {
    devicePaused_.store(true, std::memory_order_relaxed);
    SDL_PauseAudioDevice(dev_, 1);
}

void AudioOutput::resumeDevice() {
    SDL_PauseAudioDevice(dev_, 0);
    devicePaused_.store(false, std::memory_order_relaxed);
}

void AudioOutput::setClock(double t) {
    std::lock_guard<std::mutex> lock(clockMutex_);
    writeHead_ = t;
}

double AudioOutput::clock() const {
    if (devicePaused_.load(std::memory_order_relaxed)) return -1.0;
    std::lock_guard<std::mutex> lock(clockMutex_);
    return writeHead_;
}

void SDLCALL AudioOutput::sdlCallback(void* userdata, Uint8* stream, int len) {
    static_cast<AudioOutput*>(userdata)->fill(stream, len);
}

void AudioOutput::fill(Uint8* stream, int len) {
    SDL_memset(stream, 0, len);
    int space = len;
    Uint8* dst = stream;

    while (space > 0) {
        if (current_.data.empty()) {
            AudioChunk chunk;
            if (!queue_.tryPop(chunk)) {
                std::lock_guard<std::mutex> lock(clockMutex_);
                if (writeHead_ >= 0.0) {
                    writeHead_ += (double)space / 4.0 / spec_.freq
                                  * speed_.load(std::memory_order_relaxed);
                }
                break;
            }
            current_ = std::move(chunk);
            offset_ = 0;
            {
                std::lock_guard<std::mutex> lock(clockMutex_);
                if (writeHead_ < 0.0) {
                    writeHead_ = current_.pts;
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
            writeHead_ += (double)n / 4.0 / current_.outRate * speed_.load(std::memory_order_relaxed);
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
