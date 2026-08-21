#include "audio/audio_output.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    destroyFilterGraph();
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
        inSampleRate_ = par->sample_rate;
        av_channel_layout_uninit(&inLayout_);
        av_channel_layout_copy(&inLayout_, &par->ch_layout);
        inFmt_ = (AVSampleFormat)par->format;

        // SwrContext: 固定 float→S16, 44100Hz（速度由 atempo 控制）
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, spec_.channels);
        swr_ = swr_alloc();
        if (!swr_) return false;
        av_opt_set_chlayout(swr_, "in_chlayout", &inLayout_, 0);
        av_opt_set_int(swr_, "in_sample_rate", inSampleRate_, 0);
        av_opt_set_sample_fmt(swr_, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
        av_opt_set_chlayout(swr_, "out_chlayout", &outLayout, 0);
        av_opt_set_int(swr_, "out_sample_rate", spec_.freq, 0);
        av_opt_set_sample_fmt(swr_, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        int ret = swr_init(swr_);
        av_channel_layout_uninit(&outLayout);
        if (ret < 0) return false;
    }

    // 创建初始滤镜图（speed=1.0 时 atempo=1.0 等效直通）
    buildFilterGraph(speed_.load(std::memory_order_relaxed));

    ok_ = true;
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void AudioOutput::buildFilterGraph(float speed) {
    destroyFilterGraph();

    char args[512];
    char filterArgs[64];
    std::snprintf(filterArgs, sizeof(filterArgs), "%.4f", speed);

    filterGraph_ = avfilter_graph_alloc();
    if (!filterGraph_) return;

    srcFilter_ = avfilter_get_by_name("abuffer");
    sinkFilter_ = avfilter_get_by_name("abuffersink");
    if (!srcFilter_ || !sinkFilter_) { destroyFilterGraph(); return; }

    // abuffer: 接收解码后的 float 帧
    char chLayoutStr[64] = {};
    AVChannelLayout chLayout;
    av_channel_layout_copy(&chLayout, &inLayout_);
    // 取第一个描述名（如 "stereo"、"5.1"）
    av_channel_layout_describe(&chLayout, chLayoutStr, sizeof(chLayoutStr));
    av_channel_layout_uninit(&chLayout);
    std::snprintf(args, sizeof(args),
        "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
        inSampleRate_, inSampleRate_,
        av_get_sample_fmt_name(inFmt_),
        chLayoutStr);

    if (avfilter_graph_create_filter(&bufferSrcCtx_, srcFilter_, "in", args, nullptr, filterGraph_) < 0) {
        destroyFilterGraph(); return;
    }
    if (avfilter_graph_create_filter(&bufferSinkCtx_, sinkFilter_, "out", nullptr, nullptr, filterGraph_) < 0) {
        destroyFilterGraph(); return;
    }

    // aformat: 转为 float + 匹配通道布局
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = bufferSrcCtx_;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = bufferSinkCtx_;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    char graphDesc[256];
    std::snprintf(graphDesc, sizeof(graphDesc),
        "aformat=sample_fmts=flt:channel_layouts=stereo,atempo=%s", filterArgs);

    int ret = avfilter_graph_parse_ptr(filterGraph_, graphDesc, &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) { destroyFilterGraph(); return; }

    if (avfilter_graph_config(filterGraph_, nullptr) < 0) {
        destroyFilterGraph(); return;
    }

    filterFrame_ = av_frame_alloc();
    filterOutFrame_ = av_frame_alloc();
}

void AudioOutput::destroyFilterGraph() {
    if (filterFrame_) { av_frame_free(&filterFrame_); filterFrame_ = nullptr; }
    if (filterOutFrame_) { av_frame_free(&filterOutFrame_); filterOutFrame_ = nullptr; }
    if (filterGraph_) { avfilter_graph_free(&filterGraph_); filterGraph_ = nullptr; }
    bufferSrcCtx_ = nullptr;
    bufferSinkCtx_ = nullptr;
}

void AudioOutput::setSpeed(float spd) {
    if (spd <= 0.01f) spd = 0.01f;
    speed_.store(spd, std::memory_order_relaxed);
    speedChanged_.store(true, std::memory_order_relaxed);
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

    // 速度变化时：重建滤镜图（旧 chunks 在队列中自然播放完毕，无噪点）
    if (speedChanged_.exchange(false)) {
        buildFilterGraph(speed_.load(std::memory_order_relaxed));
    }
    if (!filterGraph_) {
        buildFilterGraph(speed_.load(std::memory_order_relaxed));
        if (!filterGraph_) return false;
    }

    // 将解码帧送入滤镜图
    av_frame_ref(filterFrame_, frame.get());
    int ret = av_buffersrc_add_frame_flags(bufferSrcCtx_, filterFrame_, AV_BUFFERSRC_FLAG_PUSH);
    av_frame_unref(filterFrame_);
    if (ret < 0) return false;

    // 从滤镜图接收处理后的帧
    ret = av_buffersink_get_frame(bufferSinkCtx_, filterOutFrame_);
    if (ret < 0) return false;

    // float → S16 转换
    int nb = filterOutFrame_->nb_samples;
    int ch = spec_.channels;
    int s16Bytes = av_samples_get_buffer_size(nullptr, ch, nb, AV_SAMPLE_FMT_S16, 1);

    uint8_t* s16Buf = nullptr;
    if (av_samples_alloc(&s16Buf, nullptr, ch, nb, AV_SAMPLE_FMT_S16, 0) < 0) {
        av_frame_unref(filterOutFrame_);
        return false;
    }

    const float* src = (const float*)filterOutFrame_->data[0];
    int16_t* dst = (int16_t*)s16Buf;
    int total = nb * ch;
    for (int i = 0; i < total; ++i) {
        float s = src[i] * 32768.0f;
        s = std::clamp(s, -32768.0f, 32767.0f);
        dst[i] = (int16_t)s;
    }

    chunk.data.assign(s16Buf, s16Buf + s16Bytes);
    chunk.outRate = spec_.freq;
    chunk.pts = filterOutFrame_->pts == AV_NOPTS_VALUE ? 0.0 :
                filterOutFrame_->pts * av_q2d(bufferSinkCtx_->inputs[0]->time_base);
    if (chunk.pts < 0.0) chunk.pts = 0.0;

    av_freep(&s16Buf);
    av_frame_unref(filterOutFrame_);
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
