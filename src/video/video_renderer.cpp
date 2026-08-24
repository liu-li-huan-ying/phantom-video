#include "video/video_renderer.h"

#include <SDL_image.h>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ui/easing.h"
#include "ui/svgicon.h"
#include <windows.h>

static const unsigned char kDigitFont[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},  // :
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},  // .
    {0x11, 0x12, 0x04, 0x08, 0x10, 0x11, 0x12},  // x
};

static void drawFontGlyph(SDL_Renderer* r, int x, int y, int scale, const unsigned char* glyph) {
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (glyph[row] & (0x10 >> col)) {
                SDL_Rect px{ x + col * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(r, &px);
            }
        }
    }
}

static void drawFontText(SDL_Renderer* r, int x, int y, int scale, const char* text) {
    for (const char* p = text; *p; ++p) {
        const unsigned char* g = nullptr;
        if (*p >= '0' && *p <= '9') g = kDigitFont[*p - '0'];
        else if (*p == ':') g = kDigitFont[10];
        else if (*p == '.') g = kDigitFont[11];
        else if (*p == 'x') g = kDigitFont[12];
        else if (*p == ' ') { x += 3 * scale + 1; continue; }
        else if (*p == '/') { x += 3 * scale + 1; continue; }
        else continue;
        drawFontGlyph(r, x, y, scale, g);
        x += 6 * scale;
    }
}

static void formatTimeText(char* buf, size_t n, double sec) {
    int s = (int)(sec + 0.5);
    if (s < 0) s = 0;
    int h = s / 3600, m = (s % 3600) / 60, ss = s % 60;
    if (h > 0)
        std::snprintf(buf, n, "%d:%02d:%02d", h, m, ss);
    else
        std::snprintf(buf, n, "%02d:%02d", m, ss);
}

// ---- vector icon drawing ----

static void drawIconLegacy(SDL_Renderer* r, Icon icon, int cx, int cy, int size, int alpha) {
    SDL_SetRenderDrawColor(r, 255, 255, 255, alpha);
    float s = (float)size / 24.0f;
    auto px = [&](float x) { return (int)std::lround(cx + x * s); };
    auto py = [&](float y) { return (int)std::lround(cy + y * s); };

    switch (icon) {
    case Icon::Play: {
        SDL_RenderDrawLine(r, px(-6), py(-8), px(-6), py(8));
        SDL_RenderDrawLine(r, px(-6), py(-8), px(7), py(0));
        SDL_RenderDrawLine(r, px(-6), py(8), px(7), py(0));
        break;
    }
    case Icon::Pause: {
        SDL_Rect b1{ px(-6), py(-8), (int)(4 * s), (int)(16 * s) };
        SDL_Rect b2{ px(2), py(-8), (int)(4 * s), (int)(16 * s) };
        SDL_RenderFillRect(r, &b1);
        SDL_RenderFillRect(r, &b2);
        break;
    }
    case Icon::Prev: {
        // triangle pointing left, bar on the right
        SDL_RenderDrawLine(r, px(7), py(-8), px(7), py(8));
        SDL_RenderDrawLine(r, px(-6), py(0), px(7), py(-8));
        SDL_RenderDrawLine(r, px(-6), py(0), px(7), py(8));
        break;
    }
    case Icon::Next: {
        // bar on the left, triangle pointing right
        SDL_RenderDrawLine(r, px(-7), py(-8), px(-7), py(8));
        SDL_RenderDrawLine(r, px(6), py(0), px(-7), py(-8));
        SDL_RenderDrawLine(r, px(6), py(0), px(-7), py(8));
        break;
    }
    case Icon::Volume: {
        // speaker body
        SDL_Rect body{ px(-9), py(-4), (int)(4 * s), (int)(8 * s) };
        SDL_RenderFillRect(r, &body);
        SDL_RenderDrawLine(r, px(-5), py(-4), px(-5), py(4));
        SDL_RenderDrawLine(r, px(-5), py(-4), px(-2), py(-6));
        SDL_RenderDrawLine(r, px(-5), py(4), px(-2), py(6));
        SDL_RenderDrawLine(r, px(-2), py(-6), px(1), py(-6));
        SDL_RenderDrawLine(r, px(-2), py(6), px(1), py(6));
        // waves
        SDL_Point w1[] = { { px(3), py(-5) }, { px(6), py(-2) }, { px(6), py(2) }, { px(3), py(5) } };
        SDL_RenderDrawLines(r, w1, 4);
        SDL_Point w2[] = { { px(6), py(-7) }, { px(9), py(-3) }, { px(9), py(3) }, { px(6), py(7) } };
        SDL_RenderDrawLines(r, w2, 4);
        break;
    }
    case Icon::Mute: {
        SDL_Rect body{ px(-9), py(-4), (int)(4 * s), (int)(8 * s) };
        SDL_RenderFillRect(r, &body);
        SDL_RenderDrawLine(r, px(-5), py(-4), px(-5), py(4));
        SDL_RenderDrawLine(r, px(-5), py(-4), px(-2), py(-6));
        SDL_RenderDrawLine(r, px(-5), py(4), px(-2), py(6));
        SDL_RenderDrawLine(r, px(-2), py(-6), px(1), py(-6));
        SDL_RenderDrawLine(r, px(-2), py(6), px(1), py(6));
        SDL_RenderDrawLine(r, px(3), py(-5), px(9), py(5));
        SDL_RenderDrawLine(r, px(9), py(-5), px(3), py(5));
        break;
    }
    case Icon::Fullscreen:
    case Icon::ExitFullscreen: {
        SDL_Rect o{ px(-9), py(-9), (int)(18 * s), (int)(18 * s) };
        SDL_RenderDrawRect(r, &o);
        if (icon == Icon::Fullscreen) {
            // corners outward
            SDL_RenderDrawLine(r, px(-9), py(-5), px(-9), py(-9));
            SDL_RenderDrawLine(r, px(-9), py(-9), px(-5), py(-9));
            SDL_RenderDrawLine(r, px(5), py(-9), px(9), py(-9));
            SDL_RenderDrawLine(r, px(9), py(-9), px(9), py(-5));
            SDL_RenderDrawLine(r, px(9), py(5), px(9), py(9));
            SDL_RenderDrawLine(r, px(9), py(9), px(5), py(9));
            SDL_RenderDrawLine(r, px(-5), py(9), px(-9), py(9));
            SDL_RenderDrawLine(r, px(-9), py(9), px(-9), py(5));
        }
        break;
    }
    case Icon::Single: {
        // "1" in a box: play single track
        SDL_Rect box{ px(-8), py(-8), (int)(16 * s), (int)(16 * s) };
        SDL_RenderDrawRect(r, &box);
        SDL_RenderDrawLine(r, px(-2), py(-3), px(2), py(0));
        SDL_RenderDrawLine(r, px(-2), py(-3), px(-2), py(3));
        break;
    }
    case Icon::Loop: {
        // circular arrows
        for (int a = 0; a < 360; a += 6) {
            float rad = (float)a * 3.14159f / 180.0f;
            int ex = px((float)std::cos(rad) * 8);
            int ey = py((float)std::sin(rad) * 8);
            if (a > 120 && a < 240) continue;  // gap at top-left for arrow head
            SDL_RenderDrawPoint(r, ex, ey);
        }
        SDL_RenderDrawLine(r, px(-6), py(-7), px(-2), py(-6));
        SDL_RenderDrawLine(r, px(-6), py(-7), px(-5), py(-3));
        break;
    }
    case Icon::Shuffle: {
        SDL_RenderDrawLine(r, px(-8), py(-6), px(8), py(6));
        SDL_RenderDrawLine(r, px(-8), py(6), px(8), py(-6));
        SDL_RenderDrawLine(r, px(-8), py(-6), px(-5), py(-8));
        SDL_RenderDrawLine(r, px(-8), py(-6), px(-9), py(-3));
        SDL_RenderDrawLine(r, px(8), py(6), px(5), py(8));
        SDL_RenderDrawLine(r, px(8), py(6), px(9), py(3));
        SDL_RenderDrawLine(r, px(-8), py(6), px(-5), py(8));
        SDL_RenderDrawLine(r, px(-8), py(6), px(-9), py(3));
        SDL_RenderDrawLine(r, px(8), py(-6), px(5), py(-8));
        SDL_RenderDrawLine(r, px(8), py(-6), px(9), py(-3));
        break;
    }
    }
}

// M32a: 图标绘制统一入口 —— 优先使用效果图原始 SVG path 光栅化结果（1:1 形状），
// 未收录的图标（播放模式等）回退旧像素实现。
static void drawIcon(SDL_Renderer* r, Icon icon, int cx, int cy, int size, int alpha) {
    const char* id = nullptr;
    switch (icon) {
    case Icon::Play:          id = "play"; break;
    case Icon::Pause:         id = "pause"; break;
    case Icon::Prev:          id = "prev"; break;
    case Icon::Next:          id = "next"; break;
    case Icon::Volume:        id = "volume"; break;
    case Icon::Mute:          id = "mute"; break;
    case Icon::Fullscreen:    id = "full"; break;
    case Icon::ExitFullscreen:id = "exitfull"; break;
    default: break;
    }
    if (id) {
        svgicon::draw(r, id, cx, cy, size, 255, 255, 255, (Uint8)alpha);
        return;
    }
    drawIconLegacy(r, icon, cx, cy, size, alpha);
}

