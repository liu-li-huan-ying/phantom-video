#include "core/mpv_backend.h"
#include "core/logger.h"
#include "core/config.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <SDL.h>

MpvBackend::~MpvBackend() {
    shutdown();
}

bool MpvBackend::init(HWND hwnd, bool enableZeroCopy) {
    auto t0 = SDL_GetTicks();
    mpv_ = mpv_create();
    if (!mpv_) {
        LOG_ERROR("MPV", "mpv_create failed");
        return false;
    }

    mpv_set_option_string(mpv_, "no-terminal", "yes");
    mpv_set_option_string(mpv_, "no-osd-bar", "yes");
    mpv_set_option_string(mpv_, "ao", "wasapi");
    mpv_set_option_string(mpv_, "vo", "gpu-next");
    mpv_set_option_string(mpv_, "gpu-context", "d3d11");
    // 四级 hwdec 降级链首: auto-copy-safe(最稳基线, 禁用 zero-copy)
    // enableZeroCopy=true 时直接用 auto-safe(允许 zero-copy)
    mpv_set_option_string(mpv_, "hwdec", enableZeroCopy ? "auto-safe" : "auto-copy-safe");
    mpv_set_option_string(mpv_, "sub-auto", "fuzzy");
    mpv_set_option_string(mpv_, "volume", "80");
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "no-input-default-bindings", "yes");
    mpv_set_option_string(mpv_, "input-vo-keyboard", "no");
    mpv_set_option_string(mpv_, "cursor-autohide", "0");

    // ---- 音频 ----
    // 声道: mpv 默认 auto-safe，不在 init 里设 audio-channels（运行时变更需 ao-reload）
    // 采样率: 0=使用源采样率
    mpv_set_option_string(mpv_, "audio-samplerate", "0");
    // 音频同步: 用音频时钟作为主时钟
    mpv_set_option_string(mpv_, "video-sync", "audio");
    // 音高校正: 变速时保持音高
    mpv_set_option_string(mpv_, "audio-pitch-correction", "yes");

    // 网络流缓存优化
    mpv_set_option_string(mpv_, "demuxer-max-bytes", "80MiB");
    mpv_set_option_string(mpv_, "demuxer-max-back-bytes", "20MiB");
    mpv_set_option_string(mpv_, "cache-secs", "30");

    // 截图输出到 exe/screenshots/
    {
        std::string dir = exeDir() + "screenshots";
        CreateDirectoryA(dir.c_str(), nullptr);
        mpv_set_option_string(mpv_, "screenshot-format", "png");
        mpv_set_option_string(mpv_, "screenshot-directory", dir.c_str());
    }

    // ---- 画质链路 ----
    // 上采样 spline36(锐利少振铃) / 下采样 mitchell(mpv 官方推荐) /
    // 色度上采样 spline36(4:2:0 源色度修复) / deband 去 8bit 色带
    mpv_set_option_string(mpv_, "scale",  "spline36");
    mpv_set_option_string(mpv_, "dscale", "mitchell");
    mpv_set_option_string(mpv_, "cscale", "spline36");
    mpv_set_option_string(mpv_, "deband", "yes");
    mpv_set_option_string(mpv_, "scale-antiring", "0.7");   // 缩放振铃抑制(对 ewa 系同样有效)
    // HDR 片源在 HDR 显示器上的色彩空间提示(gpu-next, SDR 屏自动忽略)
    mpv_set_option_string(mpv_, "target-colorspace-hint", "yes");
    // 抖动防梯度断裂(默认 auto, 显式声明意图)
    mpv_set_option_string(mpv_, "dither-depth", "auto");

    // wid 必须在 mpv_initialize 之前设置
    if (hwnd) {
        char widStr[64];
        std::snprintf(widStr, sizeof(widStr), "%lld", (long long)(intptr_t)hwnd);
        mpv_set_option_string(mpv_, "wid", widStr);
    }

    // WASAPI 独占模式（必须在 mpv_initialize 前设置）
    if (audioExclusive_) {
        mpv_set_option_string(mpv_, "audio-exclusive", "yes");
    }

    if (mpv_initialize(mpv_) < 0) {
        LOG_ERROR("MPV", "mpv_initialize failed");
        mpv_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }
    LOG_INFO("MPV", "mpv_initialize took %u ms", SDL_GetTicks() - t0);

    // 初始化后设置音频声道（仅一次，运行时通过 reinit 切换）
    {
        const char* chModes[] = {"auto-safe", "5.1", "7.1", "auto"};
        int mode = audioOutput_;
        if (mode >= 0 && mode < 4) {
            mpv_set_property_string(mpv_, "audio-channels", chModes[mode]);
            LOG_INFO("MPV", "audio-channels -> %s", chModes[mode]);
        }
    }

    // 回读画质关键项确认生效
    {
        const char* props[] = { "scale", "dscale", "cscale", "deband",
                                "target-colorspace-hint" };
        for (auto* p : props) {
            char* v = mpv_get_property_string(mpv_, p);
            LOG_INFO("MPV", "quality %s=%s", p, v ? v : "?");
            mpv_free(v);
        }
    }
    LOG_INFO("MPV", "post-init properties took %u ms", SDL_GetTicks() - t0);

    // 桥接 mpv 内部日志(warn+) 到统一 Logger —— 否则解码/加载失败静默
    mpv_request_log_messages(mpv_, "warn");
    mpv_set_property_string(mpv_, "terminal", "no");

    mpv_observe_property(mpv_, 1, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 2, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 3, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 4, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 5, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 6, "hwdec-current", MPV_FORMAT_STRING);
    mpv_observe_property(mpv_, 7, "width", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, 8, "height", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, 9, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 10, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 12, "demuxer-cache-state", MPV_FORMAT_NODE);
    // keep-open=yes 时播完不发 END_FILE, 必须观察 eof-reached 才能触发连播
    mpv_observe_property(mpv_, 11, "eof-reached", MPV_FORMAT_FLAG);

    running_.store(true);
    hasMedia_.store(false);
    state_.store(State::Idle);
    path_.clear();
    eventThread_ = std::thread(&MpvBackend::eventLoop, this);

    LOG_INFO("MPV", "mpv backend initialized, hwdec=%s zero_copy=%d",
             enableZeroCopy ? "auto-safe" : "auto-copy-safe", enableZeroCopy);
    return true;
}

