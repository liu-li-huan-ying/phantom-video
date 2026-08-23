#include "audio/audio_output.h"
#include "core/logger.h"

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
    if (!dev_) {
        LOG_ERROR("AUDIO", "SDL_OpenAudioDevice FAILED: %s", SDL_GetError());
        return false;
    }
    LOG_INFO("AUDIO", "open: dev=%u freq=%d ch=%d samples=%d",
             dev_, spec_.freq, spec_.channels, spec_.samples);

    {
        std::lock_guard<std::mutex> lock(swrMutex_);
        av_channel_layout_uninit(&inLayout_);
        av_channel_layout_copy(&inLayout_, &par->ch_layout);

        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, spec_.channels);
        swr_ = swr_alloc();
        if (!swr_) {
            LOG_ERROR("AUDIO", "swr_alloc FAILED");
            return false;
        }
        av_opt_set_chlayout(swr_, "in_chlayout", &par->ch_layout, 0);
        av_opt_set_int(swr_, "in_sample_rate", par->sample_rate, 0);
        av_opt_set_sample_fmt(swr_, "in_sample_fmt", (AVSampleFormat)par->format, 0);
        av_opt_set_chlayout(swr_, "out_chlayout", &outLayout, 0);
        av_opt_set_int(swr_, "out_sample_rate", spec_.freq, 0);
        av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        int ret = swr_init(swr_);
        av_channel_layout_uninit(&outLayout);
        if (ret < 0) {
            LOG_ERROR("AUDIO", "swr_init FAILED: %d", ret);
            return false;
        }
        LOG_INFO("AUDIO", "swr: in_rate=%d out_rate=%d in_fmt=%d -> out_fmt=S16",
                 par->sample_rate, spec_.freq, par->format);
    }

    sonic_ = sonicCreateStream(spec_.freq, spec_.channels);
    if (!sonic_) {
        LOG_ERROR("AUDIO", "sonicCreateStream FAILED");
        return false;
    }
    sonicSetSampleRate(sonic_, spec_.freq);
    sonicSetNumChannels(sonic_, spec_.channels);
    sonicSetSpeed(sonic_, 1.0f);
    sonicSetPitch(sonic_, 1.0f);
    sonicSetRate(sonic_, 1.0f);
    sonicSetQuality(sonic_, 1);

    ok_ = true;
    SDL_PauseAudioDevice(dev_, 0);
    LOG_INFO("AUDIO", "open OK: device started");
    return true;
}

void AudioOutput::clearQueue() {
    queue_.clear();
    LOG_DBG("FILL", "clearQueue called");
}

void AudioOutput::setClock(double t) {
    std::lock_guard<std::mutex> lock(clockMutex_);
    LOG_DBG("FILL", "setClock: writeHead_ %.3f -> %.3f", writeHead_, t);
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
    LOG_DBG("SPEED", "rebuildSonic: speed=%.2f", spd);
}

void AudioOutput::setSpeed(float spd) {
    if (spd <= 0.01f) spd = 0.01f;
    speed_.store(spd, std::memory_order_relaxed);
    LOG_DBG("SPEED", "setSpeed (atomic only): %.2f", spd);
}

void AudioOutput::requestSpeedChange(float spd, double anchor) {
    if (spd <= 0.01f) spd = 0.01f;
    if (anchor >= 0.0) {
        pendingSpeedAnchor_.store(anchor, std::memory_order_release);
    }
    pendingSpeed_.store(spd, std::memory_order_release);
    LOG_DBG("SPEED", "requestSpeedChange: spd=%.2f anchor=%.3f", spd, anchor);
}

float AudioOutput::pendingSpeed() const {
    return pendingSpeed_.load(std::memory_order_acquire);
}

bool AudioOutput::hasPendingSpeed() const {
    return pendingSpeed_.load(std::memory_order_acquire) >= 0.0f;
}

void AudioOutput::requestSeek(double t) {
    pendingSeek_.store(t, std::memory_order_release);
    LOG_DBG("FILL", "requestSeek: t=%.3f", t);
}

bool AudioOutput::hasPendingSeek() const {
    return pendingSeek_.load(std::memory_order_acquire) >= 0.0;
}

void AudioOutput::setSeeking(bool s) {
    seeking_.store(s, std::memory_order_release);
    LOG_DBG("AUDIO", "setSeeking: %d", s);
}

bool AudioOutput::push(const FramePtr& frame) {
    if (!ok_) {
        LOG_WARN("AUDIO", "push: not ok, dropping frame");
        return false;
    }
    if (seeking_.load(std::memory_order_acquire)) {
        LOG_DBG("AUDIO", "push: BLOCKED by seeking_");
        return false;
    }
    AudioChunk chunk;
    if (!convert(frame, chunk)) return true;
    bool ok = queue_.push(std::move(chunk));
    if (!ok) LOG_WARN("AUDIO", "push: queue push FAILED (queue full?)");
    return ok;
}