// ---- icon PNG textures (Material Icons, Apache 2.0) ----

static const char* kIconFile(Icon icon) {
    switch (icon) {
    case Icon::Play: return "play_arrow.png";
    case Icon::Pause: return "pause.png";
    case Icon::Prev: return "skip_previous.png";
    case Icon::Next: return "skip_next.png";
    case Icon::Volume: return "volume_up.png";
    case Icon::Mute: return "volume_off.png";
    case Icon::Fullscreen: return "fullscreen.png";
    case Icon::ExitFullscreen: return "fullscreen_exit.png";
    case Icon::Single: return "repeat_one.png";
    case Icon::Loop: return "repeat.png";
    case Icon::Shuffle: return "shuffle.png";
    }
    return nullptr;
}

void VideoRenderer::ensureIcon(Icon icon) {
    int idx = (int)icon;
    if (idx < 0 || idx >= 12 || iconTex_[idx]) return;
    const char* file = kIconFile(icon);
    if (!file || !renderer_) return;

    char dir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = std::strrchr(dir, '\\');
    if (!slash) return;
    *(slash + 1) = '\0';
    std::string path = std::string(dir) + "assets\\icons\\" + file;

    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    iconTex_[idx] = tex;
}

SDL_Texture* VideoRenderer::iconTexture(Icon icon) {
    ensureIcon(icon);
    return iconTex_[(int)icon];
}

static void drawIconOrTexture(SDL_Renderer* r, SDL_Texture* tex, Icon icon,
                              int cx, int cy, int size, int alpha) {
    if (tex) {
        SDL_SetTextureAlphaMod(tex, (Uint8)alpha);
        SDL_Rect dst{ cx - size / 2, cy - size / 2, size, size };
        SDL_RenderCopy(r, tex, nullptr, &dst);
    } else {
        drawIcon(r, icon, cx, cy, size, alpha);
    }
}

// ---- rounded rect via RenderGeometry (triangle fans) ----

static void addRoundedCorner(std::vector<SDL_Vertex>& v, float cx, float cy, float r,
                             float startA, float endA, SDL_Color c) {
    int seg = 8;
    for (int i = 0; i < seg; ++i) {
        float a1 = startA + (endA - startA) * i / seg;
        float a2 = startA + (endA - startA) * (i + 1) / seg;
        float x1 = cx + std::cos(a1) * r;
        float y1 = cy + std::sin(a1) * r;
        float x2 = cx + std::cos(a2) * r;
        float y2 = cy + std::sin(a2) * r;
        SDL_Vertex p1{ { x1, y1 }, c, { 0, 0 } };
        SDL_Vertex p2{ { x2, y2 }, c, { 0, 0 } };
        SDL_Vertex pc{ { cx, cy }, c, { 0, 0 } };
        v.push_back(p1);
        v.push_back(p2);
        v.push_back(pc);
    }
}

static void fillRoundedRect(SDL_Renderer* r, int x, int y, int w, int h, int rad,
                            Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (rad <= 0) {
        SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
        SDL_Rect rc{ x, y, w, h };
        SDL_RenderFillRect(r, &rc);
        return;
    }
    SDL_Color c{ cr, cg, cb, ca };
    std::vector<SDL_Vertex> v;
    SDL_Vertex a{ { (float)x + rad, (float)y }, c, { 0, 0 } };
    SDL_Vertex b{ { (float)x + w - rad, (float)y + h }, c, { 0, 0 } };
    SDL_Vertex cc{ { (float)x + w - rad, (float)y }, c, { 0, 0 } };
    SDL_Vertex d{ { (float)x + rad, (float)y + h }, c, { 0, 0 } };
    SDL_Vertex e{ { (float)x, (float)y + rad }, c, { 0, 0 } };
    SDL_Vertex f{ { (float)x + w, (float)y + h - rad }, c, { 0, 0 } };
    SDL_Vertex g{ { (float)x + w, (float)y + rad }, c, { 0, 0 } };
    SDL_Vertex h2{ { (float)x, (float)y + h - rad }, c, { 0, 0 } };
    // center quad
    v.push_back(a);
    v.push_back(b);
    v.push_back(cc);
    v.push_back(a);
    v.push_back(d);
    v.push_back(b);
    // side quads
    v.push_back(e);
    v.push_back(f);
    v.push_back(g);
    v.push_back(e);
    v.push_back(h2);
    v.push_back(f);
    // corners
    addRoundedCorner(v, (float)x + rad, (float)y + rad, (float)rad, (float)M_PI, (float)M_PI * 1.5f, c);
    addRoundedCorner(v, (float)x + w - rad, (float)y + rad, (float)rad, (float)M_PI * 1.5f, (float)M_PI * 2.0f, c);
    addRoundedCorner(v, (float)x + w - rad, (float)y + h - rad, (float)rad, 0.0f, (float)M_PI * 0.5f, c);
    addRoundedCorner(v, (float)x + rad, (float)y + h - rad, (float)rad, (float)M_PI * 0.5f, (float)M_PI, c);
    SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), nullptr, 0);
}

VideoRenderer::~VideoRenderer() { shutdown(); }

bool VideoRenderer::init(SDL_Window* window) {
    window_ = window;
    renderer_ = SDL_CreateRenderer(window, -1,
                                   SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        renderer_ = SDL_CreateRenderer(window, -1, 0);
        if (!renderer_) return false;
    }
    assRenderer_.init(renderer_);
    gdi_.init(renderer_);
    assRendererInit_ = true;
    return true;
}