void MpvBackend::shutdown() {
    if (!mpv_) return;

    running_.store(false);
    mpv_wakeup(mpv_);
    if (eventThread_.joinable()) eventThread_.join();

    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    LOG_INFO("MPV", "mpv backend shutdown");
}

MpvBackend::ReinitSnapshot MpvBackend::reinit(HWND hwnd, bool enableZeroCopy) {
    ReinitSnapshot snap;

    // 1. 快照当前状态
    snap.path = path_;
    if (hasMedia_ && mpv_) {
        // 读取当前播放位置
        double pos = clock();
        if (pos > 0) snap.pos = pos;
        snap.wasPaused = (state_ == State::Paused);
        snap.volume = volume_.load();
        snap.speed = speed_.load();
        snap.eqEnabled = eqEnabled_;
        snap.audioExclusive = audioExclusive_;
        std::memcpy(snap.eqGains, eqGains_, sizeof(eqGains_));
    }

    LOG_INFO("MPV", "reinit: snapshot path=%s pos=%.1f paused=%d",
             snap.path.c_str(), snap.pos, snap.wasPaused);

    // 2. 完全销毁 mpv（释放 WASAPI exclusive 设备）
    shutdown();

    // 3. 重建
    if (!init(hwnd, enableZeroCopy)) {
        LOG_ERROR("MPV", "reinit: init failed");
        return snap;
    }

    // 4. 恢复状态
    setVolume(snap.volume);
    if (std::abs(snap.speed - 1.0f) > 0.01f)
        setSpeed(snap.speed);
    if (snap.eqEnabled) {
        eqEnabled_ = true;
        for (int i = 0; i < 6; ++i)
            setEQBand(i, snap.eqGains[i]);
    }

    LOG_INFO("MPV", "reinit: complete, restored vol=%.0f spd=%.2f",
             snap.volume * 100, snap.speed);
    return snap;
}

bool MpvBackend::loadFile(const std::string& path) {
    if (!mpv_) return false;

    close();

    // P4-1: 先暂停，等 FILE_LOADED 后 seek + unpause
    // 避免 audio 先于 video 开始播放导致启动冻结
    int paused = 1;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused);

    const char* cmd[] = { "loadfile", path.c_str(), NULL };
    int ret = mpv_command(mpv_, cmd);
    if (ret < 0) {
        LOG_ERROR("MPV", "loadfile failed: %s (path=%s)", mpv_error_string(ret), path.c_str());
        return false;
    }

    path_ = path;
    hasMedia_.store(true);
    state_.store(State::Playing);
    eofFired_.store(false);
    // 换文件重置 hwdec 重试计数
    if (path != hwdecRetryPath_) {
        hwdecRetryCount_ = 0;
        hwdecRetryPath_ = path;
    }
    LOG_INFO("MPV", "loaded: %s (paused)", path.c_str());
    return true;
}

