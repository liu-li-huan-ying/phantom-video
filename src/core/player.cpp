#include "core/player.h"

#include <algorithm>

#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>

Player::~Player() { close(); }

bool Player::openFile(const std::string& path) {
    close();
    videoQueue_.reopen();

    auto vdemux = std::make_unique<Demuxer>();
    if (!vdemux->open(path)) {
        error_ = "无法打开文件或格式不受支持";
        return false;
    }
    if (vdemux->videoIndex() < 0) {
        error_ = "文件中没有视频流";
        return false;
    }

    if (!hwDeviceCtx_) {
        for (enum AVHWDeviceType t : { AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2 }) {
            if (av_hwdevice_ctx_create(&hwDeviceCtx_, t, nullptr, nullptr, 0) >= 0)
                break;
        }
    }

    auto vdec = std::make_unique<Decoder>();
    if (!vdec->open(vdemux->videoCodecpar(), hwDeviceCtx_)) {
        error_ = "视频解码器初始化失败";
        return false;
    }
    hwDecode_.store(vdec->usingHardware());

    std::unique_ptr<Demuxer> ademux;
    std::unique_ptr<Decoder> adec;
    std::unique_ptr<AudioOutput> audio;
    bool hasAudio = vdemux->audioIndex() >= 0;
    if (hasAudio) {
        ademux = std::make_unique<Demuxer>();
        if (!ademux->open(path)) {
            ademux.reset();
            hasAudio = false;
        }
    }
    if (hasAudio) {
        adec = std::make_unique<Decoder>();
        if (adec->open(ademux->audioCodecpar())) {
            audio = std::make_unique<AudioOutput>();
            double aScale = av_q2d(ademux->audioStream()->time_base);
            if (!audio->open(ademux->audioCodecpar(), aScale)) {
                audio.reset();
                hasAudio = false;
            }
        } else {
            adec.reset();
            hasAudio = false;
        }
    }
    if (hasAudio && !ademux) hasAudio = false;

    videoDemuxer_ = std::move(vdemux);
    audioDemuxer_ = std::move(ademux);
    videoDecoder_ = std::move(vdec);
    audioDecoder_ = std::move(adec);
    audio_ = std::move(audio);

    videoPtsIdx_ = videoDemuxer_->videoIndex();
    videoPtsScale_.store(av_q2d(videoDemuxer_->videoStream()->time_base));
    duration_ = videoDemuxer_->duration();
    if (duration_ <= 0.0 && videoDemuxer_->videoStream()->duration != AV_NOPTS_VALUE) {
        duration_ = videoDemuxer_->videoStream()->duration *
                    av_q2d(videoDemuxer_->videoStream()->time_base);
    }

    subtitles_.clear();
    subtitleLoaded_.store(false);
    subtitleIndex_ = videoDemuxer_->subtitleIndex();
    subtitleDecoder_.reset();
    if (subtitleIndex_ >= 0) {
        auto sdec = std::make_unique<SubtitleDecoder>();
        if (sdec->open(videoDemuxer_->subtitleCodecpar()))
            subtitleDecoder_ = std::move(sdec);
    }

    path_ = path;
    error_.clear();
    videoEnabled_.store(true);
    audioEnabled_.store(hasAudio);
    hasMedia_.store(true);
    stop_.store(false);
    dropUntil_.store(-1e9);
    videoClockStarted_ = false;
    lastFrame_.reset();
    paused_.store(false);
    playing_ = true;
    videoBasePts_ = 0.0;
    videoBaseTicks_ = SDL_GetPerformanceCounter();
    state_.store(State::Playing);

    if (audio_) audio_->setSpeed(speed_.load(std::memory_order_relaxed));
    audioThread_ = std::thread(&Player::audioLoop, this);
    decodeThread_ = std::thread(&Player::decodeLoop, this);
    return true;
}

