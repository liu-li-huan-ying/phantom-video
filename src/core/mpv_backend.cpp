#include "core/mpv_backend.h"
#include "core/logger.h"
#include "core/config.h"
#include <cstring>
#include <cstdlib>

MpvBackend::~MpvBackend() {
    shutdown();
}

bool MpvBackend::init(HWND hwnd) {
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
    mpv_set_option_string(mpv_, "hwdec", "auto-safe");
    mpv_set_option_string(mpv_, "audio-pitch-correction", "yes");
    mpv_set_option_string(mpv_, "sub-auto", "fuzzy");
    mpv_set_option_string(mpv_, "volume", "80");
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "no-input-default-bindings", "yes");
    mpv_set_option_string(mpv_, "input-vo-keyboard", "no");
    mpv_set_option_string(mpv_, "cursor-autohide", "0");

    // 截图输出到 exe/screenshots/
    {
        std::string dir = exeDir() + "screenshots";
        CreateDirectoryA(dir.c_str(), nullptr);
        mpv_set_option_string(mpv_, "screenshot-format", "png");
        mpv_set_option_string(mpv_, "screenshot-directory", dir.c_str());
    }

    // wid 必须在 mpv_initialize 之前设置
    if (hwnd) {
        char widStr[64];
        std::snprintf(widStr, sizeof(widStr), "%lld", (long long)(intptr_t)hwnd);
        mpv_set_option_string(mpv_, "wid", widStr);
    }

    if (mpv_initialize(mpv_) < 0) {
        LOG_ERROR("MPV", "mpv_initialize failed");
        mpv_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }

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

    running_.store(true);
    eventThread_ = std::thread(&MpvBackend::eventLoop, this);

    LOG_INFO("MPV", "mpv backend initialized");
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

bool MpvBackend::loadFile(const std::string& path) {
    if (!mpv_) return false;

    close();

    const char* cmd[] = { "loadfile", path.c_str(), NULL };
    int ret = mpv_command(mpv_, cmd);
    if (ret < 0) {
        LOG_ERROR("MPV", "loadfile failed: %s (path=%s)", mpv_error_string(ret), path.c_str());
        return false;
    }

    path_ = path;
    hasMedia_.store(true);
    state_.store(State::Playing);
    LOG_INFO("MPV", "loaded: %s", path.c_str());
    return true;
}

void MpvBackend::close() {
    if (!mpv_ || !hasMedia_.load()) return;

    const char* cmd[] = { "stop", NULL };
    mpv_command(mpv_, cmd);

    hasMedia_.store(false);
    state_.store(State::Idle);
    path_.clear();
}

void MpvBackend::togglePause() {
    if (!mpv_ || !hasMedia_.load()) return;

    int paused = (state_.load() == State::Paused) ? 0 : 1;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused);
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

    speed_.store(s);
    mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &s);
}

double MpvBackend::clock() const {
    if (!mpv_) return 0.0;
    std::lock_guard<std::mutex> lock(propMutex_);
    return cachedClock_;
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

        case MPV_EVENT_END_FILE: {
            auto* end = (mpv_event_end_file*)event->data;
            if (end->reason == MPV_END_FILE_REASON_EOF) {
                state_.store(State::Ended);
                if (onPlaybackEnded) onPlaybackEnded();
                LOG_INFO("MPV", "playback ended (EOF)");
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
        hwDecode_ = (hw && std::strlen(hw) > 0 && std::strcmp(hw, "no") != 0);
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
        cachedBufferFill_ = *(int*)prop->data ? 0.0 : 1.0;
    }
}
