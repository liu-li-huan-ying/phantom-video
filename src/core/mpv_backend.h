#pragma once
#include <atomic>
#include <cstdint>
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

    bool init(HWND hwnd = nullptr, bool enableZeroCopy = false);
    void shutdown();

    // 销毁并重建 mpv 上下文（用于 audio-exclusive 等需要重置 AO 的场景）
    // 返回之前播放的文件路径和位置，调用方负责恢复播放状态
    struct ReinitSnapshot {
        std::string path;
        double pos = -1.0;
        bool wasPaused = false;
        float volume = 0.8f;
        float speed = 1.0f;
        bool eqEnabled = false;
        float eqGains[6] = {};
        bool audioExclusive = false;
    };
    ReinitSnapshot reinit(HWND hwnd, bool enableZeroCopy);

    bool loadFile(const std::string& path);
    void close();

    void togglePause();
    void unpause();               // P4-1: 确保恢复播放
    void seek(double seconds);
    void seekRelative(double delta);
    void setVolume(float v);
    float volume() const { return volume_.load(std::memory_order_relaxed); }
    void toggleMute();
    bool muted() const { return muted_.load(); }
    void setSpeed(float s);
    float speed() const { return speed_.load(std::memory_order_relaxed); }

    // ---- 通用轨道信息 ----
    struct TrackInfo { int id; std::string desc; };

    // ---- 字幕 ----
    bool subVisible() const;
    void setSubVisibility(bool vis);
    std::string currentSubTrack() const;   // 当前字幕轨描述(无则空)
    std::vector<TrackInfo> subTracks() const; // 所有字幕轨
    int  currentSubId() const;             // 当前字幕轨 ID(无则-1)
    void setSubtitle(int id);              // 切换字幕轨
    void loadSubtitle(const std::string& path); // 加载外部字幕文件
    void addSubDelay(double delta);        // 秒
    double subDelay() const;

    // ---- 字幕样式 ----
    void setSubFontSize(int size);         // 字号(0-7)
    int  subFontSize() const;
    void setSubColor(int r, int g, int b); // 字幕颜色
    void setSubPos(int pos);               // 垂直位置(0-100, 底部=100)

    // ---- 音轨 ----
    std::vector<TrackInfo> audioTracks() const;
    int  currentAudioTrack() const;
    void setAudioTrack(int id);

    // ---- 章节 ----
    struct ChapterInfo { int index; double time; std::string title; };
    std::vector<ChapterInfo> chapters() const;
    int  currentChapter() const;
    void seekToChapter(int idx);

    // ---- AB 循环 ----
    void setLoopA();                       // 设置 A 点(当前时间), 返回 A 点时间
    void setLoopB();                       // 设置 B 点, 开始循环
    void clearLoop();                      // 清除循环
    double loopA() const { return loopA_; }
    double loopB() const { return loopB_; }
    bool   looping() const { return loopB_ > 0; }

    // ---- 去色带强度 ----
    void setDebandLevel(int level);        // 0=关 1=轻 2=中 3=强
    int  debandLevel() const;

    // ---- 画面比例 ----
    void cycleAspectRatio();                // 循环: auto → 16:9 → 4:3 → 1:1 → auto
    int  aspectRatioIndex() const { return aspectIdx_; }
    static constexpr const char* ASPECT_NAMES[] = {"auto", "16:9", "4:3", "1:1"};
    static constexpr int ASPECT_COUNT = 4;

    // ---- 音频输出模式 ----
    void setAudioOutput(int mode) { audioOutput_ = mode; }
    int  audioOutput() const { return audioOutput_; }
    void setAudioExclusive(bool on) { audioExclusive_ = on; }
    bool audioExclusive() const { return audioExclusive_; }

    // ---- 音频均衡器 ----
    struct EQBand { const char* freq; float gain; };  // gain: -12..+12 dB
    void setEQBand(int band, float gain);  // band 0-5
    void setEQEnabled(bool on);
    bool eqEnabled() const { return eqEnabled_; }
    float eqGain(int band) const;

    // ---- 画面调节 (-100 ~ 100) ----
    void setBrightness(int v);
    void setContrast(int v);
    void setSaturation(int v);
    void setGamma(int v);
    void setDeinterlace(bool on);

    // ---- 色彩空间映射 ----
    void setToneMapping(int mode);   // 0=auto 1=clip 2=bt.2390 3=bt.2446a 4=st2094-10
    void setGamutMapping(int mode);  // 0=auto 1=perceptual 2=clip 3=relative-colorimetric
    void setHdrPeakDetect(bool on);

    // ---- VapourSynth 滤镜（插帧 / 超分） ----
    void applyVapourSynthFilter(int interp, int superRes);  // interp=0/1, superRes=0/1
    void clearVapourSynthFilter();

    State state() const { return state_.load(); }
    double clock() const;
    double duration() const;
    bool hasMedia() const { return hasMedia_.load(); }
    std::string path() const { return path_; }
    bool hwDecodeActive() const { return hwDecode_.load(); }
    const char* hwdecCurrent() const { return hwdecCurrent_.c_str(); }
    int  hwdecRetryCount() const { return hwdecRetryCount_; }
    bool seekbarFrozen() const;

    int videoWidth() const { return videoWidth_.load(); }
    int videoHeight() const { return videoHeight_.load(); }
    double bufferFill() const;
    bool isBuffering() const { return isBuffering_.load(); }
    double bufferingLevel() const { return bufferingLevel_.load(); }

    std::string title() const;

    mpv_handle* mpv() const { return mpv_; }

    std::function<void()> onFileLoaded;
    std::function<void()> onPlaybackEnded;
    std::function<void(const char* errorMsg)> onPlaybackError;  // 网络/解码错误回调

