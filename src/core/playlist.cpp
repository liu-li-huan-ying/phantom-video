#include "core/playlist.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <random>

static const char* kVideoExts[] = {
    "mp4", "mkv", "avi", "wmv", "mov", "flv", "rm", "rmvb",
    "3gp", "mpg", "mpeg", "vob", "webm", "m4v", "ts", "m2ts",
    "gif", "swf", "asf", "f4v", "mts", "ogv",
};

static bool isVideoExt(const std::string& ext) {
    for (const char* e : kVideoExts)
        if (ext == e) return true;
    return false;
}

static std::mt19937& rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}

bool Playlist::scanDirectory(const std::string& file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(file);
    if (!fs::exists(p, ec)) return false;

    fs::path dir = p.parent_path();
    if (dir.empty()) dir = ".";

    std::vector<std::string> found;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
        if (isVideoExt(ext)) found.push_back(it->path().string());
    }
    if (found.empty()) return false;
    std::sort(found.begin(), found.end());

    files_ = std::move(found);
    idx_ = -1;
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (files_[i] == file) { idx_ = (int)i; break; }
    }
    if (idx_ < 0) idx_ = 0;
    rebuildOrder();
    return true;
}

void Playlist::setMode(PlayMode m) {
    if (m == mode_) return;
    int curFileIdx = currentIndex();
    mode_ = m;
    if (mode_ == PlayMode::Shuffle) {
        std::shuffle(order_.begin(), order_.end(), rng());
        if (curFileIdx >= 0) {
            auto it = std::find(order_.begin(), order_.end(), curFileIdx);
            if (it != order_.end()) {
                std::iter_swap(it, order_.begin());
            } else if (!order_.empty()) {
                order_[0] = curFileIdx;
            }
        }
        idx_ = 0;
    } else {
        int cur = curFileIdx;
        std::sort(order_.begin(), order_.end());
        if (cur >= 0) {
            auto it = std::find(order_.begin(), order_.end(), cur);
            if (it != order_.end()) idx_ = (int)(it - order_.begin());
            else idx_ = 0;
        } else {
            idx_ = 0;
        }
    }
}

int Playlist::currentIndex() const {
    if (idx_ < 0 || order_.empty()) return -1;
    return order_[idx_];
}

void Playlist::rebuildOrder() {
    order_.clear();
    order_.reserve(files_.size());
    for (std::size_t i = 0; i < files_.size(); ++i) order_.push_back((int)i);
    if (idx_ >= (int)order_.size()) idx_ = order_.empty() ? -1 : 0;
}

bool Playlist::next() {
    if (idx_ < 0 || files_.empty()) return false;
    if (idx_ + 1 >= (int)order_.size()) {
        if (mode_ == PlayMode::Single) return false;
        if (mode_ == PlayMode::Shuffle) {
            int cur = order_[idx_];
            std::shuffle(order_.begin(), order_.end(), rng());
            auto it = std::find(order_.begin(), order_.end(), cur);
            if (it != order_.end()) std::iter_swap(it, order_.begin());
            idx_ = 0;
        } else {
            idx_ = 0;
        }
        return true;
    }
    ++idx_;
    return true;
}

bool Playlist::prev() {
    if (idx_ < 0 || files_.empty()) return false;
    if (idx_ <= 0) {
        if (mode_ == PlayMode::Single) return false;
        idx_ = (int)order_.size() - 1;
        return true;
    }
    --idx_;
    return true;
}

bool Playlist::hasNext() const {
    if (idx_ < 0 || files_.empty()) return false;
    if (mode_ == PlayMode::Single) return idx_ + 1 < (int)order_.size();
    return true;
}

bool Playlist::hasPrev() const {
    if (idx_ < 0 || files_.empty()) return false;
    if (mode_ == PlayMode::Single) return idx_ > 0;
    return true;
}