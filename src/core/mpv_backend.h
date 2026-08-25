#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <mpv/client.h>

class MpvBackend {
public:
    enum class State { Idle, Playing, Paused, Ended };

    MpvBackend() = default;
    ~MpvBackend();

    bool init(HWND hwnd = nullptr);
    void shutdown();

    bool loadFile(const std::string& path);
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

    // ---- 字幕 ----
    bool subVisible() const;
    void setSubVisibility(bool vis);
    std::string currentSubTrack() const;   // 当前字幕轨描述(无则空)
    void addSubDelay(double delta);        // 秒
    double subDelay() const;

    State state() const { return state_.load(); }
    double clock() const;
    double duration() const;
    bool hasMedia() const { return hasMedia_.load(); }
    std::string path() const { return path_; }
    bool hwDecodeActive() const { return hwDecode_.load(); }

    int videoWidth() const { return videoWidth_.load(); }
    int videoHeight() const { return videoHeight_.load(); }
    double bufferFill() const;

    std::string title() const;

    mpv_handle* mpv() const { return mpv_; }

    std::function<void()> onFileLoaded;
    std::function<void()> onPlaybackEnded;

private:
    void eventLoop();
    void handlePropertyChange(const char* name, mpv_event_property* prop);

    mpv_handle* mpv_ = nullptr;
    std::thread eventThread_;
    std::atomic<bool> running_{ false };

    std::atomic<State> state_{ State::Idle };
    std::atomic<bool> hasMedia_{ false };
    std::atomic<bool> muted_{ false };
    std::atomic<float> volume_{ 0.8f };
    std::atomic<float> speed_{ 1.0f };
    std::atomic<bool> hwDecode_{ false };
    std::atomic<int> videoWidth_{ 0 };
    std::atomic<int> videoHeight_{ 0 };

    std::string path_;
    mutable std::mutex propMutex_;
    double cachedDuration_ = 0.0;
    double cachedClock_ = 0.0;
    double cachedBufferFill_ = 0.0;
    std::atomic<bool> eofFired_{ false };   // eof-reached 去重(事件线程读/loadFile 写)
};