void Player::close() {
    stop_.store(true);
    videoQueue_.close();
    if (audio_) audio_->closeQueue();
    if (audioThread_.joinable()) audioThread_.join();
    if (decodeThread_.joinable()) decodeThread_.join();

    subtitles_.clear();
    subtitleLoaded_.store(false);
    subtitleIndex_ = -1;
    subtitleDecoder_.reset();

    audio_.reset();
    audioDecoder_.reset();
    videoDecoder_.reset();
    audioDemuxer_.reset();
    videoDemuxer_.reset();
    lastFrame_.reset();
    videoPtsIdx_ = -1;
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
    }

    hasMedia_.store(false);
    videoEnabled_.store(false);
    audioEnabled_.store(false);
    hwDecode_.store(false);
    state_.store(State::Idle);
    paused_.store(false);
    playing_ = false;
    videoClockStarted_ = false;
    duration_ = 0.0;
    path_.clear();
    error_.clear();
}

void Player::togglePause() {
    State s = state_.load();
    if (s == State::Ended) {
        reopenFromStart();
        return;
    }
    if (s == State::Playing) {
        setPaused(true);
        state_.store(State::Paused);
    } else if (s == State::Paused) {
        setPaused(false);
        state_.store(State::Playing);
    }
}

void Player::setPaused(bool p) {
    paused_.store(p);
    playing_ = !p;
    if (audioEnabled_.load()) {
        if (p)
            audio_->pauseDevice();
        else
            audio_->resumeDevice();
    }
    videoBaseTicks_ = SDL_GetPerformanceCounter();
}

void Player::reopenFromStart() {
    if (path_.empty()) return;
    std::string p = path_;
    close();
    openFile(p);
}

void Player::seek(double seconds) {
    if (!hasMedia_.load()) return;
    if (videoQueue_.closed()) {
        reopenFromStart();
    }
    requestSeek(seconds);
}

void Player::seekRelative(double delta) {
    seek(clock() + delta);
}

void Player::requestSeek(double t) {
    std::lock_guard<std::mutex> lock(seekMutex_);
    seekPending_ = true;
    seekTarget_ = std::clamp(t, 0.0, duration_ > 0.0 ? duration_ : t);
    lastSeekTime_ = SDL_GetTicks();
    // UI 冻结：进度条不回退
    uiSeeking_.store(true, std::memory_order_relaxed);
    uiTargetPts_.store(seekTarget_, std::memory_order_relaxed);
}

bool Player::seekRequested() {
    std::lock_guard<std::mutex> lock(seekMutex_);
    return seekPending_;
}

void Player::setVolume(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    volume_.store(v);
    if (v > 0.0f) muted_.store(false);
    if (audio_) audio_->setVolume(v);
}

void Player::setSpeed(float s) {
    s = std::clamp(s, 0.05f, 3.0f);
    speed_.store(s);
    if (audio_) {
        audio_->setSpeed(s);
        double anchor = audio_->anchorPts_.load(std::memory_order_relaxed);
        if (anchor > 0.0) {
            dropUntil_.store(anchor);
            uiSeeking_.store(true, std::memory_order_relaxed);
            uiTargetPts_.store(anchor, std::memory_order_relaxed);
        }
    }
    videoBaseTicks_ = SDL_GetPerformanceCounter();
}

bool Player::loadExternalSubtitle(const std::string& path) {
    SubtitleTrack track;
    bool ok = false;
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".ass") == 0)
        ok = track.loadAss(path);
    else
        ok = track.loadSrt(path);
    if (!ok) return false;
    subtitles_ = std::move(track);
    subtitleLoaded_.store(true);
    return true;
}

std::string Player::subtitleText(double t) const {
    if (!subtitleLoaded_.load() && !subtitleDecoder_) return {};
    return subtitles_.textAt(t);
}

void Player::toggleMute() {
    muted_.store(!muted_.load());
    if (audio_) audio_->setVolume(muted_.load() ? 0.0f : volume_.load());
}

