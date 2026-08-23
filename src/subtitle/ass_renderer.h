#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <SDL.h>

struct _TTF_Font;
typedef struct _TTF_Font TTF_Font;
struct AVCodecParameters;

// ASS 颜色：BGR + Alpha 格式
struct ASSColor {
    Uint8 b = 0, g = 0, r = 0, a = 255;
    SDL_Color toSDL() const { return { r, g, b, (Uint8)(255 - a) }; }
    SDL_Color toSDLAlpha(Uint8 extraAlpha) const {
        Uint8 combinedA = (Uint8)((int)(255 - a) * extraAlpha / 255);
        return { r, g, b, combinedA };
    }
};

// ASS 样式定义（对应 [V4+ Styles] 中的 Format）
struct ASSStyle {
    std::string name;
    std::string fontname;
    int fontsize = 20;
    ASSColor primaryColor;
    ASSColor secondaryColor;
    ASSColor outlineColor;
    ASSColor shadowColor;
    int bold = false;
    int italic = false;
    int underline = false;
    int strikeout = false;
    int scale_x = 100;
    int scale_y = 100;
    int spacing = 0;
    double angle = 0.0;
    int border_style = 1;  // 1=outline+shadow, 3=opaque box
    double outline = 1.0;
    double shadow = 1.0;
    int alignment = 2;  // numpad alignment (1-9)
    int margin_l = 10, margin_r = 10, margin_v = 10;
    int encoding = 1;
};

// 文本段：带样式的连续文本
struct StyledSegment {
    std::string text;
    ASSColor color;
    ASSColor outlineColor;
    ASSColor shadowColor;
    int fontsize = 0;
    int bold = false;
    int italic = false;
    int underline = false;
    int strikeout = false;
    double outline = 0.0;
    double shadow = 0.0;
    bool operator==(const StyledSegment& o) const {
        return color.r == o.color.r && color.g == o.color.g &&
               color.b == o.color.b && color.a == o.color.a &&
               fontsize == o.fontsize && bold == o.bold && italic == o.italic;
    }
};

// 渲染好的字幕行
struct RenderedSubtitle {
    std::vector<SDL_Texture*> textures;  // 每个 segment 一个纹理
    std::vector<SDL_Rect> srcRects;
    std::vector<SDL_Rect> dstRects;
    int totalW = 0, totalH = 0;
    int alignX = 0, alignY = 0;  // 对齐参考点
    int alignment = 2;           // numpad alignment
};

// 轻量 ASS 渲染器
class ASSRenderer {
public:
    ASSRenderer() = default;
    ~ASSRenderer();

    void init(SDL_Renderer* renderer);
    void shutdown();

    // 加载 ASS 样式信息
    bool loadStyles(const std::string& assContent);

    // 渲染一行字幕
    RenderedSubtitle render(const std::string& dialogueLine,
                            int videoW, int videoH,
                            int playResX, int playResY);

    // 释放渲染结果
    void freeRendered(RenderedSubtitle& sub);

    const ASSStyle* findStyle(const std::string& name) const;

private:
    void parseStyle(const std::string& line);
    std::vector<StyledSegment> parseDialogue(const std::string& text,
                                             const ASSStyle& baseStyle);
    void parseOverrideTags(const std::string& tags, ASSStyle& style);

    SDL_Renderer* renderer_ = nullptr;
    std::vector<ASSStyle> styles_;

    // TTF 字体缓存
    struct FontKey {
        std::string name;
        int size;
        bool bold;
        bool italic;
        bool operator<(const FontKey& o) const;
    };
    std::map<FontKey, TTF_Font*> fontCache_;

    TTF_Font* getFont(const std::string& name, int size, bool bold, bool italic);
};