void VideoRenderer::shutdown() {
    clearStyledSubtitle();
    gdi_.shutdown();
    svgicon::shutdown();
    assRenderer_.shutdown();
    assRendererInit_ = false;
    destroySubtitleTexture();
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (swsCtx_) {
        sws_freeContext((SwsContext*)swsCtx_);
        swsCtx_ = nullptr;
    }
    if (convFrame_) {
        av_frame_free((AVFrame**)&convFrame_);
        convFrame_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
}

void VideoRenderer::onMouseMove(int x, int y) {
    mouseX_ = x;
    mouseY_ = y;
    lastMouseMove_ = SDL_GetTicks();
    controlsVisible_ = true;
}

void VideoRenderer::showControls() {
    lastMouseMove_ = SDL_GetTicks();
    controlsVisible_ = true;
}

void VideoRenderer::showToast(const char* text) {
    if (!text) return;
    toastText_ = text;
    toastUntil_ = SDL_GetTicks() + 2200;
}

void VideoRenderer::toggleSpeedMenu() {
    speedMenuOpen_ = !speedMenuOpen_;
    showControls();
}

void VideoRenderer::setThumbnail(SDL_Texture* tex, int w, int h, double timeSec) {
    if (thumbTex_) { SDL_DestroyTexture(thumbTex_); thumbTex_ = nullptr; }
    thumbTex_ = tex;
    thumbW_ = w;
    thumbH_ = h;
    thumbTime_ = timeSec;
}

SDL_Rect VideoRenderer::speedMenuItemRect(const ControlLayout& lay, int index) {
    constexpr int itemH = 30;   // M32d: 12.5px 字 + 7px*2 内距 ≈ 30
    constexpr int menuW = 130;  // CSS min-width:130
    constexpr int count = 8;
    int menuX = lay.spdX - 10;
    int menuBottom = lay.row2Y + 34;
    int menuY = menuBottom - count * itemH - 12;  // 面板内距 6 + 边距
    return SDL_Rect{ menuX, menuY + index * itemH, menuW, itemH };
}

// ---- M32c: 顶部栏 ----
void VideoRenderer::topBarRects(int winW, SDL_Rect out[TB_COUNT]) {
    // winW 此处 = 播放器区宽度（不含列表）；左→右按枚举顺序；34px，间距2，右边距10
    int x = winW - 10 - 34 * TB_COUNT - 2 * (TB_COUNT - 1);
    for (int i = 0; i < TB_COUNT; ++i) {
        out[i] = SDL_Rect{ x, (TOPBAR_H - 34) / 2, 34, 34 };
        x += 34 + 2;
    }
}

int VideoRenderer::topBarClick(int mx, int my) {
    if (my < 0 || my >= TOPBAR_H) return -1;
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    int playerW = w - s_panelW_.load(std::memory_order_acquire);
    SDL_Rect rs[TB_COUNT];
    topBarRects(playerW, rs);
    for (int i = 0; i < TB_COUNT; ++i)
        if (mx >= rs[i].x && mx < rs[i].x + rs[i].w) return i;
    return -1;
}

void VideoRenderer::drawTopBar() {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    // M32f.2: 顶栏只覆盖播放器区（列表在其右侧独立存在）
    int playerW = w - s_panelW_.load(std::memory_order_acquire);
    SDL_Color cTop{ 0, 0, 0, 0 }, cBot{ 0, 0, 0, 140 };
    SDL_Vertex q[] = {
        { {0,0}, cTop,{} }, { {(float)playerW,0}, cTop,{} }, { {(float)playerW,(float)TOPBAR_H}, cBot,{} },
        { {0,0}, cTop,{} }, { {(float)playerW,(float)TOPBAR_H}, cBot,{} }, { {0,(float)TOPBAR_H}, cBot,{} },
    };
    SDL_RenderGeometry(renderer_, nullptr, q, 6, nullptr, 0);

    const char* title = SDL_GetWindowTitle(window_);
    if (title && *title) gdi_.drawText(16, (TOPBAR_H - 18) / 2 + 2, title, 14, 255, 255, 255);

    SDL_Rect rs[TB_COUNT];
    topBarRects(playerW, rs);
    // 视觉左→右顺序 = 枚举顺序；图标与动作一一对应
    const char* ids[TB_COUNT] = { "camera", "pip", "list", "minimize",
                                  (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) ? "exitfull" : "maximize",
                                  "close" };
    for (int i = 0; i < TB_COUNT; ++i) {
        bool hov = mouseX_ >= rs[i].x && mouseX_ < rs[i].x + rs[i].w &&
                   mouseY_ >= rs[i].y && mouseY_ < rs[i].y + rs[i].h;
        if (hov)
            fillRoundedRect(renderer_, rs[i].x, rs[i].y, rs[i].w, rs[i].h, 8,
                            255, 255, 255, 20);
        svgicon::draw(renderer_, ids[i],
                      rs[i].x + rs[i].w / 2, rs[i].y + rs[i].h / 2, 18,
                      0xd4, 0xd4, 0xd8, hov ? 255 : 212);
    }
}

// ---- M32e: 设置模态 ----
void VideoRenderer::drawSettings(const RenderStats& stats) {
    if (!settingsOpen_) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    // 遮罩
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 140);
    SDL_Rect full{ 0, 0, w, h };
    SDL_RenderFillRect(renderer_, &full);

    const int BW = 420;
    constexpr int PAD = 18;
    // 7 toggle rows(40*7) + speed seg(46) + subScale seg(46) + lang seg(46) + theme seg(46) + title(30) + pad+14
    const int H = 30 + 40 * 7 + 46 * 4 + PAD + 14;
    SDL_Rect box{ (w - BW) / 2, (h - H) / 2 - 10, BW, H };
    fillRoundedRect(renderer_, box.x - 4, box.y - 2, BW + 8, H + 6, 16,
                    0, 0, 0, (Uint8)(110));
    fillRoundedRect(renderer_, box.x, box.y, BW, H, 14,
                    0x14, 0x14, 0x16, 255);

    auto switchDraw = [&](int x, int y, bool on) {
        fillRoundedRect(renderer_, x, y, 38, 22, 11,
                        on ? 0x25 : 255, on ? 0x63 : 255, on ? 0xeb : 255,
                        on ? 255 : (Uint8)(41));
        fillRoundedRect(renderer_, x + (on ? 18 : 2), y + 2, 18, 18, 9,
                        255, 255, 255, 255);
    };

    int cy = box.y + PAD;
    gdi_.drawText(box.x + PAD, cy, "设置", 14, 255, 255, 255);
    setClose_ = SDL_Rect{ box.x + BW - PAD - 26, cy - 4, 26, 26 };
    svgicon::draw(renderer_, "close", setClose_.x + 13, setClose_.y + 13, 14,
                  0xd4, 0xd4, 0xd8, 212);
    cy += 34;

    const char* labels[7] = { "硬件解码（D3D11VA / DXVA2）", "音量标准化（EBU R128）",
                              "记忆播放位置", "自动播放下一个", "字幕自动加载",
                              "缩略图磁盘缓存", "起播同步优化" };
    bool vals[7] = { stats.swHw, stats.swNorm, stats.swResume, stats.swAutoNext,
                     stats.swSub, stats.swThumbCache, true };
    for (int i = 0; i < 7; ++i) {
        setRows_[i] = SDL_Rect{ box.x + PAD, cy, BW - PAD * 2, 40 };
        gdi_.drawText(box.x + PAD, cy + 11, labels[i], 12, 230, 230, 231);
        if (i == 6) {
            // 起播同步：只读指示，不可切换
            gdi_.drawText(box.x + BW - PAD - 40, cy + 12, "ON", 10, 0x25, 0x63, 0xeb);
        } else {
            switchDraw(box.x + BW - PAD - 38, cy + 7, vals[i]);
        }
        if (i < 6) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 26);
            SDL_Rect sep{ box.x + PAD, cy + 39, BW - PAD * 2, 1 };
            SDL_RenderFillRect(renderer_, &sep);
        }
        cy += 40;
    }

    // 播放速度 seg
    cy += 4;
    setSpeed_[0] = { box.x + PAD, cy, BW - PAD * 2, 38 };
    gdi_.drawText(box.x + PAD, cy + 11, "播放速度", 12, 230, 230, 231);
    {
        const char* spdLabels[6] = { "0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "2.0x" };
        const float spdVals[6] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
        int sx = box.x + BW - PAD - 6 * 48;
        for (int i = 0; i < 6; ++i) {
            setSpeed_[i] = SDL_Rect{ sx, cy + 4, 46, 24 };
            bool active = (std::abs(stats.speed - spdVals[i]) < 0.01f);
            if (active)
                fillRoundedRect(renderer_, sx + 2, cy + 6, 42, 20, 6,
                                255, 255, 255, 36);
            gdi_.drawText(sx + 8, cy + 9, spdLabels[i], 10,
                          active ? 255 : 161, active ? 255 : 161, active ? 255 : 166);
            sx += 48;
        }
    }
    cy += 46;

    // 字幕字号 seg
    setSubScale_[0] = { box.x + PAD, cy, BW - PAD * 2, 38 };
    gdi_.drawText(box.x + PAD, cy + 11, "字幕字号", 12, 230, 230, 231);
    {
        const char* szLabels[3] = { "小", "中", "大" };
        const float szVals[3] = { 0.75f, 1.0f, 1.5f };
        int sx = box.x + BW - PAD - 3 * 56;
        for (int i = 0; i < 3; ++i) {
            setSubScale_[i] = SDL_Rect{ sx, cy + 4, 54, 24 };
            bool active = (std::abs(stats.subScale - szVals[i]) < 0.01f);
            if (active)
                fillRoundedRect(renderer_, sx + 2, cy + 6, 50, 20, 6,
                                255, 255, 255, 36);
            gdi_.drawText(sx + 18, cy + 9, szLabels[i], 11,
                          active ? 255 : 161, active ? 255 : 161, active ? 255 : 166);
            sx += 56;
        }
    }
    cy += 46;

    // 界面语言 seg
    setLang_[0] = { box.x + PAD, cy, BW - PAD * 2, 38 };
    gdi_.drawText(box.x + PAD, cy + 11, "界面语言", 12, 230, 230, 231);
    {
        const char* langs[3] = { "中文", "English", "日本語" };
        int sx = box.x + BW - PAD - (58 + 66 + 56);
        for (int i = 0; i < 3; ++i) {
            setLang_[i] = SDL_Rect{ sx, cy + 4, i == 0 ? 58 : (i == 1 ? 66 : 56), 24 };
            if (stats.langIdx == i)
                fillRoundedRect(renderer_, sx + 2, cy + 6, setLang_[i].w - 4, 20, 6,
                                255, 255, 255, 36);
            gdi_.drawText(sx + 10, cy + 9, langs[i], 11,
                          stats.langIdx == i ? 255 : 161, stats.langIdx == i ? 255 : 161,
                          stats.langIdx == i ? 255 : 166);
            sx += setLang_[i].w;
        }
    }
    cy += 46;

    // 主题 seg
    setTheme_[0] = { box.x + PAD, cy, BW - PAD * 2, 38 };
    gdi_.drawText(box.x + PAD, cy + 11, "主题", 12, 230, 230, 231);
    {
        const char* ths[2] = { "深色", "浅色" };
        int sx = box.x + BW - PAD - 108;
        for (int i = 0; i < 2; ++i) {
            setTheme_[i] = SDL_Rect{ sx, cy + 4, 54, 24 };
            if (stats.themeIdx == i)
                fillRoundedRect(renderer_, sx + 2, cy + 6, 50, 20, 6,
                                255, 255, 255, 36);
            gdi_.drawText(sx + 12, cy + 9, ths[i], 11,
                          stats.themeIdx == i ? 255 : 161, stats.themeIdx == i ? 255 : 161,
                          stats.themeIdx == i ? 255 : 166);
            sx += 54;
        }
    }
}

int VideoRenderer::settingsClick(int mx, int my) {
    if (!settingsOpen_) return -1;
    for (int i = 0; i < 7; ++i)
        if (mx >= setRows_[i].x && mx < setRows_[i].x + setRows_[i].w &&
            my >= setRows_[i].y && my < setRows_[i].y + setRows_[i].h) return i;
    for (int i = 0; i < 6; ++i)
        if (setSpeed_[i].w && mx >= setSpeed_[i].x && mx < setSpeed_[i].x + setSpeed_[i].w &&
            my >= setSpeed_[i].y && my < setSpeed_[i].y + setSpeed_[i].h) return 30 + i;
    for (int i = 0; i < 3; ++i)
        if (setSubScale_[i].w && mx >= setSubScale_[i].x && mx < setSubScale_[i].x + setSubScale_[i].w &&
            my >= setSubScale_[i].y && my < setSubScale_[i].y + setSubScale_[i].h) return 40 + i;
    for (int i = 0; i < 3; ++i)
        if (setLang_[i].w && mx >= setLang_[i].x && mx < setLang_[i].x + setLang_[i].w &&
            my >= setLang_[i].y && my < setLang_[i].y + setLang_[i].h) return 10 + i;
    for (int i = 0; i < 2; ++i)
        if (setTheme_[i].w && mx >= setTheme_[i].x && mx < setTheme_[i].x + setTheme_[i].w &&
            my >= setTheme_[i].y && my < setTheme_[i].y + setTheme_[i].h) return 20 + i;
    if (mx >= setClose_.x && mx < setClose_.x + setClose_.w &&
        my >= setClose_.y && my < setClose_.y + setClose_.h) return -2;
    return -1;
}

