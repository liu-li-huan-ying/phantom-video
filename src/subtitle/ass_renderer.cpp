#include "subtitle/ass_renderer.h"
#include "core/logger.h"
#include <SDL_ttf.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

static const char* kMod = "ASR";

ASSRenderer::~ASSRenderer() {
    shutdown();
}

void ASSRenderer::init(SDL_Renderer* renderer) {
    renderer_ = renderer;
    if (!TTF_WasInit()) {
        if (TTF_Init() == -1) {
            LOG_ERROR(kMod, "TTF_Init failed: %s", TTF_GetError());
            return;
        }
    }
    LOG_INFO(kMod, "ASSRenderer initialized");
}

void ASSRenderer::shutdown() {
    for (auto& [key, font] : fontCache_) {
        if (font) TTF_CloseFont(font);
    }
    fontCache_.clear();
    renderer_ = nullptr;
}

bool ASSRenderer::FontKey::operator<(const FontKey& o) const {
    if (name != o.name) return name < o.name;
    if (size != o.size) return size < o.size;
    if (bold != o.bold) return bold < o.bold;
    return italic < o.italic;
}

TTF_Font* ASSRenderer::getFont(const std::string& name, int size, bool bold, bool italic) {
    FontKey key{ name, size, bold, italic };
    auto it = fontCache_.find(key);
    if (it != fontCache_.end()) return it->second;

    std::string path = name + ".ttf";
    TTF_Font* font = TTF_OpenFont(path.c_str(), size);
    if (!font) {
        const char* fallbacks[] = {
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/simsun.ttc",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/times.ttf",
            nullptr
        };
        for (const char** fb = fallbacks; *fb; ++fb) {
            font = TTF_OpenFont(*fb, size);
            if (font) break;
        }
    }
    if (font) {
        TTF_SetFontStyle(font, (bold ? TTF_STYLE_BOLD : 0) |
                               (italic ? TTF_STYLE_ITALIC : 0));
    }
    fontCache_[key] = font;
    return font;
}

static ASSColor parseASSColor(const std::string& s) {
    ASSColor c;
    if (s.size() < 8) return c;
    size_t start = (s[0] == '&' && s[1] == 'H') ? 2 : 0;
    size_t end = s.size();
    if (end > 0 && s[end - 1] == '&') --end;
    std::string hex = s.substr(start, end - start);
    if (hex.size() == 8) {
        unsigned int val = 0;
        std::sscanf(hex.c_str(), "%X", &val);
        c.a = (val >> 24) & 0xFF;
        c.b = (val >> 16) & 0xFF;
        c.g = (val >> 8) & 0xFF;
        c.r = val & 0xFF;
    } else if (hex.size() == 6) {
        unsigned int val = 0;
        std::sscanf(hex.c_str(), "%X", &val);
        c.b = (val >> 16) & 0xFF;
        c.g = (val >> 8) & 0xFF;
        c.r = val & 0xFF;
    }
    return c;
}

void ASSRenderer::parseStyle(const std::string& line) {
    std::vector<std::string> fields;
    std::istringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
        fields.push_back(field);
    }
    if (fields.size() < 23) return;

    ASSStyle s;
    s.name = fields[0];
    s.fontname = fields[1];
    s.fontsize = std::stoi(fields[2]);
    s.primaryColor = parseASSColor(fields[3]);
    s.secondaryColor = parseASSColor(fields[4]);
    s.outlineColor = parseASSColor(fields[5]);
    s.shadowColor = parseASSColor(fields[6]);
    s.bold = std::stoi(fields[7]);
    s.italic = std::stoi(fields[8]);
    s.underline = std::stoi(fields[9]);
    s.strikeout = std::stoi(fields[10]);
    s.scale_x = std::stoi(fields[11]);
    s.scale_y = std::stoi(fields[12]);
    s.spacing = std::stoi(fields[13]);
    s.angle = std::stod(fields[14]);
    s.border_style = std::stoi(fields[15]);
    s.outline = std::stod(fields[16]);
    s.shadow = std::stod(fields[17]);
    s.alignment = std::stoi(fields[18]);
    s.margin_l = std::stoi(fields[19]);
    s.margin_r = std::stoi(fields[20]);
    s.margin_v = std::stoi(fields[21]);
    s.encoding = std::stoi(fields[22]);

    styles_.push_back(std::move(s));
    LOG_DBG(kMod, "Parsed style: %s font=%s size=%d",
            styles_.back().name.c_str(), styles_.back().fontname.c_str(),
            styles_.back().fontsize);
}