void MpvBackend::close() {
    if (!mpv_ || !hasMedia_.load()) return;

    // 用 async stop 避免 mpv 忙时阻塞 UI 线程
    const char* cmd[] = { "stop", NULL };
    mpv_command_async(mpv_, 0, cmd);

    hasMedia_.store(false);
    state_.store(State::Idle);
    path_.clear();
}

void MpvBackend::togglePause() {
    if (!mpv_ || !hasMedia_.load()) return;

    int paused = (state_.load() == State::Paused) ? 0 : 1;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused);
}

void MpvBackend::unpause() {
    if (!mpv_ || !hasMedia_.load()) return;
    int unpaused = 0;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &unpaused);
    state_.store(State::Playing);
}

void MpvBackend::seek(double seconds) {
    if (!mpv_) return;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", seconds);
    const char* cmd[] = { "seek", buf, "absolute", "exact", NULL };
    mpv_command(mpv_, cmd);
}

void MpvBackend::seekRelative(double delta) {
    if (!mpv_) return;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", delta);
    const char* cmd[] = { "seek", buf, "relative", NULL };
    mpv_command(mpv_, cmd);
}

void MpvBackend::setVolume(float v) {
    if (!mpv_) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    volume_.store(v);
    double vol = v * 100.0;
    mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void MpvBackend::toggleMute() {
    if (!mpv_) return;

    bool m = !muted_.load();
    muted_.store(m);
    int flag = m ? 1 : 0;
    mpv_set_property(mpv_, "mute", MPV_FORMAT_FLAG, &flag);
}

void MpvBackend::setSpeed(float s) {
    if (!mpv_) return;
    if (s < 0.05f) s = 0.05f;
    if (s > 8.0f) s = 8.0f;

    // 速度切换前冻结进度条, 防止 time-pos 跳变导致进度条抖动
    seekbarFreezeEnd_ = SDL_GetTicks() + 500;

    double sd = s;
    int r = mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &sd);
    LOG_INFO("MPV", "setSpeed %.2f ret=%d (seekbar frozen 500ms)", s, r);
    speed_.store(s);
}

// ---- 字幕 ----
bool MpvBackend::subVisible() const {
    if (!mpv_) return true;
    int flag = 1;
    if (mpv_get_property(mpv_, "sub-visibility", MPV_FORMAT_FLAG, &flag) < 0)
        return true;
    return flag != 0;
}

void MpvBackend::setSubVisibility(bool vis) {
    if (!mpv_) return;
    int flag = vis ? 1 : 0;
    mpv_set_property(mpv_, "sub-visibility", MPV_FORMAT_FLAG, &flag);
}

std::string MpvBackend::currentSubTrack() const {
    if (!mpv_) return {};
    // current-tracks/sub 返回形如 "(sub)-id:1 lang:eng title:..." 的描述
    char* s = mpv_get_property_string(mpv_, "current-tracks/sub");
    if (!s) return {};
    std::string result(s);
    mpv_free(s);

    // 提取 title/lang 拼成可读名
    std::string name;
    std::size_t tp = result.find("title:");
    if (tp != std::string::npos) {
        name = result.substr(tp + 6);
        std::size_t sp = name.find(' ');
        if (sp != std::string::npos) name = name.substr(0, sp);
    }
    std::size_t lp = result.find("lang:");
    if (name.empty() && lp != std::string::npos) {
        name = result.substr(lp + 5);
        std::size_t sp = name.find(' ');
        if (sp != std::string::npos) name = name.substr(0, sp);
    }
    return name;
}

void MpvBackend::addSubDelay(double delta) {
    if (!mpv_) return;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", delta);
    const char* cmd[] = { "add", "sub-delay", buf, NULL };
    mpv_command(mpv_, cmd);
}

double MpvBackend::subDelay() const {
    if (!mpv_) return 0.0;
    double v = 0.0;
    if (mpv_get_property(mpv_, "sub-delay", MPV_FORMAT_DOUBLE, &v) < 0)
        return 0.0;
    return v;
}

