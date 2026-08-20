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

class Player {
public:
    enum class State { Idle, Playing, Paused, Ended };

    Player() = default;
    ~Player();

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
    double duration() const { return duration_; }
    bool hasMedia() const { return hasMedia_.load(); }
    std::string path() const { return path_; }
    std::string error() const { return error_; }
    bool usingHardware() const { return hwDecode_.load(); }
    bool loadExternalSubtitle(const std::string& path);
    std::string subtitleText(double t) const;
    bool hasSubtitle() const { return subtitleLoaded_ || subtitleIndex_ >= 0; }

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
    double videoBasePts_ = 0.0;
    Uint64 videoBaseTicks_ = 0;
    bool videoClockStarted_ = false;
    bool playing_ = false;
};