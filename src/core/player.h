#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "audio/audio_output.h"
#include "core/blocking_queue.h"
#include "core/decoder.h"
#include "core/demuxer.h"
#include "subtitle/subtitle.h"

#include <functional>

class Player {
public:
    enum class State { Idle, Playing, Paused, Ended };

    Player() = default;
    ~Player();

    // M18: seeking 状态回调
    std::function<void(bool seeking)> onSeekingChanged;

    bool openFile(const std::string& path);
    void close();
    void togglePause();
    void seek(double seconds);
    void seekRelative(double delta);
    void setVolume(float v);
    float volume() const { return volume_.load(std::memory_order_relaxed); }
    void toggleMute();
    bool muted() const { return muted_.load(); }
    void setSpeed(float s);
    float speed() const { return speed_.load(std::memory_order_relaxed); }

    State state() const { return state_.load(); }
    double clock() const;
    // UI 进度条安全读取：seek/切倍速期间冻结，永不回退
    double uiClock() const;
    bool uiSeeking() const { return uiSeeking_.load(std::memory_order_relaxed); }
    double uiTargetPts() const { return uiTargetPts_.load(std::memory_order_relaxed); }
    void clearUiSeeking() { uiSeeking_.store(false, std::memory_order_relaxed); }
    double duration() const { return duration_; }
    bool hasMedia() const { return hasMedia_.load(); }
    std::string path() const { return path_; }
    std::string error() const { return error_; }
    bool usingHardware() const { return hwDecode_.load(); }
    bool loadExternalSubtitle(const std::string& path);
    std::string subtitleText(double t) const;
    bool hasSubtitle() const { return subtitleLoaded_ || subtitleIndex_ >= 0; }
    AudioOutput& audio() { return *audio_; }

    FramePtr pullFrame();

private:
    void decodeLoop();
    void audioLoop();
    void doSeek(double t);
    void requestSeek(double t);
    bool seekRequested();
    void setPaused(bool p);
    void reopenFromStart();
    double videoClock() const;
    double framePts(const FramePtr& f) const;

    std::unique_ptr<Demuxer> videoDemuxer_;
    std::unique_ptr<Demuxer> audioDemuxer_;
    std::unique_ptr<Decoder> videoDecoder_;
    std::unique_ptr<Decoder> audioDecoder_;
    std::unique_ptr<AudioOutput> audio_;
    BlockingQueue<FramePtr> videoQueue_{ 60 };

    std::thread decodeThread_;
    std::thread audioThread_;
    std::atomic<bool> audioSeekPending_{ false };
    std::atomic<double> audioSeekTarget_{ 0.0 };
    std::atomic<bool> stop_{ false };
    std::atomic<bool> videoEnabled_{ false };
    std::atomic<bool> audioEnabled_{ false };
    std::atomic<State> state_{ State::Idle };
    std::atomic<bool> paused_{ false };
    std::atomic<bool> hasMedia_{ false };
    std::atomic<double> dropUntil_{ -1e9 };
    std::atomic<double> videoPtsScale_{ 1.0 };
    std::atomic<float> volume_{ 0.8f };
    std::atomic<bool> muted_{ false };
    std::atomic<float> speed_{ 1.0f };
    std::atomic<bool> audioWait_{ false };
    std::atomic<bool> audioSeeking_{ false };  // M18: seek 期间跳过旧帧
    double lastAudioPts_{ -1e9 };              // M18: 检测音频 PTS 跳变
    std::atomic<bool> seekFirstFrame_{ false };
    Uint32 lastSeekTime_ = 0;                    // M17: seek 合并 debounce

    // UI 进度条冻结（seek / 切倍速期间不回退）
    std::atomic<bool> uiSeeking_{ false };
    std::atomic<double> uiTargetPts_{ 0.0 };

    std::mutex seekMutex_;
    bool seekPending_ = false;
    double seekTarget_ = 0.0;

    AVBufferRef* hwDeviceCtx_ = nullptr;
    std::atomic<bool> hwDecode_{ false };

    SubtitleTrack subtitles_;
    std::atomic<bool> subtitleLoaded_{ false };
    int subtitleIndex_ = -1;
    std::unique_ptr<SubtitleDecoder> subtitleDecoder_;

    double duration_ = 0.0;
    std::string path_;
    std::string error_;
    int videoPtsIdx_ = -1;

    FramePtr lastFrame_;
    std::atomic<double> videoBasePts_{ 0.0 };
    std::atomic<Uint64> videoBaseTicks_{ 0 };
    bool videoClockStarted_ = false;
    bool playing_ = false;
};