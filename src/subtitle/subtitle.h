#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

struct SubtitleEvent {
    double start = 0.0;
    double end = 0.0;
    std::string text;
    std::string rawAss;  // 原始 ASS Dialogue 行（含 override tags）
};

class SubtitleTrack {
public:
    void clear();
    void add(double start, double end, std::string text);
    bool loadSrt(const std::string& path);
    bool loadAss(const std::string& path);
    std::string textAt(double t) const;
    std::string rawDialogueAt(double t) const;
    bool empty() const { return events_.empty(); }
    int size() const { return (int)events_.size(); }
    const SubtitleEvent& back() const { return events_.back(); }
    const std::string& assContent() const { return assContent_; }

    static std::string assStripTags(const std::string& s);
    static std::string assDialogueText(const std::string& line);

private:
    std::vector<SubtitleEvent> events_;
    std::string assContent_;  // ASS 文件完整内容（供 ASSRenderer 解析样式）
};

class SubtitleDecoder {
public:
    ~SubtitleDecoder();
    bool open(const AVCodecParameters* par);
    bool decode(const AVPacket* pkt, double timeBase, SubtitleTrack& out);
    bool opened() const { return ctx_ != nullptr; }
    void flushBuffers();

private:
    AVCodecContext* ctx_ = nullptr;
};