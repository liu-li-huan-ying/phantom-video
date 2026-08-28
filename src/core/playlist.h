#pragma once
#include <string>
#include <vector>

enum class PlayMode { Single, Loop, Shuffle };

class Playlist {
public:
    void set(const std::vector<std::string>& files) {
        files_ = files;
        idx_ = files_.empty() ? -1 : 0;
        rebuildOrder();
    }
    void set(const std::string& file) {
        files_.clear();
        files_.push_back(file);
        idx_ = 0;
        rebuildOrder();
    }
    // Scan the directory containing |file| for video files (filename-sorted),
    // locate |file| as the current item. Returns false if nothing found.
    bool scanDirectory(const std::string& file);

    void setMode(PlayMode m);
    PlayMode mode() const { return mode_; }

    // Manual navigation (Prev/Next buttons). Loop/Shuffle always succeed.
    bool next();
    bool prev();
    // True when the player should auto-advance after the current file ends.
    bool hasNext() const;
    bool hasPrev() const;

    const std::string& current() const { return files_[order_[idx_]]; }
    int index() const { return idx_; }
    int size() const { return (int)files_.size(); }
    bool empty() const { return files_.empty(); }
    const std::string& fileAt(int displayIndex) const { return files_[order_[displayIndex]]; }

    // P1-5: 动态增删
    void add(const std::string& file) {
        files_.push_back(file);
        rebuildOrder();
    }
    bool remove(int displayIndex) {
        if (displayIndex < 0 || displayIndex >= (int)files_.size()) return false;
        int realIdx = order_[displayIndex];
        files_.erase(files_.begin() + realIdx);
        if (files_.empty()) { idx_ = -1; order_.clear(); return true; }
        rebuildOrder();
        // 修正当前索引
        if (idx_ >= (int)files_.size()) idx_ = (int)files_.size() - 1;
        return true;
    }

private:
    int currentIndex() const;
    void rebuildOrder();

    std::vector<std::string> files_;
    std::vector<int> order_;
    int idx_ = -1;
    PlayMode mode_ = PlayMode::Loop;
};