std::vector<MpvBackend::TrackInfo> MpvBackend::subTracks() const {
    std::vector<TrackInfo> result;
    if (!mpv_) return result;
    mpv_node list;
    if (mpv_get_property(mpv_, "track-list", MPV_FORMAT_NODE, &list) < 0) return result;
    if (list.format != MPV_FORMAT_NODE_ARRAY) { mpv_free_node_contents(&list); return result; }
    for (int i = 0; i < list.u.list->num; ++i) {
        mpv_node* node = &list.u.list->values[i];
        if (node->format != MPV_FORMAT_NODE_MAP) continue;
        const char* type = nullptr;
        int id = 0;
        const char* desc = "";
        for (int j = 0; j < node->u.list->num; ++j) {
            const char* key = node->u.list->keys[j];
            mpv_node* val = &node->u.list->values[j];
            if (std::strcmp(key, "type") == 0 && val->format == MPV_FORMAT_STRING)
                type = val->u.string;
            else if (std::strcmp(key, "id") == 0 && val->format == MPV_FORMAT_INT64)
                id = (int)val->u.int64;
            else if (std::strcmp(key, "desc") == 0 && val->format == MPV_FORMAT_STRING)
                desc = val->u.string;
        }
        if (type && std::strcmp(type, "sub") == 0)
            result.push_back({id, desc ? desc : ""});
    }
    mpv_free_node_contents(&list);
    return result;
}

int MpvBackend::currentSubId() const {
    if (!mpv_) return -1;
    int64_t v = -1;
    mpv_get_property(mpv_, "sid", MPV_FORMAT_INT64, &v);
    return (int)v;
}

void MpvBackend::setSubtitle(int id) {
    if (!mpv_) return;
    int64_t v = id;
    mpv_set_property(mpv_, "sid", MPV_FORMAT_INT64, &v);
}

void MpvBackend::loadSubtitle(const std::string& path) {
    if (!mpv_ || path.empty()) return;
    const char* cmd[] = { "sub-add", path.c_str(), "select", NULL };
    int ret = mpv_command(mpv_, cmd);
    if (ret < 0) {
        LOG_ERROR("MPV", "sub-add failed: %s (path=%s)", mpv_error_string(ret), path.c_str());
    } else {
        LOG_INFO("MPV", "sub loaded: %s", path.c_str());
    }
}

// ---- 字幕样式 ----
void MpvBackend::setSubFontSize(int size) {
    if (!mpv_) return;
    // mpv sub-font-size: 0=script, 1-7 对应不同大小
    const char* sizes[] = {"50","75","100","150","200","300","400"};
    if (size >= 0 && size < 7) {
        mpv_set_option_string(mpv_, "sub-font-size", sizes[size]);
    }
}

int MpvBackend::subFontSize() const {
    if (!mpv_) return 2;
    char* v = mpv_get_property_string(mpv_, "sub-font-size");
    if (!v) return 2;
    static const char* sizes[] = {"50","75","100","150","200","300","400"};
    int r = 2;
    for (int i = 0; i < 7; ++i) {
        if (std::strcmp(v, sizes[i]) == 0) { r = i; break; }
    }
    mpv_free(v);
    return r;
}

void MpvBackend::setSubColor(int r, int g, int b) {
    if (!mpv_) return;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    mpv_set_option_string(mpv_, "sub-color", buf);
}

void MpvBackend::setSubPos(int pos) {
    if (!mpv_) return;
    // mpv sub-pos: 0=顶, 100=底(默认100)
    if (pos < 0) pos = 0; if (pos > 100) pos = 100;
    int64_t v = pos;
    mpv_set_property(mpv_, "sub-pos", MPV_FORMAT_INT64, &v);
}

// ---- 音轨 ----
std::vector<MpvBackend::TrackInfo> MpvBackend::audioTracks() const {
    std::vector<TrackInfo> result;
    if (!mpv_) return result;
    mpv_node list;
    if (mpv_get_property(mpv_, "track-list", MPV_FORMAT_NODE, &list) < 0) return result;
    if (list.format != MPV_FORMAT_NODE_ARRAY) { mpv_free_node_contents(&list); return result; }
    for (int i = 0; i < list.u.list->num; ++i) {
        mpv_node* node = &list.u.list->values[i];
        if (node->format != MPV_FORMAT_NODE_MAP) continue;
        const char* type = nullptr;
        int id = 0;
        const char* desc = "";
        for (int j = 0; j < node->u.list->num; ++j) {
            const char* key = node->u.list->keys[j];
            mpv_node* val = &node->u.list->values[j];
            if (std::strcmp(key, "type") == 0 && val->format == MPV_FORMAT_STRING)
                type = val->u.string;
            else if (std::strcmp(key, "id") == 0 && val->format == MPV_FORMAT_INT64)
                id = (int)val->u.int64;
            else if (std::strcmp(key, "desc") == 0 && val->format == MPV_FORMAT_STRING)
                desc = val->u.string;
        }
        if (type && std::strcmp(type, "audio") == 0)
            result.push_back({id, desc ? desc : ""});
    }
    mpv_free_node_contents(&list);
    return result;
}