void VideoRenderer::drawToast() {
    Uint32 now = SDL_GetTicks();
    if (now >= toastUntil_ || toastText_.empty()) return;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, toastText_.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return;
    std::wstring wtext(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, toastText_.c_str(), -1, wtext.data(), wlen);

    HDC mem = CreateCompatibleDC(nullptr);
    if (!mem) return;
    HFONT font = CreateFontW(-24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(mem, font);

    RECT rc{ 0, 0, 2000, 100 };
    DrawTextW(mem, wtext.c_str(), -1, &rc, DT_CALCRECT | DT_NOPREFIX | DT_CENTER);
    int tw = rc.right - rc.left + 32;
    int th = rc.bottom - rc.top + 20;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tw;
    bmi.bmiHeader.biHeight = -th;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SDL_Texture* tex = nullptr;
    if (hbmp && bits) {
        HGDIOBJ oldBmp = SelectObject(mem, hbmp);
        RECT trc{ 0, 0, tw, th };
        HBRUSH black = CreateSolidBrush(RGB(20,20,22)); // M32g: 效果图 toast 底色
        FillRect(mem, &trc, black);
        SetBkMode(mem, TRANSPARENT);
        RECT drc{ 15, 9, tw - 15, th - 9 };
        SetTextColor(mem, RGB(255, 255, 255));
        DrawTextW(mem, wtext.c_str(), -1, &drc, DT_NOPREFIX | DT_CENTER | DT_VCENTER);

        tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, tw, th);
        if (tex) {
            void* tbits = nullptr;
            int pitch = 0;
            if (SDL_LockTexture(tex, nullptr, &tbits, &pitch) == 0) {
                std::memcpy(tbits, bits, (size_t)tw * th * 4);
                SDL_UnlockTexture(tex);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            } else {
                SDL_DestroyTexture(tex);
                tex = nullptr;
            }
        }
        SelectObject(mem, oldBmp);
        DeleteObject(hbmp);
    }
    SelectObject(mem, oldFont);
    DeleteObject(font);
    DeleteDC(mem);
    if (!tex) return;

    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);
    Uint32 remain = toastUntil_ - now;
    int alpha = (remain < 500) ? (int)(remain * 255 / 500) : 255;
    SDL_SetTextureAlphaMod(tex, (Uint8)alpha);
    SDL_Rect dst{ (winW - tw) / 2, 60, tw, th };
    // M32g.2: 柔和阴影 + 浅色描边（效果图 .toast 边框白.10）
    fillRoundedRect(renderer_, dst.x - 2, dst.y + 2, dst.w + 4, dst.h + 4, 11,
                    0, 0, 0, (Uint8)(60 * alpha / 255));
    fillRoundedRect(renderer_, dst.x - 1, dst.y - 1, dst.w + 2, dst.h + 2, 10,
                    255, 255, 255, (Uint8)(26 * alpha / 255));
    SDL_RenderCopy(renderer_, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

void VideoRenderer::destroySubtitleTexture() {
    if (subtitleTexture_) {
        SDL_DestroyTexture((SDL_Texture*)subtitleTexture_);
        subtitleTexture_ = nullptr;
    }
    subtitleCache_.clear();
}

void VideoRenderer::drawSubtitle(const RenderStats& stats) {
    // Try styled ASS rendering first
    if (stats.rawSubtitle && *stats.rawSubtitle && assRendererInit_) {
        std::string raw(stats.rawSubtitle);
        if (raw != styledSubCache_) {
            assRenderer_.freeRendered(styledSub_);
            int winW = 0, winH = 0;
            SDL_GetWindowSize(window_, &winW, &winH);
            styledSub_ = assRenderer_.render(raw, winW, winH, 1920, 1080);
            styledSubCache_ = raw;
        }
        if (!styledSub_.textures.empty()) {
            for (size_t i = 0; i < styledSub_.textures.size(); ++i) {
                SDL_RenderCopy(renderer_, styledSub_.textures[i],
                               &styledSub_.srcRects[i], &styledSub_.dstRects[i]);
            }
            return;
        }
    }

    // Fallback: plain text rendering (existing Win32 GDI method)
    clearStyledSubtitle();
    if (!stats.subtitle || !*stats.subtitle) {
        destroySubtitleTexture();
        return;
    }
    std::string text = stats.subtitle;
    if (text != subtitleCache_) {
        destroySubtitleTexture();

        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return;
        std::wstring wtext(wlen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext.data(), wlen);

        HDC mem = CreateCompatibleDC(nullptr);
        if (!mem) return;
        HFONT font = CreateFontW(-32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        HGDIOBJ oldFont = SelectObject(mem, font);

        RECT rc{ 0, 0, 2000, 200 };
        DrawTextW(mem, wtext.c_str(), -1, &rc, DT_CALCRECT | DT_NOPREFIX | DT_CENTER | DT_WORDBREAK);
        int tw = rc.right - rc.left + 24;
        int th = rc.bottom - rc.top + 24;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = tw;
        bmi.bmiHeader.biHeight = -th;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hbmp && bits) {
            HGDIOBJ oldBmp = SelectObject(mem, hbmp);
            RECT trc{ 0, 0, tw, th };
            HBRUSH black = CreateSolidBrush(RGB(20,20,22)); // M32g: 效果图 toast 底色
            FillRect(mem, &trc, black);
            SetBkMode(mem, TRANSPARENT);
            RECT drc{ 12, 12, tw - 12, th - 12 };
            SetTextColor(mem, RGB(0, 0, 0));
            DrawTextW(mem, wtext.c_str(), -1, &drc, DT_NOPREFIX | DT_CENTER | DT_WORDBREAK);
            RECT drc2{ 10, 10, tw - 10, th - 10 };
            SetTextColor(mem, RGB(255, 255, 255));
            DrawTextW(mem, wtext.c_str(), -1, &drc2, DT_NOPREFIX | DT_CENTER | DT_WORDBREAK);

            SDL_Texture* tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                                 SDL_TEXTUREACCESS_STREAMING, tw, th);
            if (tex) {
                void* tbits = nullptr;
                int pitch = 0;
                if (SDL_LockTexture(tex, nullptr, &tbits, &pitch) == 0) {
                    std::memcpy(tbits, bits, (size_t)tw * th * 4);
                    SDL_UnlockTexture(tex);
                    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                    subtitleTexture_ = tex;
                    subTexW_ = tw;
                    subTexH_ = th;
                } else {
                    SDL_DestroyTexture(tex);
                }
            }
            SelectObject(mem, oldBmp);
            DeleteObject(hbmp);
        }
        SelectObject(mem, oldFont);
        DeleteObject(font);
        DeleteDC(mem);
        subtitleCache_ = text;
    }

    if (!subtitleTexture_) return;

    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);
    SDL_Rect dst{ (winW - subTexW_) / 2, winH - 60 - subTexH_ - 8, subTexW_, subTexH_ };
    SDL_RenderCopy(renderer_, (SDL_Texture*)subtitleTexture_, nullptr, &dst);
}

void VideoRenderer::setAssContent(const std::string& assContent) {
    if (assRendererInit_) {
        assRenderer_.loadStyles(assContent);
    }
}

void VideoRenderer::clearStyledSubtitle() {
    assRenderer_.freeRendered(styledSub_);
    styledSubCache_.clear();
}

ControlLayout ControlLayout::compute(int winW, int winH, int panelWidth) {
    ControlLayout l;
    const int mL = 16, mR = 16;
    int areaW = winW - panelWidth;
    // 单行布局：seek带18 + 行(42) + 底距16 ≈ 80
    l.top = winH - 80;
    l.progX = mL + 2;
    l.progW = areaW - mL - mR - 4;
    l.progY = l.top + 9;          // track 中心线
    l.row1Y = l.top + 24;         // 行顶（高42）
    l.row2Y = l.row1Y + 7;        // 文本钮命中带（高28）垂直居中于行
    constexpr int S = 34, P = 40, G = 6;
    // 左簇
    l.prevX = mL;
    l.playX = l.prevX + S + G;
    l.nextX = l.playX + P + G;
    l.timeX = l.nextX + S + 18;
    l.timeY = l.row1Y + 13;
    // 右簇（右对齐，向左排）
    int x = areaW - mR;
    l.fs2X  = x -= S;            x -= S + 10;
    l.setX   = x -= 44;          x -= 44 + 12;
    l.volSlW = 70;
    l.volSlX = x -= l.volSlW;    x -= l.volSlW + 2;
    l.volBxX = x -= S;           x -= S + 12;
    l.qualX  = x -= 48;          x -= 48 + 12;
    l.spdX   = x -= 84;          x -= 84 + 12;
    l.subX   = x -= 44;
    // legacy 别名
    l.barY = l.top; l.btnY = l.row1Y; l.btnSize = P;
    l.modeX = l.subX; l.speedX = l.spdX; l.volX = l.volBxX; l.fsX = l.fs2X;
    return l;
}