double Player::framePts(const FramePtr& f) const {
    if (!f) return 0.0;
    int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    if (ts == AV_NOPTS_VALUE || ts < 0) return 0.0;
    return (double)ts * videoPtsScale_.load();
}

double Player::videoClock() const {
    if (!videoClockStarted_) return 0.0;
    if (!playing_) return videoBasePts_;
    double elapsed = (double)(SDL_GetPerformanceCounter() - videoBaseTicks_) /
                     (double)SDL_GetPerformanceFrequency();
    return videoBasePts_ + elapsed * speed_.load();
}

double Player::clock() const {
    if (audioEnabled_.load()) {
        double ac = audio_->clock();
        if (ac >= 0.0) return ac;
    }
    return videoClock();
}

double Player::uiClock() const {
    // seek / 切倍速期间冻结，用 targetPts；否则取 max(时钟, target) 防回退
    if (uiSeeking_.load(std::memory_order_relaxed)) {
        return uiTargetPts_.load(std::memory_order_relaxed);
    }
    double c = clock();
    double t = uiTargetPts_.load(std::memory_order_relaxed);
    return (c > t) ? c : t;
}

FramePtr Player::pullFrame() {
    if (!videoEnabled_.load()) return nullptr;
    if (paused_.load()) return lastFrame_;

    double c = clock();
    float spd = speed_.load();
    double target = c;  // 音频时钟已含变速；无音频时 videoClock 也含变速
    FramePtr f;

    if (!videoQueue_.peek(f)) {
        if (videoQueue_.closed()) {
            state_.store(State::Ended);
            return nullptr;
        }
        SDL_Delay(1);  // M17: 轮询延迟从 8ms 优化到 1ms
        return lastFrame_;
    }
    if (!f) {
        videoQueue_.pop(f);
        state_.store(State::Ended);
        return nullptr;
    }

    double pts = framePts(f);
    if (audioWait_.load()) {
        // M17: seek 期间逐帧显示，用户看到画面从关键帧推进到目标
        videoQueue_.pop(f);
        lastFrame_ = f;
        // 到达目标帧附近时才恢复音频
        double seekTarget = audioSeekTarget_.load();
        if (pts >= seekTarget - 0.1 || pts < videoBasePts_ - 1.0) {
            // PTS 已超过目标，或 PTS 发生跳变（新 seek）→ 校准时钟并恢复音频
            videoBasePts_ = pts;
            videoBaseTicks_ = SDL_GetPerformanceCounter();
            playing_ = !paused_.load();
            if (audioWait_.exchange(false) && audio_ && !paused_.load())
                audio_->resumeDevice();
            if (onSeekingChanged) onSeekingChanged(false);  // M18: 通知 seeking 完成
        }
        return f;
    }
    if (pts - target > 0.05) {
        double remain = (pts - target) / spd;
        int delay = std::min((int)(remain * 1000.0), 50);
        SDL_Delay(delay);
        return lastFrame_;
    }

    videoQueue_.pop(f);
    lastFrame_ = f;
    if (!videoClockStarted_) {
        videoClockStarted_ = true;
        videoBasePts_ = pts;
    }
    videoBasePts_ = pts;
    videoBaseTicks_ = SDL_GetPerformanceCounter();
    playing_ = !paused_.load();

    // seek: audio was paused + clock set to target; resume once video catches up
    if (audioWait_.exchange(false) && audio_ && !paused_.load())
        audio_->resumeDevice();

    while (videoQueue_.peek(f) && f) {
        if (framePts(f) <= c - 0.05)
            videoQueue_.pop(f);
        else
            break;
    }
    return f;
}