int MpvBackend::currentAudioTrack() const {
    if (!mpv_) return -1;
    int64_t v = -1;
    mpv_get_property(mpv_, "aid", MPV_FORMAT_INT64, &v);
    return (int)v;
}

void MpvBackend::setAudioTrack(int id) {
    if (!mpv_) return;
    int64_t v = id;
    mpv_set_property(mpv_, "aid", MPV_FORMAT_INT64, &v);
}

// ---- 章节 ----
std::vector<MpvBackend::ChapterInfo> MpvBackend::chapters() const {
    std::vector<ChapterInfo> result;
    if (!mpv_) return result;
    mpv_node list;
    if (mpv_get_property(mpv_, "chapter-list", MPV_FORMAT_NODE, &list) < 0) return result;
    if (list.format != MPV_FORMAT_NODE_ARRAY) { mpv_free_node_contents(&list); return result; }
    for (int i = 0; i < list.u.list->num; ++i) {
        mpv_node* node = &list.u.list->values[i];
        if (node->format != MPV_FORMAT_NODE_MAP) continue;
        double time = 0;
        const char* title = "";
        for (int j = 0; j < node->u.list->num; ++j) {
            const char* key = node->u.list->keys[j];
            mpv_node* val = &node->u.list->values[j];
            if (std::strcmp(key, "time") == 0 && val->format == MPV_FORMAT_DOUBLE)
                time = val->u.double_;
            else if (std::strcmp(key, "title") == 0 && val->format == MPV_FORMAT_STRING)
                title = val->u.string;
        }
        result.push_back({i, time, title ? title : ""});
    }
    mpv_free_node_contents(&list);
    return result;
}

int MpvBackend::currentChapter() const {
    if (!mpv_) return -1;
    int64_t v = -1;
    mpv_get_property(mpv_, "chapter", MPV_FORMAT_INT64, &v);
    return (int)v;
}

void MpvBackend::seekToChapter(int idx) {
    if (!mpv_) return;
    int64_t v = idx;
    mpv_set_property(mpv_, "chapter", MPV_FORMAT_INT64, &v);
}

// ---- AB 循环 ----
void MpvBackend::setLoopA() {
    loopA_ = clock();
    loopB_ = -1.0;
    LOG_INFO("MPV", "AB loop A=%.2f", loopA_);
}

void MpvBackend::setLoopB() {
    if (loopA_ < 0) return;
    loopB_ = clock();
    if (loopB_ <= loopA_) { loopA_ = loopB_ = -1.0; return; }
    LOG_INFO("MPV", "AB loop B=%.2f (range %.2f..%.2f)", loopB_, loopA_, loopB_);
}

void MpvBackend::clearLoop() {
    loopA_ = loopB_ = -1.0;
    LOG_INFO("MPV", "AB loop cleared");
}

// ---- 去色带强度 ----
// 等级: 0=关, 1=轻(iter=1,th=2,rng=16), 2=中(iter=2,th=3,rng=24), 3=强(iter=4,th=4,rng=32)
void MpvBackend::setDebandLevel(int level) {
    if (!mpv_) return;
    if (level < 0) level = 0; if (level > 3) level = 3;
    debandLevel_ = level;
    int flag = (level > 0) ? 1 : 0;
    mpv_set_property(mpv_, "deband", MPV_FORMAT_FLAG, &flag);
    if (level > 0) {
        struct { int iter, th, rng; } presets[] = {{1,2,16},{2,3,24},{4,4,32}};
        auto& p = presets[level - 1];
        int64_t v;
        v = p.iter; mpv_set_property(mpv_, "deband-iterations", MPV_FORMAT_INT64, &v);
        v = p.th;   mpv_set_property(mpv_, "deband-threshold", MPV_FORMAT_INT64, &v);
        v = p.rng;  mpv_set_property(mpv_, "deband-range", MPV_FORMAT_INT64, &v);
    }
    const char* names[] = {"Off","Light","Medium","Strong"};
    LOG_INFO("MPV", "deband -> %s (iter/th/rng)", names[level]);
}

int MpvBackend::debandLevel() const {
    return debandLevel_;
}

