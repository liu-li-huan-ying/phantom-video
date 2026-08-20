#include "subtitle/subtitle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

extern "C" {
#include <libavutil/mathematics.h>
}

void SubtitleTrack::clear() {
    events_.clear();
}

void SubtitleTrack::add(double start, double end, std::string text) {
    if (end <= start) end = start + 1.0;
    if (text.empty()) return;
    events_.push_back({ start, end, std::move(text) });
}

std::string SubtitleTrack::textAt(double t) const {
    std::string out;
    for (const auto& e : events_) {
        if (t < e.start) break;
        if (t >= e.start && t < e.end) out = e.text;
    }
    return out;
}

static std::string readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    // Strip UTF-8 BOM
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s = s.substr(3);
    // Normalize CRLF -> LF
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\r') out += c;
    return out;
}

static double parseSrtTime(const std::string& s) {
    int h = 0, m = 0, sec = 0, ms = 0;
    char sep = ',';
    std::sscanf(s.c_str(), "%d:%d:%d%c%d", &h, &m, &sec, &sep, &ms);
    return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
}

bool SubtitleTrack::loadSrt(const std::string& path) {
    std::string content = readWholeFile(path);
    if (content.empty()) return false;

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        // Skip empty lines and numeric index lines
        if (line.empty()) continue;
        bool isNumber = true;
        for (char c : line)
            if (c < '0' || c > '9') { isNumber = false; break; }
        if (isNumber) continue;

        auto arrow = line.find("-->");
        if (arrow == std::string::npos) continue;

        double start = parseSrtTime(line.substr(0, arrow));
        double end = parseSrtTime(line.substr(arrow + 3));

        std::string text;
        while (std::getline(in, line)) {
            if (line.empty()) break;
            if (!text.empty()) text += '\n';
            text += line;
        }
        add(start, end, text);
    }
    return !events_.empty();
}

static double parseAssTime(const std::string& s) {
    // H:MM:SS.cc or 0:00:01.00
    int h = 0, m = 0, sec = 0, cs = 0;
    std::sscanf(s.c_str(), "%d:%d:%d.%d", &h, &m, &sec, &cs);
    return h * 3600.0 + m * 60.0 + sec + cs / 100.0;
}

std::string SubtitleTrack::assStripTags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (c == '{') { inTag = true; continue; }
        if (c == '}') { inTag = false; continue; }
        if (!inTag) out += c;
    }
    // Convert \N and \n to space
    std::string result;
    result.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\\' && i + 1 < out.size() && (out[i + 1] == 'N' || out[i + 1] == 'n')) {
            result += ' ';
            ++i;
        } else {
            result += out[i];
        }
    }
    return result;
}

std::string SubtitleTrack::assDialogueText(const std::string& line) {
    // Dialogue: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
    std::vector<std::string> fields;
    size_t pos = 0;
    while (true) {
        size_t comma = line.find(',', pos);
        if (comma == std::string::npos) {
            fields.push_back(line.substr(pos));
            break;
        }
        fields.push_back(line.substr(pos, comma - pos));
        pos = comma + 1;
    }
    if (fields.size() < 10) return {};
    return assStripTags(fields[9]);
}

bool SubtitleTrack::loadAss(const std::string& path) {
    std::string content = readWholeFile(path);
    if (content.empty()) return false;

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("Dialogue:", 0) != 0) continue;
        // Dialogue: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
        size_t pos = 9;  // after "Dialogue:"
        // Skip Layer field
        size_t c1 = line.find(',', pos);
        if (c1 == std::string::npos) continue;
        pos = c1 + 1;
        // Start field
        size_t c2 = line.find(',', pos);
        if (c2 == std::string::npos) continue;
        std::string startS = line.substr(pos, c2 - pos);
        pos = c2 + 1;
        // End field
        size_t c3 = line.find(',', pos);
        if (c3 == std::string::npos) continue;
        std::string endS = line.substr(pos, c3 - pos);
        pos = c3 + 1;
        // Skip Style, Name, MarginL, MarginR, MarginV, Effect (6 fields)
        for (int k = 0; k < 6; ++k) {
            size_t comma = line.find(',', pos);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        std::string text = assStripTags(line.substr(pos));
        add(parseAssTime(startS), parseAssTime(endS), text);
    }
    std::sort(events_.begin(), events_.end(),
              [](const SubtitleEvent& a, const SubtitleEvent& b) { return a.start < b.start; });
    return !events_.empty();
}

SubtitleDecoder::~SubtitleDecoder() {
    if (ctx_) avcodec_free_context(&ctx_);
}

void SubtitleDecoder::flushBuffers() {
    if (ctx_) avcodec_flush_buffers(ctx_);
}

bool SubtitleDecoder::open(const AVCodecParameters* par) {
    if (!par) return false;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    if (avcodec_parameters_to_context(ctx_, par) < 0) return false;
    if (avcodec_open2(ctx_, codec, nullptr) < 0) return false;
    return true;
}

bool SubtitleDecoder::decode(const AVPacket* pkt, double timeBase, SubtitleTrack& out) {
    if (!ctx_) return false;
    AVSubtitle sub;
    int got = 0;
    if (avcodec_decode_subtitle2(ctx_, &sub, &got, pkt) < 0 || !got) return false;

    // FFmpeg srt decoder does not set sub.pts for container-timed subtitles;
    // use packet pts (stream time_base) instead. start_display_time is relative
    // to sub.pts; when absent, rely on packet pts/duration.
    double start = 0.0, end = 0.0;
    if (sub.pts != AV_NOPTS_VALUE) {
        start = (double)sub.pts / AV_TIME_BASE + (double)sub.start_display_time / 1000.0;
        end = (double)sub.pts / AV_TIME_BASE + (double)sub.end_display_time / 1000.0;
    }
    if (pkt->pts != AV_NOPTS_VALUE) {
        double p = (double)pkt->pts * timeBase;
        if (sub.start_display_time == 0) start = p;
        if (sub.end_display_time == 0) {
            if (pkt->duration > 0)
                end = p + (double)pkt->duration * timeBase;
            else
                end = start + 2.0;  // heuristic
        }
    }

    for (int i = 0; i < sub.num_rects; ++i) {
        AVSubtitleRect* r = sub.rects[i];
        if (!r) continue;
        std::string text;
        if (r->type == SUBTITLE_TEXT && r->text) {
            text = r->text;
        } else if (r->type == SUBTITLE_ASS && r->ass) {
            // FFmpeg srt decoder produces "0,0,Default,,0,0,0,,Hello World"
            // (9 fields, no "Dialogue:" prefix, no times).
            std::string ass = r->ass;
            text = SubtitleTrack::assStripTags(ass);
            // Strip leading "Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,"
            // i.e. everything up to the 9th comma (fields 0-8), keep Text.
            size_t comma = std::string::npos;
            int n = 0;
            for (size_t i2 = 0; i2 < ass.size() && n < 9; ++i2) {
                if (ass[i2] == ',') { comma = i2; ++n; }
            }
            if (comma != std::string::npos && comma + 1 < ass.size()) {
                std::string t2 = SubtitleTrack::assStripTags(ass.substr(comma + 1));
                if (!t2.empty()) text = t2;
            }
        }
        if (!text.empty()) out.add(start, end, text);
    }
    avsubtitle_free(&sub);
    return true;
}