bool AudioOutput::tryPush(const FramePtr& frame) {
    if (!ok_) return false;
    if (seeking_.load(std::memory_order_acquire)) return false;
    AudioChunk chunk;
    if (!convert(frame, chunk)) return true;
    bool ok = queue_.tryPush(std::move(chunk));
    if (!ok) LOG_DBG("AUDIO", "tryPush: queue full, dropped frame");
    return ok;
}

bool AudioOutput::convert(const FramePtr& frame, AudioChunk& chunk) {
    std::lock_guard<std::mutex> swrLock(swrMutex_);
    std::lock_guard<std::mutex> sLock(sonicMutex_);
    if (!swr_ || !sonic_) {
        LOG_WARN("AUDIO", "convert: swr_=%p sonic_=%p, dropping frame", swr_, sonic_);
        return false;
    }

    int outSamples = (int)av_rescale_rnd(
        swr_get_delay(swr_, frame->sample_rate) + frame->nb_samples,
        spec_.freq, frame->sample_rate, AV_ROUND_UP);

    int channels = spec_.channels;
    int outBytes = outSamples * channels * (int)sizeof(int16_t);
    uint8_t* s16Buf = (uint8_t*)av_malloc(outBytes);
    if (!s16Buf) {
        LOG_ERROR("AUDIO", "convert: av_malloc FAILED (%d bytes)", outBytes);
        return false;
    }

    uint8_t* outPlanes[1] = { s16Buf };
    int converted = swr_convert(swr_, outPlanes, outSamples,
                                (const uint8_t**)frame->extended_data,
                                frame->nb_samples);
    if (converted <= 0) {
        LOG_WARN("AUDIO", "convert: swr_convert FAILED: %d (in_samples=%d)", converted, frame->nb_samples);
        av_free(s16Buf);
        return false;
    }

    int validBytes = converted * channels * (int)sizeof(int16_t);

    float spd = speed_.load(std::memory_order_relaxed);
    if (std::abs(spd - 1.0f) < 0.001f) {
        chunk.data.assign(s16Buf, s16Buf + validBytes);
        av_free(s16Buf);
    } else {
        sonicWriteShortToStream(sonic_, (int16_t*)s16Buf, converted);
        av_free(s16Buf);

        int available = sonicSamplesAvailable(sonic_);
        if (available <= 0) {
            LOG_DBG("AUDIO", "convert: sonicSamplesAvailable=0 (speed=%.2f)", spd);
            return false;
        }

        uint8_t* sonicOut = (uint8_t*)av_malloc(available * channels * (int)sizeof(int16_t));
        if (!sonicOut) return false;

        int readSamples = sonicReadShortFromStream(sonic_, (int16_t*)sonicOut, available);
        if (readSamples <= 0) {
            LOG_DBG("AUDIO", "convert: sonicRead FAILED (available=%d)", available);
            av_free(sonicOut);
            return false;
        }

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

void AudioOutput::closeQueue() {
    queue_.close();
    LOG_DBG("FILL", "closeQueue called");
}

void AudioOutput::pauseDevice() {
    SDL_PauseAudioDevice(dev_, 1);
    LOG_DBG("AUDIO", "pauseDevice: dev=%u", dev_);
}

void AudioOutput::resumeDevice() {
    SDL_PauseAudioDevice(dev_, 0);
    LOG_DBG("AUDIO", "resumeDevice: dev=%u", dev_);
}

void SDLCALL AudioOutput::sdlCallback(void* userdata, Uint8* stream, int len) {
    static_cast<AudioOutput*>(userdata)->fill(stream, len);
}

void AudioOutput::fill(Uint8* stream, int len) {
    // 诊断：每 200 次回调（约 4.6 秒）记录一次基线
    static int g_fillCount = 0;
    g_fillCount++;
    if (g_fillCount == 1) {
        LOG_DBG("FILL","FIRST CALL: len=%d spec_.freq=%d writeHead_=%.3f speed=%.2f reanchor=%d vol=%.2f",
            len, spec_.freq, writeHead_, speed_.load(), reanchor_, volume_.load());
    }
    if (g_fillCount % 200 == 0) {
        LOG_DBG("FILL","tick=%u count=%d speed=%.2f reanchor=%d writeHead_=%.3f vol=%.2f",
            SDL_GetTicks(), g_fillCount, speed_.load(), reanchor_, writeHead_,
            volume_.load());
    }

    // 原子处理待处理的速度变更（在 SDL 回调线程内，零竞态）
    float newSpeed = pendingSpeed_.exchange(-1.0f, std::memory_order_acq_rel);
    if (newSpeed >= 0.0f) {
        double anchor = pendingSpeedAnchor_.exchange(-1.0, std::memory_order_acq_rel);
        LOG_DBG("FILL","pendingSpeed consumed: speed=%.2f anchor=%.3f writeHead_=%.3f", newSpeed, anchor, writeHead_);
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
        // 原子锚定时钟：anchor 由 Player::setSpeed 在 requestSpeedChange 时传入
        if (anchor >= 0.0) {
            std::lock_guard<std::mutex> lock(clockMutex_);
            writeHead_ = anchor;
            LOG_DBG("FILL","pendingSpeed anchor: writeHead_=%.3f", writeHead_);
        }
        reanchor_ = true;
        reanchorSpeed_ = true;
    }

    // 原子处理待处理的 seek（清 current_ + 清队列 + 设时钟）
    double seekTarget = pendingSeek_.exchange(-1.0, std::memory_order_acq_rel);
    if (seekTarget >= 0.0) {
        LOG_DBG("FILL","pendingSeek consumed: target=%.3f writeHead_before=%.3f", seekTarget, writeHead_);
        current_.data.clear();
        offset_ = 0;
        queue_.clear();
        setClock(seekTarget);
        reanchor_ = true;
        reanchorSpeed_ = false;
        LOG_DBG("FILL","pendingSeek done: writeHead_after=%.3f reanchor=%d", writeHead_, reanchor_);
    }

    SDL_memset(stream, 0, len);
    int space = len;
    Uint8* dst = stream;

    while (space > 0) {
        if (current_.data.empty()) {
            AudioChunk chunk;
            if (!queue_.tryPop(chunk)) {
                // 队列空：输出静音，按内容时间推进时钟
                std::lock_guard<std::mutex> lock(clockMutex_);
                if (writeHead_ >= 0.0) {
                    double before = writeHead_;
                    writeHead_ += (double)space / 4.0 / spec_.freq
                                  * speed_.load(std::memory_order_relaxed);
                    if (reanchor_) {
                        LOG_TRACE("FILL","SILENCE+REANCHOR: %.3f -> %.3f speed=%.2f space=%d",
                            before, writeHead_, speed_.load(std::memory_order_relaxed), space);
                    }
                }
                // 每 50 次空队列静音输出记录一次（避免刷屏）
                static int silenceCount = 0;
                silenceCount++;
                if (silenceCount % 50 == 0) {
                    LOG_WARN("FILL","SILENCE: queue empty x%d, writeHead_=%.3f vol=%.2f speed=%.2f reanchor=%d",
                        silenceCount, writeHead_, volume_.load(), speed_.load(), reanchor_);
                }
                break;
            }
            current_ = std::move(chunk);
            offset_ = 0;
        {
            std::lock_guard<std::mutex> lock(clockMutex_);
            if (writeHead_ < 0.0 || reanchor_) {
                double diff = current_.pts - writeHead_;
                if (reanchor_ && !reanchorSpeed_ && writeHead_ > 0.0 && (diff > 2.0 || diff < -2.0)) {
                    LOG_WARN("FILL","REANCHOR SKIP: chunk.pts %.3f way off writeHead_ %.3f (diff=%.3f), discarding chunk",
                        current_.pts, writeHead_, diff);
                    current_.data.clear();
                    offset_ = 0;
                } else {
                    if (reanchor_ && std::abs(diff) > 2.0) {
                        LOG_DBG("FILL","REANCHOR: writeHead_ %.3f -> chunk.pts %.3f (JUMP %.3fs, speedChange=%d) speed=%.2f",
                            writeHead_, current_.pts, diff, reanchorSpeed_, speed_.load());
                    } else if (reanchor_) {
                        LOG_DBG("FILL","REANCHOR: writeHead_ %.3f -> chunk.pts %.3f speed=%.2f",
                            writeHead_, current_.pts, speed_.load());
                    }
                    writeHead_ = current_.pts;
                    reanchor_ = false;
                    reanchorSpeed_ = false;
                }
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
            writeHead_ += (double)n / 4.0 / current_.outRate
                          * speed_.load(std::memory_order_relaxed);
        }
    }

    applyVolume(stream, len);
}

void AudioOutput::applyVolume(Uint8* stream, int len) {
    float v = volume_.load(std::memory_order_relaxed);
    bool norm = normalization_.load(std::memory_order_relaxed);

    // 静音检测：volume=0 时记录
    static int zeroVolCount = 0;
    if (v <= 0.001f) {
        zeroVolCount++;
        if (zeroVolCount % 200 == 1) {
            LOG_WARN("AUDIO", "applyVolume: volume=%.3f (MUTED!) x%d", v, zeroVolCount);
        }
    } else {
        zeroVolCount = 0;
    }

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
        LOG_WARN("AUDIO", "applyVolume: totalGain=%.3f (zeroing stream!) vol=%.2f norm=%.2f",
                 totalGain, v, normGain);
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
