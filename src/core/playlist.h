#pragma once
#include <string>
#include <vector>

class Playlist {
public:
    void set(const std::vector<std::string>& files) {
        files_ = files;
        idx_ = files_.empty() ? -1 : 0;
    }
    void set(const std::string& file) {
        files_.clear();
        files_.push_back(file);
        idx_ = 0;
    }
    bool next() {
        if (idx_ < 0 || idx_ + 1 >= (int)files_.size()) return false;
        ++idx_;
        return true;
    }
    bool prev() {
        if (idx_ <= 0) return false;
        --idx_;
        return true;
    }
    bool hasNext() const { return idx_ >= 0 && idx_ + 1 < (int)files_.size(); }
    bool hasPrev() const { return idx_ > 0; }
    const std::string& current() const { return files_[idx_]; }
    int index() const { return idx_; }
    int size() const { return (int)files_.size(); }
    bool empty() const { return files_.empty(); }

private:
    std::vector<std::string> files_;
    int idx_ = -1;
};