// ---- 画面比例 ----
void MpvBackend::cycleAspectRatio() {
    if (!mpv_) return;
    aspectIdx_ = (aspectIdx_ + 1) % ASPECT_COUNT;
    const char* ratios[] = {"auto", "16:9", "4:3", "1:1"};
    mpv_set_property_string(mpv_, "video-aspect-override", ratios[aspectIdx_]);
    LOG_INFO("MPV", "aspect ratio -> %s", ratios[aspectIdx_]);
}

// ---- 音频均衡器 ----
// 6 频段: 60Hz, 170Hz, 310Hz, 600Hz, 3kHz, 12kHz
void MpvBackend::setEQBand(int band, float gain) {
    if (!mpv_ || band < 0 || band > 5) return;
    if (gain < -12.0f) gain = -12.0f; if (gain > 12.0f) gain = 12.0f;
    eqGains_[band] = gain;
    // 重建 af=equalizer 字符串
    static const char* freqs[] = {"60","170","310","600","3000","12000"};
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "equalizer=f=%s:t=q:w=1.2:g=%.1f,equalizer=f=%s:t=q:w=1.2:g=%.1f,"
        "equalizer=f=%s:t=q:w=1.2:g=%.1f,equalizer=f=%s:t=q:w=1.2:g=%.1f,"
        "equalizer=f=%s:t=q:w=1.2:g=%.1f,equalizer=f=%s:t=q:w=1.2:g=%.1f",
        freqs[0], eqGains_[0], freqs[1], eqGains_[1],
        freqs[2], eqGains_[2], freqs[3], eqGains_[3],
        freqs[4], eqGains_[4], freqs[5], eqGains_[5]);
    mpv_set_property_string(mpv_, "af", eqEnabled_ ? buf : "");
    LOG_INFO("MPV", "EQ band %d -> %.1f dB", band, gain);
}

void MpvBackend::setEQEnabled(bool on) {
    if (!mpv_) return;
    eqEnabled_ = on;
    if (on) {
        // 重新应用所有频段
        for (int i = 0; i < 6; ++i) setEQBand(i, eqGains_[i]);
    } else {
        mpv_set_property_string(mpv_, "af", "");
    }
    LOG_INFO("MPV", "EQ -> %s", on ? "ON" : "OFF");
}

float MpvBackend::eqGain(int band) const {
    if (band < 0 || band > 5) return 0.0f;
    return eqGains_[band];
}

// ---- 画面调节 ----
void MpvBackend::setBrightness(int v) {
    if (!mpv_) return;
    double val = (double)std::clamp(v, -100, 100);
    mpv_set_property(mpv_, "brightness", MPV_FORMAT_DOUBLE, &val);
}
void MpvBackend::setContrast(int v) {
    if (!mpv_) return;
    double val = (double)std::clamp(v, -100, 100);
    mpv_set_property(mpv_, "contrast", MPV_FORMAT_DOUBLE, &val);
}
void MpvBackend::setSaturation(int v) {
    if (!mpv_) return;
    double val = (double)std::clamp(v, -100, 100);
    mpv_set_property(mpv_, "saturation", MPV_FORMAT_DOUBLE, &val);
}
void MpvBackend::setGamma(int v) {
    if (!mpv_) return;
    double val = (double)std::clamp(v, -100, 100);
    mpv_set_property(mpv_, "gamma", MPV_FORMAT_DOUBLE, &val);
}
void MpvBackend::setDeinterlace(bool on) {
    if (!mpv_) return;
    int flag = on ? 1 : 0;
    mpv_set_property(mpv_, "deinterlace", MPV_FORMAT_FLAG, &flag);
}

// ---- 色彩空间映射 ----
void MpvBackend::setToneMapping(int mode) {
    if (!mpv_) return;
    const char* modes[] = {"auto", "clip", "bt.2390", "bt.2446a", "st2094-10"};
    mpv_set_property_string(mpv_, "tone-mapping", modes[std::clamp(mode, 0, 4)]);
}
void MpvBackend::setGamutMapping(int mode) {
    if (!mpv_) return;
    const char* modes[] = {"auto", "perceptual", "clip", "relative-colorimetric"};
    mpv_set_property_string(mpv_, "gamut-mapping-mode", modes[std::clamp(mode, 0, 3)]);
}
void MpvBackend::setHdrPeakDetect(bool on) {
    if (!mpv_) return;
    int flag = on ? 1 : 0;
    mpv_set_property(mpv_, "hdr-compute-peak", MPV_FORMAT_FLAG, &flag);
}