void Player::doSeek(double t) {
    if (!videoDemuxer_) return;
    audioSeeking_.store(true);
    if (onSeekingChanged) onSeekingChanged(true);  // M18: 通知 seeking 开始
    if (audio_) {
        audio_->clearQueue();
        audio_->pauseDevice();
        audio_->setClock(t);
    }
    videoQueue_.clear();
    videoClockStarted_ = true;
    videoBasePts_ = t;
    videoBaseTicks_ = SDL_GetPerformanceCounter();
    audioWait_ = true;
    seekFirstFrame_.store(true);
    audioSeekPending_.store(true);
    audioSeekTarget_.store(t);
    videoDemuxer_->seek(t);
    if (videoDecoder_) videoDecoder_->flushBuffers();
    if (subtitleDecoder_) {
        subtitles_.clear();
        subtitleDecoder_->flushBuffers();
    }
    dropUntil_.store(-1e9);
}

void Player::decodeLoop() {
    while (!stop_.load()) {
        {
            std::lock_guard<std::mutex> lock(seekMutex_);
            if (seekPending_) {
                // M17: 150ms debounce — 拖动进度条时等松手再 seek
                if (SDL_GetTicks() - lastSeekTime_ < 150) {
                    // 还在快速 seek 中，跳过本轮
                } else {
                    seekPending_ = false;
                    double t = seekTarget_;
                    doSeek(t);
                    continue;
                }
            }
        }

        PacketPtr pkt = videoDemuxer_->readPacket();
        if (!pkt) break;

        if (pkt->stream_index == videoPtsIdx_) {
            if (!videoDecoder_ || !videoDecoder_->send(pkt.get())) continue;
            while (FramePtr f = videoDecoder_->receive()) {
                if (seekRequested() || stop_.load()) break;
                while (!videoQueue_.tryPush(f)) {
                    if (seekRequested() || stop_.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (seekRequested() || stop_.load()) break;
            }
        } else if (subtitleIndex_ >= 0 && pkt->stream_index == subtitleIndex_ &&
                   !subtitleLoaded_.load()) {
            if (subtitleDecoder_) {
                AVStream* s = videoDemuxer_->subtitleStream();
                double tb = s ? av_q2d(s->time_base) : 0.001;
                subtitleDecoder_->decode(pkt.get(), tb, subtitles_);
            }
        }
    }

    if (stop_.load()) return;

    if (videoDecoder_) {
        videoDecoder_->flush();
        while (FramePtr f = videoDecoder_->receive()) {
            if (framePts(f) < dropUntil_.load()) continue;
            if (!videoQueue_.push(f)) return;
        }
    }
    videoQueue_.close();
}

void Player::audioLoop() {
    while (!stop_.load()) {
        if (!audioDemuxer_ || !audioEnabled_.load()) break;
        if (audioSeekPending_.exchange(false)) {
            audioDemuxer_->seek(audioSeekTarget_.load());
            if (audioDecoder_) audioDecoder_->flushBuffers();
            audioSeeking_.store(false);  // M18: seek 完成，允许新帧推入
        }
        PacketPtr pkt = audioDemuxer_->readPacket();
        if (!pkt) break;
        if (pkt->stream_index != audioDemuxer_->audioIndex()) continue;
        if (!audioDecoder_ || !audioDecoder_->send(pkt.get())) continue;
        while (FramePtr f = audioDecoder_->receive()) {
            if (seekRequested() || stop_.load()) break;
            if (audioSeeking_.load()) continue;  // M18: 跳过旧帧
            if (framePts(f) < dropUntil_.load()) continue;
            while (!audio_ || !audio_->tryPush(f)) {
                if (seekRequested() || stop_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (seekRequested() || stop_.load()) break;
        }
    }
    if (stop_.load()) return;
    if (audioDecoder_) {
        audioDecoder_->flush();
        while (FramePtr f = audioDecoder_->receive()) {
            if (framePts(f) < dropUntil_.load()) continue;
            if (!audio_ || !audio_->push(f)) break;
        }
    }
    if (audio_) audio_->closeQueue();
}