void VideoRenderer::drawControls(const RenderStats& stats) {
    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);
    ControlLayout lay = ControlLayout::compute(winW, winH, panelWidth_);
    const int h = 64;
    const int barY = lay.barY;

    Uint32 now = SDL_GetTicks();
    if (now - lastMouseMove_ > 700 && !stats.draggingVolume) {
        controlsVisible_ = false;
    }

    // M30a: 控件 alpha 缓动动画
    bool controlsHover = mouseY_ >= lay.barY && mouseY_ < winH;
    float targetAlpha = controlsVisible_ ? 1.0f : 0.0f;
    if (targetAlpha != animControlsTo_) {
        animControlsFrom_ = animControlsAlpha_;
        animControlsTo_ = targetAlpha;
        animControlsStart_ = now;
    }
    {
        float t = (now - animControlsStart_) / (float)kControlsFadeMs;
        t = std::min(t, 1.0f);
        float eased = easeOutCubic(t);
        animControlsAlpha_ = lerpf(animControlsFrom_, animControlsTo_, eased);
    }
    int a = (int)(animControlsAlpha_ * 255);
    if (a <= 0) return;

    // ---- background: M32b 底栏渐变（CSS: .62 底 → .25@75% → 0 顶）----
    {
        const float yTop = (float)lay.top, yBot = (float)winH;
        float y75 = yTop + (yBot - yTop) * 0.75f;
        SDL_Color c0{ 0,0,0, 0 }, c25{ 0,0,0,(Uint8)(64*a/255) }, c62{ 0,0,0,(Uint8)(158*a/255) };
        SDL_Vertex q[] = {
            { {0,yTop}, c0,{} }, { {(float)winW,yTop}, c0,{} }, { {(float)winW,y75}, c25,{} },
            { {0,yTop}, c0,{} }, { {(float)winW,y75}, c25,{} }, { {0,y75}, c25,{} },
            { {0,y75}, c25,{} }, { {(float)winW,y75}, c25,{} }, { {(float)winW,yBot}, c62,{} },
            { {0,y75}, c25,{} }, { {(float)winW,yBot}, c62,{} }, { {0,yBot}, c62,{} },
        };
        SDL_RenderGeometry(renderer_, nullptr, q, 12, nullptr, 0);
    }

    // ---- layout ----
    struct Btn { int x, y, w, h; };
    Btn prevBtn{ lay.prevX, lay.row1Y, 34, 34 };
    Btn playBtn{ lay.playX, lay.row1Y, 42, 42 };
    Btn nextBtn{ lay.nextX, lay.row1Y, 34, 34 };

    auto hit = [&](const Btn& b) {
        return mouseX_ >= b.x && mouseX_ < b.x + b.w && mouseY_ >= b.y && mouseY_ < b.y + b.h;
    };

    auto drawBtn = [&](const Btn& b, Icon icon, bool hover, bool active) {
        if (hover || active) {
            Uint8 bgA = (Uint8)(20 * a / 255);
            fillRoundedRect(renderer_, b.x, b.y, b.w, b.h, 8, 255, 255, 255, bgA);
        }
        int iconSize = (int)(20 * (hover ? 1.08f : 1.0f));
        int iconAlpha = (int)(228 * a / 255);
        drawIconOrTexture(renderer_, iconTexture(icon), icon,
                          b.x + b.w / 2, b.y + b.h / 2, iconSize, iconAlpha);
    };
    // M32b.2: 播放钮 —— 无底色，与其他按钮同排，白色图标稍大
    auto drawPlayBtn = [&](const Btn& b, bool hover) {
        if (hover)
            fillRoundedRect(renderer_, b.x, b.y, b.w, b.h, 8, 255, 255, 255,
                            (Uint8)(20 * a / 255));
        svgicon::draw(renderer_, stats.playing && !stats.paused ? "pause" : "play",
                      b.x + b.w / 2, b.y + b.h / 2, 22, 255, 255, 255, (Uint8)a);
    };

    // ---- progress bar: full-width slim bar at the very bottom ----
    const int progX = lay.progX;
    const int progY = lay.progY;
    const int progressW = lay.progW;

    double pct = (stats.duration > 0) ? (stats.uiClock / stats.duration) : 0;
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;

    bool progHover = mouseX_ >= progX && mouseX_ < progX + progressW &&
                     mouseY_ >= progY - 10 && mouseY_ < progY + 12;

    // M30a: 进度条展开/收起缓动动画
    float targetTrackH = (progHover || stats.draggingVolume) ? 6.0f : 4.0f;
    if (targetTrackH != animTrackTo_) {
        animTrackFrom_ = animTrackH_;
        animTrackTo_ = targetTrackH;
        animTrackStart_ = now;
    }
    {
        float t = (now - animTrackStart_) / (float)kTrackExpandMs;
        t = std::min(t, 1.0f);
        float eased = easeOutCubic(t);
        animTrackH_ = lerpf(animTrackFrom_, animTrackTo_, eased);
    }
    int trackH = (int)animTrackH_;
    int trackY = progY - trackH / 2;

    // track 背景（CSS rgba(255,255,255,.18)）+ buffered(.30) + fill(accent)
    fillRoundedRect(renderer_, progX, trackY, progressW, trackH, 3,
                    255, 255, 255, (Uint8)(46 * a / 255));
    int bufW = (int)(progressW * std::max(0.0f, std::min(1.0f, stats.bufferPct)));
    if (bufW > 0) {
        fillRoundedRect(renderer_, progX, trackY, bufW, trackH, 3,
                        255, 255, 255, (Uint8)(77 * a / 255));
    }
    int fillW = (int)(pct * progressW);
    if (fillW > 0) {
        fillRoundedRect(renderer_, progX, trackY, fillW, trackH, 3,
                        0x25, 0x63, 0xeb, (Uint8)(255 * a / 255));
    }
    // hover/drag 时发光 thumb + 阴影
    if (progHover || stats.draggingVolume) {
        // M30a: thumb 缩放缓动动画
        float targetThumbScale = 1.15f;
        if (targetThumbScale != animThumbTo_) {
            animThumbFrom_ = animThumbScale_;
            animThumbTo_ = targetThumbScale;
            animThumbStart_ = now;
        }
        {
            float t = (now - animThumbStart_) / (float)kThumbHoverMs;
            t = std::min(t, 1.0f);
            float eased = easeOutBack(t);
            animThumbScale_ = lerpf(animThumbFrom_, animThumbTo_, eased);
        }

        int thumbD = (int)(13 * animThumbScale_);
        int thumbCX = progX + fillW;
        int thumbCY = trackY + trackH / 2;
        int thumbXDraw = thumbCX - thumbD / 2;
        int thumbYDraw = thumbCY - thumbD / 2;
        fillRoundedRect(renderer_, thumbXDraw - 3, thumbYDraw - 3, thumbD + 6, thumbD + 6, 8,
                        0, 0, 0, (Uint8)(70 * a / 255));
        fillRoundedRect(renderer_, thumbXDraw, thumbYDraw, thumbD, thumbD, 6,
                        255, 255, 255, (Uint8)(255 * a / 255));

        // M15: 缩略图预览（在进度条上方，居中于鼠标位置）
        if (thumbTex_ && thumbW_ > 0 && thumbH_ > 0) {
            int maxThumbW = 160;
            int maxThumbH = 90;
            float scale = std::min((float)maxThumbW / thumbW_, (float)maxThumbH / thumbH_);
            int dispW = (int)(thumbW_ * scale);
            int dispH = (int)(thumbH_ * scale);
            int thumbCenterX = mouseX_;  // 居中于鼠标 X 坐标
            int thumbX2 = thumbCenterX - dispW / 2;
            int thumbY2 = trackY - dispH - 12;
            // 边界约束
            if (thumbX2 < 4) thumbX2 = 4;
            if (thumbX2 + dispW > winW - 4) thumbX2 = winW - dispW - 4;
            if (thumbY2 < 36) thumbY2 = 36;  // 标题栏高度 + 间距

            // 白色边框 + 黑色背景
            fillRoundedRect(renderer_, thumbX2 - 2, thumbY2 - 2, dispW + 4, dispH + 4, 4,
                            40, 40, 40, (Uint8)(240 * a / 255));
            SDL_SetTextureAlphaMod(thumbTex_, (Uint8)a);
            SDL_Rect dst{ thumbX2, thumbY2, dispW, dispH };
            SDL_RenderCopy(renderer_, thumbTex_, nullptr, &dst);

            // 缩略图下方显示时间戳
            if (thumbTime_ >= 0.0) {
                char timeBuf[16];
                formatTimeText(timeBuf, sizeof(timeBuf), thumbTime_);
                int textW = 0;
                for (const char* p = timeBuf; *p; ++p) textW += 6 * 2;
                int textX = thumbCenterX - textW / 2;
                int textY = thumbY2 + dispH + 4;
                // 黑色背景条
                fillRoundedRect(renderer_, textX - 4, textY - 1, textW + 8, 14, 3,
                                0, 0, 0, (Uint8)(200 * a / 255));
                SDL_SetRenderDrawColor(renderer_, 255, 255, 255, (Uint8)(255 * a / 255));
                drawFontText(renderer_, textX, textY, 2, timeBuf);
            }
        }
    }

    // M32b: 时间文本（行1，跟在 next 后，灰 #a1a1a6，tabular）
    {
        char curText[16], durText[16];
        formatTimeText(curText, sizeof(curText), stats.clock);
        formatTimeText(durText, sizeof(durText), stats.duration);
        char timeText[40];
        std::snprintf(timeText, sizeof(timeText), "%s / %s", curText, durText);
        gdi_.drawText(lay.timeX, lay.timeY, timeText, 12,
                      0xa1, 0xa1, 0xa6);
    }

    // ---- M32b 行2：字幕 / 倍速 / 画质 / 音量 / 设置 / 全屏（左对齐文本钮）----
    auto rowText = [&](int x, const char* txt, bool hover,
                       Uint8 r, Uint8 g, Uint8 b) {
        if (hover)
            fillRoundedRect(renderer_, x - 9, lay.row2Y, (int)std::strlen(txt) * 13 + 18, 28, 8,
                            255, 255, 255, (Uint8)(20 * a / 255));
        gdi_.drawText(x, lay.row2Y + 6, txt, 12, r, g, b);
    };
    // 字幕
    rowText(lay.subX, "字幕", hit(prevBtn) && false ||
            (mouseX_ >= lay.subX && mouseX_ < lay.subX + 44 &&
             mouseY_ >= lay.row2Y && mouseY_ < lay.row2Y + 28),
            0xe4, 0xe4, 0xe7);
    // 倍速 + 蓝色数值标签（accent2 #3b82f6）
    {
        char spdTxt[16];
        std::snprintf(spdTxt, sizeof(spdTxt), "%.2gx", stats.speed);
        std::string label = std::string("倍速 ") + spdTxt;
        bool hov = mouseX_ >= lay.spdX && mouseX_ < lay.spdX + 76 &&
                   mouseY_ >= lay.row2Y && mouseY_ < lay.row2Y + 28;
        rowText(lay.spdX, label.c_str(), hov, 0xe4, 0xe4, 0xe7);
        gdi_.drawText(lay.spdX + 40, lay.row2Y + 6, spdTxt, 12, 0x3b, 0x82, 0xf6);
    }
    // 画质 / 设置（本地播放占位）
    rowText(lay.qualX, "画质",
            mouseX_ >= lay.qualX && mouseX_ < lay.qualX + 44 &&
            mouseY_ >= lay.row2Y && mouseY_ < lay.row2Y + 28,
            0xa1, 0xa1, 0xa6);
    drawBtn(Btn{ lay.volBxX, lay.row1Y + 4, 34, 34 },
            stats.muted ? Icon::Mute : Icon::Volume,
            mouseX_ >= lay.volBxX && mouseX_ < lay.volBxX + 34 &&
            mouseY_ >= lay.row1Y + 4 && mouseY_ < lay.row1Y + 38, false);
    rowText(lay.setX, "设置",
            mouseX_ >= lay.setX && mouseX_ < lay.setX + 44 &&
            mouseY_ >= lay.row2Y && mouseY_ < lay.row2Y + 28,
            0xe4, 0xe4, 0xe7);
    drawBtn(Btn{ lay.fs2X, lay.row1Y + 4, 34, 34 },
            stats.fullscreen ? Icon::ExitFullscreen : Icon::Fullscreen,
            mouseX_ >= lay.fs2X && mouseX_ < lay.fs2X + 34 &&
            mouseY_ >= lay.row1Y + 4 && mouseY_ < lay.row1Y + 38, false);

    // 行1 传输钮（最后绘制保证在上层）
    drawBtn(prevBtn, Icon::Prev, hit(prevBtn), false);
    drawPlayBtn(playBtn, hit(playBtn));
    drawBtn(nextBtn, Icon::Next, hit(nextBtn), false);

    // M32b: 音量横条（volwrap hover 或拖动时显示在音量钮右侧）
    {
        Btn volWrap{ lay.volBxX, lay.row1Y + 4, 34 + 6 + lay.volSlW, 34 };
        bool volHover = hit(volWrap);
        if (volHover || stats.draggingVolume) {
            int slx = lay.volSlX, sly = lay.row1Y + 21;
            fillRoundedRect(renderer_, slx, sly - 2, lay.volSlW, 4, 2,
                            255, 255, 255, (Uint8)(51 * a / 255));
            float v = stats.muted ? 0.f : stats.volume;
            int fw = (int)(lay.volSlW * v);
            if (fw > 0)
                fillRoundedRect(renderer_, slx, sly - 2, fw, 4, 2,
                                255, 255, 255, (Uint8)(255 * a / 255));
            fillRoundedRect(renderer_, slx + fw - 5, sly - 5, 10, 10, 5,
                            255, 255, 255, (Uint8)(255 * a / 255));
        }
    }

    // ---- speed menu (popup above 倍速 button) ----
    if (speedMenuOpen_) {
        // M32d: 按效果图精修 —— 面板 rgba(24,24,26,.98) r10、阴影、
        // 选中项 accent2 蓝字(非填充)、hover 行白.08、右对齐灰色提示
        constexpr int count = 8;
        const float speeds[count] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f };
        const char* hints[count] = { "", "慢", "", "正常", "", "", "快", "" };
        int menuX = lay.spdX - 10, menuW = 130, itemH = 30;
        int menuBottom = lay.row2Y + 34 - 12;
        int menuY = menuBottom - count * itemH - 12;   // 与 speedMenuItemRect 一致
        // 阴影（近似）+ 面板 + 1px 边框
        fillRoundedRect(renderer_, menuX - 1, menuY + 3, menuW + 10, count * itemH + 14, 12,
                        0, 0, 0, (Uint8)(70 * a / 255));
        fillRoundedRect(renderer_, menuX - 6, menuY - 6, menuW + 12, count * itemH + 12, 10,
                        24, 24, 26, (Uint8)(250 * a / 255));
        for (int i = 0; i < count; ++i) {
            SDL_Rect r = speedMenuItemRect(lay, i);
            bool cur = std::abs(stats.speed - speeds[i]) < 0.001f;
            bool hov = mouseX_ >= r.x && mouseX_ < r.x + r.w &&
                       mouseY_ >= r.y && mouseY_ < r.y + r.h;
            if (hov)
                fillRoundedRect(renderer_, r.x + 4, r.y + 1, r.w - 8, r.h - 2, 7,
                                255, 255, 255, (Uint8)(20 * a / 255));
            char txt[16];
            std::snprintf(txt, sizeof(txt), "%.2gx", speeds[i]);
            if (cur) gdi_.drawText(r.x + 12, r.y + 7, txt, 12,
                                   0x3b, 0x82, 0xf6);
            else     gdi_.drawText(r.x + 12, r.y + 7, txt, 12,
                                   hov ? 255 : 0xe4, hov ? 255 : 0xe4, hov ? 255 : 0xe7);
            if (hints[i][0])
                gdi_.drawText(r.x + r.w - 34, r.y + 8, hints[i], 11,
                              0xa1, 0xa1, 0xa6);
        }
    }
}