double MpvBackend::clock() const {
    if (!mpv_) return 0.0;
    std::lock_guard<std::mutex> lock(propMutex_);
    return cachedClock_;
}

bool MpvBackend::seekbarFrozen() const {
    return seekbarFreezeEnd_ > SDL_GetTicks();
}

double MpvBackend::duration() const {
    if (!mpv_) return 0.0;
    std::lock_guard<std::mutex> lock(propMutex_);
    return cachedDuration_;
}

double MpvBackend::bufferFill() const {
    if (!mpv_) return 0.0;
    std::lock_guard<std::mutex> lock(propMutex_);
    return cachedBufferFill_;
}

std::string MpvBackend::title() const {
    if (!mpv_) return {};
    char* str = mpv_get_property_string(mpv_, "media-title");
    if (!str) return {};
    std::string result(str);
    mpv_free(str);
    return result;
}

void MpvBackend::eventLoop() {
    while (running_.load()) {
        mpv_event* event = mpv_wait_event(mpv_, 0.1);
        if (!event) continue;

        switch (event->event_id) {
        case MPV_EVENT_SHUTDOWN:
            running_.store(false);
            break;

        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* prop = (mpv_event_property*)event->data;
            handlePropertyChange(prop->name, prop);
            break;
        }

        case MPV_EVENT_LOG_MESSAGE: {
            auto* lm = (mpv_event_log_message*)event->data;
            if (lm->log_level >= MPV_LOG_LEVEL_WARN)
                LOG_WARN("MPV", "[%s] %s", lm->prefix ? lm->prefix : "?",
                         lm->text ? lm->text : "");
            break;
        }

        case MPV_EVENT_END_FILE: {
            auto* end = (mpv_event_end_file*)event->data;
            if (end->reason == MPV_END_FILE_REASON_EOF) {
                state_.store(State::Ended);
                if (onPlaybackEnded) onPlaybackEnded();
                LOG_INFO("MPV", "playback ended (EOF)");
            } else if (end->reason == MPV_END_FILE_REASON_ERROR) {
                const char* es = mpv_error_string(end->error);
                LOG_ERROR("MPV", "playback FAILED: %s (%d) hwdec=%s",
                          es ? es : "?", end->error, hwdecCurrent_.c_str());
                state_.store(State::Ended);

                // hwdec 降级触发条件:
                // 1. 确实尝试过硬件解码 (hwdecCurrent_ 非空且非 "no")
                // 2. 同一文件重试未超限 (上限 2 次)
                // 3. 当前文件路径与重试路径一致 (换文件重置)
                // P4-2: 降级时不再触发 onPlaybackEnded, 避免 auto-next 级联
                bool hwdecRetry = false;
                if (!hwdecCurrent_.empty() && hwdecCurrent_ != "no" &&
                    hwdecRetryPath_ == path_ && hwdecRetryCount_ < 2) {
                    retryWithHwdecFallback();
                    hwdecRetry = true;
                }

                if (!hwdecRetry) {
                    // 非 hwdec 降级错误 → 通知 UI（网络错误/解码错误等）
                    if (onPlaybackError) onPlaybackError(es ? es : "Unknown error");
                    if (onPlaybackEnded) onPlaybackEnded();
                }
            } else if (end->reason == MPV_END_FILE_REASON_STOP) {
                LOG_DBG("MPV", "playback stopped");
            }
            break;
        }

        case MPV_EVENT_START_FILE:
            state_.store(State::Playing);
            LOG_DBG("MPV", "file start");
            break;

        case MPV_EVENT_FILE_LOADED:
            if (onFileLoaded) onFileLoaded();
            LOG_INFO("MPV", "file loaded");
            break;

        default:
            break;
        }
    }
}

