#include "audio/audio_output.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
}

AudioOutput::AudioOutput() {
    inLayout_ = {};  // AV_CHANNEL_ORDER_UNSPEC，av_channel_layout_uninit 安全
}

AudioOutput::~AudioOutput() {
    if (dev_) {
        SDL_PauseAudioDevice(dev_, 1);
        SDL_CloseAudioDevice(dev_);
    }
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

    std::lock_guard<std::mutex> lock(swrMutex_);
    inSampleRate_ = par->sample_rate;
    av_channel_layout_uninit(&inLayout_);
    av_channel_layout_copy(&inLayout_, &par->ch_layout);
    inFmt_ = (AVSampleFormat)par->format;

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, spec_.channels);
    swr_ = swr_alloc();
    if (!swr_) return false;
    av_opt_set_chlayout(swr_, "in_chlayout", &inLayout_, 0);
    av_opt_set_int(swr_, "in_sample_rate", inSampleRate_, 0);
    av_opt_set_sample_fmt(swr_, "in_sample_fmt", inFmt_, 0);
    av_opt_set_chlayout(swr_, "out_chlayout", &outLayout, 0);
    av_opt_set_int(swr_, "out_sample_rate", spec_.freq, 0);
    av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    int ret = swr_init(swr_);
    av_channel_layout_uninit(&outLayout);
    if (ret < 0) return false;

    ok_ = true;
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void AudioOutput::setSpeed(float spd) {
    if (spd <= 0.01f) spd = 0.01f;
    speed_.store(spd, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(swrMutex_);
    if (!swr_) return;
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, spec_.channels);
    SwrContext* nswr = swr_alloc();
    if (!nswr) return;
    av_opt_set_chlayout(nswr, "in_chlayout", &inLayout_, 0);
    av_opt_set_int(nswr, "in_sample_rate", inSampleRate_, 0);
    av_opt_set_sample_fmt(nswr, "in_sample_fmt", inFmt_, 0);
    av_opt_set_chlayout(nswr, "out_chlayout", &outLayout, 0);
    av_opt_set_int(nswr, "out_sample_rate", (int)(spec_.freq / spd + 0.5f), 0);
    av_opt_set_sample_fmt(nswr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    int ret = swr_init(nswr);
    av_channel_layout_uninit(&outLayout);
    if (ret < 0) {
        swr_free(&nswr);
        return;
    }
    swr_free(&swr_);
    swr_ = nswr;
    // 变速后旧缓冲按旧采样率推进会污染时钟：标记由 fill 回调内清空（无锁安全）
    clearPending_.store(true);
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
    std::lock_guard<std::mutex> lock(swrMutex_);
    if (!swr_) return false;
    int outChannels = spec_.channels;
    float spd = speed_.load(std::memory_order_relaxed);
    int outRate = (int)(spec_.freq / spd + 0.5f);
    int outSamples = (int)av_rescale_rnd(
        swr_get_delay(swr_, frame->sample_rate) + frame->nb_samples,
        outRate, frame->sample_rate, AV_ROUND_UP);

    uint8_t* outBuf = nullptr;
    if (av_samples_alloc(&outBuf, nullptr, outChannels, outSamples,
                         AV_SAMPLE_FMT_S16, 0) < 0)
        return false;

    int converted = swr_convert(swr_, &outBuf, outSamples,
                                (const uint8_t**)frame->extended_data,
                                frame->nb_samples);

    if (converted > 0) {
        int bytes = av_samples_get_buffer_size(nullptr, outChannels, converted,
                                               AV_SAMPLE_FMT_S16, 1);
        chunk.data.assign(outBuf, outBuf + bytes);
        chunk.outRate = outRate;
        chunk.pts = frame->pts == AV_NOPTS_VALUE ? 0.0 : frame->pts * ptsScale_;
        if (chunk.pts < 0.0) chunk.pts = 0.0;
    }
    av_freep(&outBuf);
    return !chunk.data.empty();
}

void AudioOutput::closeQueue() { queue_.close(); }

void AudioOutput::clearQueue() {
    queue_.clear();
    current_.data.clear();
    offset_ = 0;
}

void AudioOutput::pauseDevice() { SDL_PauseAudioDevice(dev_, 1); }

void AudioOutput::resumeDevice() { SDL_PauseAudioDevice(dev_, 0); }

void AudioOutput::resetClock() {
    std::lock_guard<std::mutex> lock(clockMutex_);
    writeHead_ = -1.0;
}

void AudioOutput::setClock(double t) {
    std::lock_guard<std::mutex> lock(clockMutex_);
    writeHead_ = t;
}

double AudioOutput::clock() const {
    std::lock_guard<std::mutex> lock(clockMutex_);
    return writeHead_;
}

void SDLCALL AudioOutput::sdlCallback(void* userdata, Uint8* stream, int len) {
    static_cast<AudioOutput*>(userdata)->fill(stream, len);
}

void AudioOutput::fill(Uint8* stream, int len) {
    if (clearPending_.exchange(false)) {
        queue_.clear();
        current_.data.clear();
        offset_ = 0;
    }
    SDL_memset(stream, 0, len);
    int space = len;
    Uint8* dst = stream;

    while (space > 0) {
        if (current_.data.empty()) {
            AudioChunk chunk;
            if (!queue_.tryPop(chunk)) break;
            current_ = std::move(chunk);
            offset_ = 0;
            {
                std::lock_guard<std::mutex> lock(clockMutex_);
                if (writeHead_ < 0.0 || current_.pts - writeHead_ > 0.5 ||
                    current_.pts < writeHead_ - 0.5) {
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
            // 该 chunk 按自身输出采样率换算内容秒：n 字节 = n/4 采样（S16 双声道）
            writeHead_ += (double)n / 4.0 / current_.outRate;
        }
    }

    applyVolume(stream, len);
}

void AudioOutput::applyVolume(Uint8* stream, int len) {
    float v = volume_.load(std::memory_order_relaxed);
    bool norm = normalization_.load(std::memory_order_relaxed);

    // 音量标准化：峰值检测 + 增益调节
    float normGain = 1.0f;
    if (norm) {
        int16_t* samples = (int16_t*)stream;
        int numSamples = len / 2;
        float maxPeak = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            float s = std::abs((float)samples[i]);
            if (s > maxPeak) maxPeak = s;
        }

        // 滑动最大值衰减（1秒衰减一次）
        Uint32 now = SDL_GetTicks();
        if (now - peakDecayTime_ > 1000) {
            peakTracker_ *= 0.5f;  // 半衰
            peakDecayTime_ = now;
        }
        if (maxPeak > peakTracker_) peakTracker_ = maxPeak;

        // 目标峰值：-1 dBFS ≈ 28672 (S16 范围 32768)
        float targetPeak = 28672.0f;
        if (peakTracker_ > 100.0f) {
            normGain = targetPeak / peakTracker_;
            // 限制增益范围：0.25x ~ 4.0x（防止过放大或过小）
            if (normGain < 0.25f) normGain = 0.25f;
            if (normGain > 4.0f) normGain = 4.0f;
        }
        normGain_.store(normGain, std::memory_order_relaxed);
    }

    float totalGain = v * normGain;
    if (totalGain >= 1.0f && !norm) return;  // 仅音量无标准化时快速返回
    if (totalGain <= 0.0f) {
        SDL_memset(stream, 0, len);
        return;
    }

    int16_t* p = (int16_t*)stream;
    int n = len / 2;
    for (int i = 0; i < n; ++i) {
        float s = p[i] * totalGain;
        // 软限幅（tanh 曲线）
        if (s > 32767.0f || s < -32768.0f) {
            s = s / 32768.0f;
            s = std::tanh(s) * 32768.0f;
        }
        p[i] = (int16_t)s;
    }
}