void VideoRenderer::drawBackground() {
    if (!renderer_) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    const int titleH = 0;  // 标题栏高度
    int areaW = w - panelWidth_;  // 减去面板宽度
    // 视频区域纯黑 (0,0,0)，与标题栏 (30,30,30) 形成层次感
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_Rect videoArea{ 0, titleH, areaW, h - titleH };
    SDL_RenderFillRect(renderer_, &videoArea);
}

void VideoRenderer::render(const AVFrame* frame, const RenderStats& stats) {
    if (!renderer_ || !frame) return;

    // M32c: 截图请求 —— 用 sws 把当前帧转 RGB24 存 PNG
    if (!pendingShotPath_.empty()) {
        int dstW = frame->width, dstH = frame->height;
        SwsContext* sc = sws_getContext(dstW, dstH, (AVPixelFormat)frame->format,
                                        dstW, dstH, AV_PIX_FMT_RGB24,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sc) {
            AVFrame* rgb = av_frame_alloc();
            rgb->format = AV_PIX_FMT_RGB24;
            rgb->width = dstW; rgb->height = dstH;
            av_frame_get_buffer(rgb, 1);
            sws_scale(sc, frame->data, frame->linesize, 0, dstH,
                      rgb->data, rgb->linesize);
            SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
                rgb->data[0], dstW, dstH, 24, rgb->linesize[0], SDL_PIXELFORMAT_RGB24);
            if (surf) {
                IMG_SavePNG(surf, pendingShotPath_.c_str());
                SDL_FreeSurface(surf);
            }
            av_frame_free(&rgb);
            sws_freeContext(sc);
        }
        pendingShotPath_.clear();
    }

    int fmt = frame->format;
    if (frame->width != fw_ || frame->height != fh_ || fmt != pixFmt_) {
        if (texture_) SDL_DestroyTexture(texture_);
        fw_ = frame->width;
        fh_ = frame->height;
        pixFmt_ = fmt;
        Uint32 pf = SDL_PIXELFORMAT_IYUV;
        if (fmt == AV_PIX_FMT_NV12) pf = SDL_PIXELFORMAT_NV12;
        texture_ = SDL_CreateTexture(renderer_, pf,
                                     SDL_TEXTUREACCESS_STREAMING, fw_, fh_);
        if (!texture_) return;
    }

    const AVFrame* up = frame;
    AVFrame* conv = nullptr;
    if (fmt == AV_PIX_FMT_YUV420P) {
        SDL_UpdateYUVTexture(texture_, nullptr,
                             frame->data[0], frame->linesize[0],
                             frame->data[1], frame->linesize[1],
                             frame->data[2], frame->linesize[2]);
    } else if (fmt == AV_PIX_FMT_NV12) {
        SDL_UpdateNVTexture(texture_, nullptr,
                            frame->data[0], frame->linesize[0],
                            frame->data[1], frame->linesize[1]);
    } else {
        // fallback: convert to YUV420P with swscale
        if (!swsCtx_ || convW_ != fw_ || convH_ != fh_ || convSrcFmt_ != fmt) {
            if (swsCtx_) sws_freeContext((SwsContext*)swsCtx_);
            if (convFrame_) av_frame_free((AVFrame**)&convFrame_);
            swsCtx_ = sws_getContext(fw_, fh_, (AVPixelFormat)fmt,
                                     fw_, fh_, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
            convFrame_ = av_frame_alloc();
            if (convFrame_) {
                ((AVFrame*)convFrame_)->format = AV_PIX_FMT_YUV420P;
                ((AVFrame*)convFrame_)->width = fw_;
                ((AVFrame*)convFrame_)->height = fh_;
                av_frame_get_buffer((AVFrame*)convFrame_, 32);
            }
            convW_ = fw_;
            convH_ = fh_;
            convSrcFmt_ = fmt;
        }
        if (swsCtx_ && convFrame_) {
            sws_scale((SwsContext*)swsCtx_, frame->data, frame->linesize, 0, fh_,
                      ((AVFrame*)convFrame_)->data, ((AVFrame*)convFrame_)->linesize);
            conv = (AVFrame*)convFrame_;
        }
        if (conv) {
            SDL_UpdateYUVTexture(texture_, nullptr,
                                 conv->data[0], conv->linesize[0],
                                 conv->data[1], conv->linesize[1],
                                 conv->data[2], conv->linesize[2]);
        }
    }

    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);
    const int titleH = 0;  // 标题栏高度
    int areaY = titleH;     // 视频区域从标题栏下方开始
    int areaW = winW - panelWidth_;  // 减去播放列表面板宽度
    int areaH = winH - titleH;
    float scale = std::min((float)areaW / fw_, (float)areaH / fh_);
    SDL_Rect dst;
    dst.w = (int)(fw_ * scale);
    dst.h = (int)(fh_ * scale);
    dst.x = (areaW - dst.w) / 2;
    dst.y = areaY + (areaH - dst.h) / 2;

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    drawBackground();
    SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
    // M32g: 扫描线质感（CSS repeating-linear-gradient 3px 周期 白1.2%）
    {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 8);
        for (int y = dst.y; y < dst.y + dst.h; y += 3) {
            SDL_Rect ln{ dst.x, y, dst.w, 1 };
            SDL_RenderFillRect(renderer_, &ln);
        }
    }
    drawSubtitle(stats);
    drawToast();
    drawControls(stats);
    drawTopBar();
    drawPauseOverlay(stats);
    drawSeekingOverlay(stats);
    drawSettings(stats);
}