bool ASSRenderer::loadStyles(const std::string& assContent) {
    styles_.clear();
    bool inStyles = false;
    std::istringstream in(assContent);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("[V4+ Styles]", 0) == 0) {
            inStyles = true;
            continue;
        }
        if (!line.empty() && line[0] == '[') {
            inStyles = false;
            continue;
        }
        if (!inStyles) continue;
        if (line.rfind("Style:", 0) == 0) {
            parseStyle(line.substr(6));
        }
    }
    LOG_INFO(kMod, "Loaded %d ASS styles", (int)styles_.size());
    return !styles_.empty();
}

const ASSStyle* ASSRenderer::findStyle(const std::string& name) const {
    for (auto& s : styles_)
        if (s.name == name) return &s;
    return styles_.empty() ? nullptr : &styles_[0];
}

void ASSRenderer::parseOverrideTags(const std::string& tags, ASSStyle& style) {
    size_t i = 0;
    while (i < tags.size()) {
        if (tags[i] != '\\') { ++i; continue; }
        ++i;
        if (i >= tags.size()) break;

        std::string tag;
        while (i < tags.size() && std::isalpha(tags[i])) {
            tag += tags[i++];
        }
        if (tag.empty()) continue;

        std::string val;
        if (i < tags.size() && tags[i] == '(') {
            ++i;
            int depth = 1;
            while (i < tags.size() && depth > 0) {
                if (tags[i] == '(') ++depth;
                else if (tags[i] == ')') --depth;
                if (depth > 0) val += tags[i];
                ++i;
            }
        } else {
            while (i < tags.size() && tags[i] != '\\' && tags[i] != '}') {
                val += tags[i++];
            }
        }

        if (tag == "b" || tag == "B") {
            style.bold = (!val.empty() && val != "0") ? 1 : 0;
        } else if (tag == "i" || tag == "I") {
            style.italic = (!val.empty() && val != "0") ? 1 : 0;
        } else if (tag == "u") {
            style.underline = (!val.empty() && val != "0") ? 1 : 0;
        } else if (tag == "s") {
            style.strikeout = (!val.empty() && val != "0") ? 1 : 0;
        } else if (tag == "fn") {
            if (!val.empty()) style.fontname = val;
        } else if (tag == "fs") {
            if (!val.empty()) {
                if (val[0] == '+' || val[0] == '-')
                    style.fontsize += std::stoi(val);
                else
                    style.fontsize = std::stoi(val);
                if (style.fontsize < 1) style.fontsize = 1;
                if (style.fontsize > 200) style.fontsize = 200;
            }
        } else if (tag == "c") {
            if (!val.empty()) style.primaryColor = parseASSColor(val);
        } else if (tag == "1c") {
            if (!val.empty()) style.primaryColor = parseASSColor(val);
        } else if (tag == "2c") {
            if (!val.empty()) style.secondaryColor = parseASSColor(val);
        } else if (tag == "3c") {
            if (!val.empty()) style.outlineColor = parseASSColor(val);
        } else if (tag == "4c") {
            if (!val.empty()) style.shadowColor = parseASSColor(val);
        } else if (tag == "alpha") {
            if (!val.empty()) {
                ASSColor c = style.primaryColor;
                c.a = (Uint8)std::stoi(val, nullptr, 16);
                style.primaryColor = c;
            }
        } else if (tag == "1a") {
            if (!val.empty()) {
                ASSColor c = style.primaryColor;
                c.a = (Uint8)std::stoi(val, nullptr, 16);
                style.primaryColor = c;
            }
        } else if (tag == "bord") {
            if (!val.empty()) style.outline = std::stod(val);
        } else if (tag == "shad") {
            if (!val.empty()) style.shadow = std::stod(val);
        } else if (tag == "fscx") {
            if (!val.empty()) style.scale_x = std::stoi(val);
        } else if (tag == "fscy") {
            if (!val.empty()) style.scale_y = std::stoi(val);
        } else if (tag == "fsp") {
            if (!val.empty()) style.spacing = std::stoi(val);
        } else if (tag == "an") {
            if (!val.empty()) style.alignment = std::stoi(val);
        }
        // Skip: \\pos, \\move, \\fad, \\fade, \\t, \\clip, \\org, \\frz, \\frx, \\fry
        // These require frame-level state; supported in future
    }
}

