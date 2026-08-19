#include "audio/audio_output.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
}

AudioOutput::~AudioOutput() {
    if (dev_) {
        SDL_PauseAudioDevice(dev_, 1);
        SDL_CloseAudioDevice(dev_);
    }
    if (swr_) swr_free(&swr_);
    if (mixTemp_) SDL_free(mixTemp_);
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

    bytesPerSec_ = (double)spec_.freq * spec_.channels * 2;
    mixTemp_ = (Uint8*)SDL_malloc(spec_.size > 0 ? spec_.size : 8192);
    if (!mixTemp_) return false;

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

    ok_ = true;
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

bool AudioOutput::push(const FramePtr& frame) {
    if (!ok_) return false;

    int outChannels = spec_.channels;
    int outSamples = (int)av_rescale_rnd(
        swr_get_delay(swr_, frame->sample_rate) + frame->nb_samples,
        spec_.freq, frame->sample_rate, AV_ROUND_UP);

    uint8_t* outBuf = nullptr;
    if (av_samples_alloc(&outBuf, nullptr, outChannels, outSamples,
                         AV_SAMPLE_FMT_S16, 0) < 0)
        return false;

    int converted = swr_convert(swr_, &outBuf, outSamples,
                                (const uint8_t**)frame->extended_data,
                                frame->nb_samples);

    AudioChunk chunk;
    if (converted > 0) {
        int bytes = av_samples_get_buffer_size(nullptr, outChannels, converted,
                                               AV_SAMPLE_FMT_S16, 1);
        chunk.data.assign(outBuf, outBuf + bytes);
        chunk.pts = frame->pts == AV_NOPTS_VALUE ? 0.0 : frame->pts * ptsScale_;
        if (chunk.pts < 0.0) chunk.pts = 0.0;
    }
    av_freep(&outBuf);
    if (chunk.data.empty()) return true;
    return queue_.push(std::move(chunk));
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

double AudioOutput::clock() const {
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
    }

    {
        std::lock_guard<std::mutex> lock(clockMutex_);
        if (space != len) writeHead_ += (double)(len - space) / bytesPerSec_;
    }
    applyVolume(stream, len);
}

void AudioOutput::applyVolume(Uint8* stream, int len) {
    float v = volume_.load(std::memory_order_relaxed);
    if (v >= 1.0f) return;
    int vol = (int)(v * 128.0f);
    SDL_memcpy(mixTemp_, stream, len);
    SDL_MixAudioFormat(stream, mixTemp_, AUDIO_S16SYS, len, vol);
}