void VideoRenderer::showPauseOverlay(PauseIcon icon) {
    pauseOverlayIcon_ = icon;
    pauseOverlayAlpha_ = 1;
    pauseOverlayUntil_ = SDL_GetTicks() + 2000;
}

void VideoRenderer::drawPauseOverlay(const RenderStats& stats) {
    if (!renderer_) return;

    // M32g.2: 中央按钮由暂停状态直接驱动（此前无任何触发源，从未显示）
    if (stats.paused) {
        pauseOverlayAlpha_ = std::min(200, pauseOverlayAlpha_ + 25);
    } else {
        pauseOverlayAlpha_ = std::max(0, pauseOverlayAlpha_ - 12);
    }
    if (pauseOverlayAlpha_ <= 0) return;

    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    // 暂停时全屏轻微压暗（用户确认保留的视觉效果）
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, (Uint8)(pauseOverlayAlpha_ * 0.35f));
    SDL_Rect overlay{ 0, 0, winW - panelWidth_, winH };
    SDL_RenderFillRect(renderer_, &overlay);

    int cx = (winW - panelWidth_) / 2;
    int cy = winH / 2;
    Uint8 a = (Uint8)pauseOverlayAlpha_;

    // M32g: 效果图中央播放钮 —— 72px 圆、黑.55底、白.75描边
    {
        const int D = 72;
        fillRoundedRect(renderer_, cx - D/2 - 2, cy - D/2 - 2, D + 4, D + 4, D/2 + 2,
               255, 255, 255, (Uint8)(a * 0.75f / 255));
        fillRoundedRect(renderer_, cx - D/2, cy - D/2, D, D, D/2,
               0, 0, 0, (Uint8)(a * 0.55f / 255));
        svgicon::draw(renderer_,
                      pauseOverlayIcon_ == PauseIcon::Play ? "play" : "pause",
                      cx, cy, 30, 255, 255, 255, a);
        return;
    }

    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, a);

    if (pauseOverlayIcon_ == PauseIcon::Play) {
        // 大三角形播放图标 ▶（指向右）
        int size = 70;
        SDL_Point tri[3];
        tri[0] = { cx - size / 3, cy - size / 2 };  // 左上
        tri[1] = { cx - size / 3, cy + size / 2 };  // 左下
        tri[2] = { cx + size / 2, cy };               // 右中
        // 用多边形填充（SDL 没有直接填充三角形，用水平线扫描）
        for (int y = -size / 2; y <= size / 2; ++y) {
            float t = (float)(y + size / 2) / size;
            int leftX = cx - size / 3;
            int rightX;
            if (t < 0.5f) {
                // 上半部分：左边界固定，右边界从 leftX 线性到 cx+size/2
                rightX = leftX + (int)((cx + size / 2 - leftX) * (t / 0.5f));
            } else {
                // 下半部分：左边界固定，右边界从 cx+size/2 线性回到 leftX
                rightX = cx + size / 2 - (int)((cx + size / 2 - leftX) * ((t - 0.5f) / 0.5f));
            }
            SDL_RenderDrawLine(renderer_, leftX, cy + y, rightX, cy + y);
        }
    } else {
        // 双竖线暂停图标 ||
        int barW = 24;
        int barH = 80;
        int gap = 20;
        SDL_Rect bar1{ cx - gap - barW, cy - barH / 2, barW, barH };
        SDL_Rect bar2{ cx + gap, cy - barH / 2, barW, barH };
        SDL_RenderFillRect(renderer_, &bar1);
        SDL_RenderFillRect(renderer_, &bar2);
    }
}

void VideoRenderer::clear() {
    destroySubtitleTexture();
    if (renderer_) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        drawBackground();
    }
}