std::vector<StyledSegment> ASSRenderer::parseDialogue(
        const std::string& text, const ASSStyle& baseStyle) {
    std::vector<StyledSegment> result;
    ASSStyle cur = baseStyle;
    std::string buf;

    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            // Flush current buffer
            if (!buf.empty()) {
                StyledSegment seg;
                seg.text = buf;
                seg.color = cur.primaryColor;
                seg.outlineColor = cur.outlineColor;
                seg.shadowColor = cur.shadowColor;
                seg.fontsize = cur.fontsize;
                seg.bold = cur.bold;
                seg.italic = cur.italic;
                seg.underline = cur.underline;
                seg.strikeout = cur.strikeout;
                seg.outline = cur.outline;
                seg.shadow = cur.shadow;
                result.push_back(std::move(seg));
                buf.clear();
            }
            // Parse override block
            size_t close = text.find('}', i + 1);
            if (close == std::string::npos) break;
            parseOverrideTags(text.substr(i + 1, close - i - 1), cur);
            i = close + 1;
        } else if (text[i] == '\\' && i + 1 < text.size()) {
            char next = text[i + 1];
            if (next == 'N' || next == 'n') {
                if (!buf.empty()) {
                    StyledSegment seg;
                    seg.text = buf;
                    seg.color = cur.primaryColor;
                    seg.outlineColor = cur.outlineColor;
                    seg.shadowColor = cur.shadowColor;
                    seg.fontsize = cur.fontsize;
                    seg.bold = cur.bold;
                    seg.italic = cur.italic;
                    seg.underline = cur.underline;
                    seg.strikeout = cur.strikeout;
                    seg.outline = cur.outline;
                    seg.shadow = cur.shadow;
                    result.push_back(std::move(seg));
                    buf.clear();
                }
                StyledSegment nlSeg;
                nlSeg.text = "\n";
                nlSeg.fontsize = cur.fontsize;
                result.push_back(std::move(nlSeg));
                i += 2;
            } else {
                buf += text[i];
                ++i;
            }
        } else {
            buf += text[i];
            ++i;
        }
    }

    if (!buf.empty()) {
        StyledSegment seg;
        seg.text = buf;
        seg.color = cur.primaryColor;
        seg.outlineColor = cur.outlineColor;
        seg.shadowColor = cur.shadowColor;
        seg.fontsize = cur.fontsize;
        seg.bold = cur.bold;
        seg.italic = cur.italic;
        seg.underline = cur.underline;
        seg.strikeout = cur.strikeout;
        seg.outline = cur.outline;
        seg.shadow = cur.shadow;
        result.push_back(std::move(seg));
    }

    return result;
}