private:
    void eventLoop();
    void handlePropertyChange(const char* name, mpv_event_property* prop);
    void retryWithHwdecFallback();
    const char* nextHwdecLevel();

    mpv_handle* mpv_ = nullptr;
    std::thread eventThread_;
    std::atomic<bool> running_{ false };

    std::atomic<State> state_{ State::Idle };
    std::atomic<bool> hasMedia_{ false };
    std::atomic<bool> muted_{ false };
    std::atomic<float> volume_{ 0.8f };
    std::atomic<float> speed_{ 1.0f };
    std::atomic<bool> hwDecode_{ false };
    std::string hwdecCurrent_;          // mpv 报告的实际 hwdec 值
    int hwdecRetryCount_ = 0;           // 当前文件重试计数(上限 2)
    std::string hwdecRetryPath_;        // 正在重试的文件路径(换文件重置)
    std::atomic<int> videoWidth_{ 0 };
    std::atomic<int> videoHeight_{ 0 };

    std::string path_;
    mutable std::mutex propMutex_;
    double cachedDuration_ = 0.0;
    double cachedClock_ = 0.0;
    double cachedBufferFill_ = 0.0;
    std::atomic<double> bufferingLevel_{ 0.0 };  // demuxer-cache-state: 0.0~1.0
    std::atomic<bool> isBuffering_{ false };      // paused-for-cache
    std::atomic<bool> eofFired_{ false };   // eof-reached 去重(事件线程读/loadFile 写)
    uint32_t seekbarFreezeEnd_ = 0;         // 速度切换后冻结进度条显示(ms)
    double loopA_ = -1.0;                   // AB 循环 A 点
    double loopB_ = -1.0;                   // AB 循环 B 点
    int    debandLevel_ = 2;                // 去色带等级 0-3
    int    aspectIdx_ = 0;                  // 画面比例索引 0=auto 1=16:9 2=4:3 3=1:1
    int    audioOutput_ = 0;                // 音频输出模式 0=立体声 1=5.1 2=7.1 3=直通
    bool   audioExclusive_ = false;         // WASAPI 独占模式
    bool   eqEnabled_ = false;              // 均衡器开关
    float  eqGains_[6] = {};                // 均衡器 6 频段增益
};