// ---- M33j: 欢迎页面 ----
void VideoRenderer::drawWelcome(const std::vector<std::string>& historyNames) {
    if (!renderer_) return;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);

    // 背景 #0b0b0b
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0x0b, 0x0b, 0x0b, 255);
    SDL_Rect full{ 0, 0, winW, winH };
    SDL_RenderFillRect(renderer_, &full);

    // 顶栏渐变
    SDL_Color cTop{ 0,0,0,0 }, cBot{ 0,0,0,140 };
    int playerW = winW - panelWidth_;
    SDL_Vertex grad[] = {
        { {0,0}, cTop,{} }, { {(float)playerW,0}, cTop,{} },
        { {(float)playerW,(float)TOPBAR_H}, cBot,{} },
        { {0,0}, cTop,{} }, { {(float)playerW,(float)TOPBAR_H}, cBot,{} },
        { {0,(float)TOPBAR_H}, cBot,{} },
    };
    SDL_RenderGeometry(renderer_, nullptr, grad, 6, nullptr, 0);
    gdi_.drawText(16, (TOPBAR_H - 18) / 2 + 2, "VPlayer", 14, 255, 255, 255);

    // 居中容器
    const int CW = std::min(720, playerW - 40);
    int cx = (playerW - CW) / 2;
    int cy = 80;

    // Logo
    svgicon::draw(renderer_, "play", cx + CW / 2, cy + 18, 32, 0x25, 0x63, 0xeb, 230);
    cy += 50;
    gdi_.drawText(cx + CW / 2 - 42, cy, "VPlayer", 20, 255, 255, 255);
    cy += 32;

    // 副标题
    gdi_.drawText(cx + CW / 2 - 72, cy, "拖拽视频文件到此处播放", 12, 0xa1, 0xa1, 0xa6);
    cy += 36;

    // 拖拽区域
    int dzH = 90;
    SDL_Rect dzRect{ cx, cy, CW, dzH };
    Uint8 dzR = welcomeDropHover_ ? 0x25 : 50;
    Uint8 dzG = welcomeDropHover_ ? 0x63 : 50;
    Uint8 dzB = welcomeDropHover_ ? 0xeb : 50;
    fillRoundedRect(renderer_, dzRect.x, dzRect.y, dzRect.w, dzRect.h, 10,
                    dzR, dzG, dzB, welcomeDropHover_ ? 60 : 25);
    // 虚线边框（四边分别画）
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, dzR, dzG, dzB, welcomeDropHover_ ? 180 : 70);
    int dashLen = 6, gapLen = 6;
    // 上边
    for (int x = dzRect.x + 10; x < dzRect.x + dzRect.w - 10; x += dashLen + gapLen)
        SDL_RenderDrawLine(renderer_, x, dzRect.y + 1, std::min(x + dashLen, dzRect.x + dzRect.w - 10), dzRect.y + 1);
    // 下边
    for (int x = dzRect.x + 10; x < dzRect.x + dzRect.w - 10; x += dashLen + gapLen)
        SDL_RenderDrawLine(renderer_, x, dzRect.y + dzRect.h - 2, std::min(x + dashLen, dzRect.x + dzRect.w - 10), dzRect.y + dzRect.h - 2);
    // 左边
    for (int y = dzRect.y + 10; y < dzRect.y + dzRect.h - 10; y += dashLen + gapLen)
        SDL_RenderDrawLine(renderer_, dzRect.x + 1, y, dzRect.x + 1, std::min(y + dashLen, dzRect.y + dzRect.h - 10));
    // 右边
    for (int y = dzRect.y + 10; y < dzRect.y + dzRect.h - 10; y += dashLen + gapLen)
        SDL_RenderDrawLine(renderer_, dzRect.x + dzRect.w - 2, y, dzRect.x + dzRect.w - 2, std::min(y + dashLen, dzRect.y + dzRect.h - 10));
    // 中心内容
    svgicon::draw(renderer_, "play", cx + CW / 2, cy + dzH / 2 - 6, 18,
                  dzR, dzG, dzB, welcomeDropHover_ ? 220 : 100);
    gdi_.drawText(cx + CW / 2 - 42, cy + dzH / 2 + 10, "拖放视频文件", 11, dzR, dzG, dzB);

    cy += dzH + 18;

    // 按钮行
    bool fileHover = (mouseX_ >= welcomeOpenFile_.x && mouseX_ < welcomeOpenFile_.x + welcomeOpenFile_.w &&
                      mouseY_ >= welcomeOpenFile_.y && mouseY_ < welcomeOpenFile_.y + welcomeOpenFile_.h);
    bool folderHover = (mouseX_ >= welcomeOpenFolder_.x && mouseX_ < welcomeOpenFolder_.x + welcomeOpenFolder_.w &&
                        mouseY_ >= welcomeOpenFolder_.y && mouseY_ < welcomeOpenFolder_.y + welcomeOpenFolder_.h);
    const int btnW = 140, btnH = 36, btnGap = 16;
    int btnTotalW = btnW * 2 + btnGap;
    int btnX = cx + (CW - btnTotalW) / 2;
    // 打开文件
    welcomeOpenFile_ = SDL_Rect{ btnX, cy, btnW, btnH };
    Uint8 bfA = fileHover ? 240 : 200;
    fillRoundedRect(renderer_, btnX, cy, btnW, btnH, 8, 0x25, 0x63, 0xeb, bfA);
    gdi_.drawText(btnX + 34, cy + 10, "打开文件", 12, 255, 255, 255);
    svgicon::draw(renderer_, "list", btnX + 18, cy + 18, 12, 255, 255, 255, 220);
    // 打开文件夹
    btnX += btnW + btnGap;
    welcomeOpenFolder_ = SDL_Rect{ btnX, cy, btnW, btnH };
    Uint8 bgA = folderHover ? 40 : 20;
    fillRoundedRect(renderer_, btnX, cy, btnW, btnH, 8, 255, 255, 255, bgA);
    if (folderHover) {
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 60);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        // 简单 hover 边框
    }
    gdi_.drawText(btnX + 34, cy + 10, "打开文件夹", 12, 230, 230, 231);
    svgicon::draw(renderer_, "list", btnX + 18, cy + 18, 12, 230, 230, 231, 180);

    cy += btnH + 28;

    // 最近播放
    welcomeHistoryCount_ = (int)historyNames.size();
    if (welcomeHistoryCount_ > 8) welcomeHistoryCount_ = 8;
    if (welcomeHistoryCount_ > 0) {
        // 分割线
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 20);
        SDL_RenderDrawLine(renderer_, cx + 50, cy, cx + CW - 50, cy);
        cy += 12;
        int labelW = 5 * 12; // "最近播放" 5字 × 12px
        gdi_.drawText(cx + CW / 2 - labelW / 2, cy, "最近播放", 11, 0x71, 0x71, 0x7a);
        cy += 24;

        const int cols = 4;
        const int cardGap = 10;
        const int cardW = (CW - (cols - 1) * cardGap) / cols;
        const int cardH = 56;
        for (int i = 0; i < welcomeHistoryCount_; ++i) {
            int row = i / cols, col = i % cols;
            int cardX = cx + col * (cardW + cardGap);
            int cardY = cy + row * (cardH + cardGap);
            welcomeHistory_[i] = SDL_Rect{ cardX, cardY, cardW, cardH };
            bool cardHover = (mouseX_ >= cardX && mouseX_ < cardX + cardW &&
                              mouseY_ >= cardY && mouseY_ < cardY + cardH);
            Uint8 cardA = cardHover ? 22 : 10;
            fillRoundedRect(renderer_, cardX, cardY, cardW, cardH, 6, 255, 255, 255, cardA);
            svgicon::draw(renderer_, "play", cardX + 16, cardY + cardH / 2, 10,
                          cardHover ? 0xd4 : 0xa1, cardHover ? 0xd4 : 0xa1,
                          cardHover ? 0xd8 : 0xa6, cardHover ? 180 : 100);
            // 文件名截断
            const std::string& name = historyNames[i];
            std::string display = name;
            int maxChars = (cardW - 40) / 7;
            if ((int)display.size() > maxChars) display = display.substr(0, maxChars - 2) + "..";
            gdi_.drawText(cardX + 30, cardY + cardH / 2 - 6, display.c_str(), 10,
                          cardHover ? 255 : 0xd4, cardHover ? 255 : 0xd4,
                          cardHover ? 255 : 0xd8);
        }
        int rows = (welcomeHistoryCount_ + cols - 1) / cols;
        cy += rows * (cardH + cardGap) + 10;
    }

    // 底部快捷键
    cy = winH - 36;
    gdi_.drawText(cx + CW / 2 - 200, cy,
                  "Ctrl+O 打开    空格 播放/暂停    \xe2\x86\x90 \xe2\x86\x92 快进退    F 全屏    Tab 列表",
                  10, 0x51, 0x51, 0x5a);
}

int VideoRenderer::welcomeClick(int mx, int my) {
    if (mx >= welcomeOpenFile_.x && mx < welcomeOpenFile_.x + welcomeOpenFile_.w &&
        my >= welcomeOpenFile_.y && my < welcomeOpenFile_.y + welcomeOpenFile_.h)
        return 0;
    if (mx >= welcomeOpenFolder_.x && mx < welcomeOpenFolder_.x + welcomeOpenFolder_.w &&
        my >= welcomeOpenFolder_.y && my < welcomeOpenFolder_.y + welcomeOpenFolder_.h)
        return 1;
    for (int i = 0; i < welcomeHistoryCount_; ++i)
        if (mx >= welcomeHistory_[i].x && mx < welcomeHistory_[i].x + welcomeHistory_[i].w &&
            my >= welcomeHistory_[i].y && my < welcomeHistory_[i].y + welcomeHistory_[i].h)
            return 2 + i;
    return -1;
}

// M18-3: Seeking 指示器
void VideoRenderer::showSeekingOverlay() {
    seekingAlpha_ = 1;
}

void VideoRenderer::hideSeekingOverlay() {
    seekingAlpha_ = 0;
}

void VideoRenderer::drawSeekingOverlay(const RenderStats&) {
    if (!renderer_ || seekingAlpha_ <= 0) return;

    // 淡入
    if (seekingAlpha_ < 200) seekingAlpha_ = std::min(200, seekingAlpha_ + 25);

    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    // 半透明暗色遮罩
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, (Uint8)(seekingAlpha_ * 0.3f));
    SDL_Rect overlay{ 0, 32, winW - panelWidth_, winH - 32 };
    SDL_RenderFillRect(renderer_, &overlay);

    // "Seeking..." 文字（GDI 渲染）
    int cx = (winW - panelWidth_) / 2;
    int cy = winH / 2;
    const char* text = "Seeking...";
    // 使用 SDL_ttf 不可用，用简单矩形指示器代替
    int boxW = 200, boxH = 60;
    SDL_SetRenderDrawColor(renderer_, 30, 30, 30, (Uint8)(seekingAlpha_ * 0.9f));
    SDL_Rect box{ cx - boxW / 2, cy - boxH / 2, boxW, boxH };
    SDL_RenderFillRect(renderer_, &box);

    // 白色边框
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, (Uint8)(seekingAlpha_ * 0.8f));
    SDL_RenderDrawRect(renderer_, &box);

    // 三点动画（简单矩形表示）
    Uint32 now = SDL_GetTicks();
    int dotPhase = (now / 200) % 4;  // 0~3
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, (Uint8)(seekingAlpha_));
    for (int i = 0; i < 3; ++i) {
        if (i < dotPhase) {
            SDL_Rect dot{ cx - 30 + i * 30, cy - 6, 12, 12 };
            SDL_RenderFillRect(renderer_, &dot);
        }
    }
}