void MpvBackend::handlePropertyChange(const char* name, mpv_event_property* prop) {
    if (std::strcmp(name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
        int paused = *(int*)prop->data;
        state_.store(paused ? State::Paused : State::Playing);
    }
    else if (std::strcmp(name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
        std::lock_guard<std::mutex> lock(propMutex_);
        cachedClock_ = *(double*)prop->data;
    }
    else if (std::strcmp(name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
        std::lock_guard<std::mutex> lock(propMutex_);
        cachedDuration_ = *(double*)prop->data;
    }
    else if (std::strcmp(name, "volume") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
        volume_.store((float)(*(double*)prop->data / 100.0));
    }
    else if (std::strcmp(name, "speed") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
        speed_.store((float)*(double*)prop->data);
    }
    else if (std::strcmp(name, "hwdec-current") == 0 && prop->format == MPV_FORMAT_STRING) {
        const char* hw = *(char**)prop->data;
        hwdecCurrent_ = hw ? hw : "";
        hwDecode_ = (!hwdecCurrent_.empty() && hwdecCurrent_ != "no");
        LOG_INFO("MPV", "hwdec-current=%s active=%d", hwdecCurrent_.c_str(), hwDecode_.load());
    }
    else if (std::strcmp(name, "width") == 0 && prop->format == MPV_FORMAT_INT64) {
        videoWidth_.store((int)*(int64_t*)prop->data);
    }
    else if (std::strcmp(name, "height") == 0 && prop->format == MPV_FORMAT_INT64) {
        videoHeight_.store((int)*(int64_t*)prop->data);
    }
    else if (std::strcmp(name, "mute") == 0 && prop->format == MPV_FORMAT_FLAG) {
        muted_.store(*(int*)prop->data != 0);
    }
    else if (std::strcmp(name, "paused-for-cache") == 0 && prop->format == MPV_FORMAT_FLAG) {
        bool buf = *(int*)prop->data != 0;
        isBuffering_.store(buf);
        cachedBufferFill_ = buf ? 0.0 : 1.0;
    }
    else if (std::strcmp(name, "demuxer-cache-state") == 0 && prop->format == MPV_FORMAT_NODE) {
        // 解析 demuxer-cache-state 获取 fw.byte 和 total 字节计算缓存百分比
        mpv_node* node = (mpv_node*)prop->data;
        if (node && node->format == MPV_FORMAT_NODE_MAP) {
            mpv_node_list* list = node->u.list;
            double fwBytes = 0, totalBytes = 0;
            for (int i = 0; i < list->num; i++) {
                if (std::strcmp(list->keys[i], "fw.byte") == 0 &&
                    list->values[i].format == MPV_FORMAT_DOUBLE) {
                    fwBytes = list->values[i].u.double_;
                } else if (std::strcmp(list->keys[i], "total") == 0 &&
                           list->values[i].format == MPV_FORMAT_DOUBLE) {
                    totalBytes = list->values[i].u.double_;
                }
            }
            if (totalBytes > 0) {
                double level = fwBytes / totalBytes;
                if (level < 0.0) level = 0.0;
                if (level > 1.0) level = 1.0;
                bufferingLevel_.store(level);
            }
        }
    }
    else if (std::strcmp(name, "eof-reached") == 0 && prop->format == MPV_FORMAT_FLAG) {
        // keep-open 下播完的唯一信号; 去重: 仅从未触发态进入
        if (*(int*)prop->data && !eofFired_) {
            eofFired_ = true;
            state_.store(State::Ended);
            LOG_INFO("MPV", "playback ended (eof-reached)");
            if (onPlaybackEnded) onPlaybackEnded();
        } else if (!*(int*)prop->data) {
            eofFired_ = false;   // 新文件加载后复位
        }
    }
}

// ---- hwdec 四级降级链 ----
// auto-copy-safe → auto-safe → d3d11va → no

const char* MpvBackend::nextHwdecLevel() {
    if (hwdecCurrent_.empty() || hwdecCurrent_ == "no" ||
        hwdecCurrent_ == "auto-copy-safe") {
        return "auto-safe";
    } else if (hwdecCurrent_ == "auto-safe") {
        return "d3d11va";
    } else if (hwdecCurrent_ == "d3d11va") {
        return "no";
    }
    return nullptr;
}

void MpvBackend::retryWithHwdecFallback() {
    const char* next = nextHwdecLevel();
    if (!next) {
        LOG_WARN("MPV", "hwdec fallback exhausted");
        return;
    }

    hwdecRetryCount_++;
    LOG_INFO("MPV", "hwdec fallback: #%d %s -> %s (file=%s)",
             hwdecRetryCount_, hwdecCurrent_.c_str(), next, path_.c_str());

    mpv_set_property_string(mpv_, "hwdec", next);

    const char* stopCmd[] = { "stop", NULL };
    mpv_command(mpv_, stopCmd);

    const char* loadCmd[] = { "loadfile", path_.c_str(), NULL };
    int ret = mpv_command(mpv_, loadCmd);
    if (ret < 0) {
        LOG_ERROR("MPV", "hwdec fallback reload failed: %s", mpv_error_string(ret));
    } else {
        int unpaused = 0;
        mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &unpaused);
        state_.store(State::Playing);
        eofFired_.store(false);
    }
}