RenderedSubtitle ASSRenderer::render(const std::string& dialogueLine,
                                     int videoW, int videoH,
                                     int playResX, int playResY) {
    RenderedSubtitle out;
    if (!renderer_ || dialogueLine.empty()) return out;

    // Extract style name and text from Dialogue line
    std::string styleName = "Default";
    std::string text = dialogueLine;
    {
        // Dialogue: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text
        std::vector<std::string> fields;
        std::istringstream ss(dialogueLine);
        std::string field;
        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }
        if (fields.size() >= 10) {
            styleName = fields[3];
            text = fields[9];
            for (size_t k = 10; k < fields.size(); ++k)
                text += "," + fields[k];
        }
    }

    const ASSStyle* baseStyle = findStyle(styleName);
    if (!baseStyle) {
        if (!styles_.empty()) baseStyle = &styles_[0];
        else return out;
    }

    // Remove inline comments (anything after // in text)
    {
        size_t commentPos = text.find("//");
        if (commentPos != std::string::npos)
            text = text.substr(0, commentPos);
    }

    ASSStyle resolved = *baseStyle;
    std::vector<StyledSegment> segments = parseDialogue(text, resolved);

    float scaleX = (playResX > 0) ? (float)videoW / playResX : 1.0f;
    float scaleY = (playResY > 0) ? (float)videoH / playResY : 1.0f;
    float scale = std::min(scaleX, scaleY);

    int marginL = (int)(resolved.margin_l * scaleX);
    int marginR = (int)(resolved.margin_r * scaleX);
    int marginV = (int)(resolved.margin_v * scaleY);

    // Render each segment
    int lineH = 0;
    int totalW = 0;
    int curLineW = 0;
    int lineCount = 1;

    for (auto& seg : segments) {
        int fontSize = (int)(seg.fontsize * scale);
        if (fontSize < 1) fontSize = 1;
        if (fontSize > 200) fontSize = 200;

        TTF_Font* font = getFont(resolved.fontname, fontSize, seg.bold, seg.italic);
        if (!font) continue;

        // Split segment by newlines
        std::istringstream segStream(seg.text);
        std::string line;
        bool firstLine = true;
        while (std::getline(segStream, line, '\n')) {
            if (!firstLine) {
                ++lineCount;
                if (curLineW > totalW) totalW = curLineW;
                curLineW = 0;
            }
            firstLine = false;

            if (line.empty()) continue;

            int tw = 0, th = 0;
            TTF_SizeUTF8(font, line.c_str(), &tw, &th);
            if (th > lineH) lineH = th;

            // Create text surface with outline
            SDL_Color sdlColor = seg.color.toSDL();
            SDL_Color outlineSdl = seg.outlineColor.toSDL();

            int outlinePx = (int)(seg.outline * scale);
            if (outlinePx < 0) outlinePx = 0;
            if (outlinePx > 8) outlinePx = 8;

            // Render outline by drawing text at offsets
            SDL_Surface* surf = nullptr;
            if (outlinePx > 0) {
                TTF_SetFontOutline(font, outlinePx);
                int oth = 0;
                TTF_SizeUTF8(font, line.c_str(), &tw, &oth);
                surf = SDL_CreateRGBSurfaceWithFormat(0, tw + outlinePx * 2, oth + outlinePx * 2,
                                                       32, SDL_PIXELFORMAT_ARGB8888);
                if (surf) {
                    SDL_FillRect(surf, nullptr, SDL_MapRGBA(surf->format, 0, 0, 0, 0));
                    // Draw outline in 8 directions
                    for (int dx = -outlinePx; dx <= outlinePx; ++dx) {
                        for (int dy = -outlinePx; dy <= outlinePx; dy += (outlinePx > 0 ? 1 : 2)) {
                            if (dx == 0 && dy == 0) continue;
                            if (dx * dx + dy * dy > outlinePx * outlinePx + outlinePx) continue;
                            SDL_Surface* ol = TTF_RenderUTF8_Blended(font, line.c_str(), outlineSdl);
                            if (ol) {
                                SDL_Rect dst{ outlinePx + dx, outlinePx + dy, ol->w, ol->h };
                                SDL_BlitSurface(ol, nullptr, surf, &dst);
                                SDL_FreeSurface(ol);
                            }
                        }
                    }
                    // Draw main text on top
                    SDL_Surface* main = TTF_RenderUTF8_Blended(font, line.c_str(), sdlColor);
                    if (main) {
                        SDL_Rect dst{ outlinePx, outlinePx, main->w, main->h };
                        SDL_BlitSurface(main, nullptr, surf, &dst);
                        SDL_FreeSurface(main);
                    }
                    TTF_SetFontOutline(font, 0);
                }
            } else {
                surf = TTF_RenderUTF8_Blended(font, line.c_str(), sdlColor);
            }

            if (!surf) continue;

            SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
            if (tex) {
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                out.textures.push_back(tex);
                out.srcRects.push_back({ 0, 0, surf->w, surf->h });
                out.dstRects.push_back({ 0, 0, surf->w, surf->h });
                curLineW += surf->w;
            }
            SDL_FreeSurface(surf);
        }
    }
    if (curLineW > totalW) totalW = curLineW;
    out.totalW = totalW;
    out.totalH = lineH * lineCount + (lineCount - 1) * 2;
    out.alignment = resolved.alignment;

    // Position based on alignment (numpad: 1-9)
    int areaW = videoW - marginL - marginR;
    int baseX = marginL;
    int baseY = videoH - marginV - out.totalH;

    int alignH = out.alignment <= 3 ? 3 : (out.alignment <= 6 ? 6 : 9);
    int alignV = ((out.alignment - 1) % 3);  // 0=left, 1=center, 2=right

    switch (alignH) {
        case 3: baseY = videoH - marginV - out.totalH; break;  // Bottom
        case 6: baseY = (videoH - out.totalH) / 2; break;      // Middle
        case 9: baseY = marginV; break;                          // Top
    }

    switch (alignV) {
        case 0: baseX = marginL; break;                          // Left
        case 1: baseX = marginL + (areaW - totalW) / 2; break;  // Center
        case 2: baseX = videoW - marginR - totalW; break;        // Right
    }

    // Layout segments into destination rects
    int cx = baseX;
    int cy = baseY;
    int segIdx = 0;
    int lineHCalc = lineH + 2;

    for (auto& seg : segments) {
        int fontSize = (int)(seg.fontsize * scale);
        if (fontSize < 1) fontSize = 1;
        if (fontSize > 200) fontSize = 200;

        std::istringstream segStream(seg.text);
        std::string line;
        bool firstLine = true;
        while (std::getline(segStream, line, '\n')) {
            if (!firstLine) {
                cx = baseX;
                cy += lineHCalc;
            }
            firstLine = false;

            if (segIdx < (int)out.dstRects.size()) {
                out.dstRects[segIdx].x = cx;
                out.dstRects[segIdx].y = cy;
                cx += out.dstRects[segIdx].w;
                ++segIdx;
            }
        }
    }

    LOG_DBG(kMod, "Rendered subtitle: %dx%d align=%d segs=%d",
            out.totalW, out.totalH, out.alignment, (int)out.textures.size());
    return out;
}

void ASSRenderer::freeRendered(RenderedSubtitle& sub) {
    for (auto* tex : sub.textures) {
        SDL_DestroyTexture(tex);
    }
    sub.textures.clear();
    sub.srcRects.clear();
    sub.dstRects.clear();
    sub.totalW = sub.totalH = 0;
}
