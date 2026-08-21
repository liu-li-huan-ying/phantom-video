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

static void drawIcon(SDL_Renderer* r, Icon icon, int cx, int cy, int size, int alpha) {
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
    return true;
}

void VideoRenderer::shutdown() {
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
    int itemH = 28;
    int menuW = 92;
    int count = 8;
    int menuX = lay.speedX + lay.btnSize / 2 - menuW / 2;
    int menuBottom = lay.barY - 8;
    int menuY = menuBottom - count * itemH;
    return SDL_Rect{ menuX, menuY + index * itemH, menuW, itemH };
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
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
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
            HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
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

ControlLayout ControlLayout::compute(int winW, int winH, int panelWidth) {
    ControlLayout l;
    l.btnSize = 40;
    l.gap = 12;
    const int titleH = 32;  // 标题栏高度
    int areaW = winW - panelWidth;  // 减去播放列表面板宽度
    // 进度条贴底全宽（独立一行），控制按钮行在进度条上方
    l.progX = 8;
    l.progY = winH - 5;
    l.progW = areaW - 16;
    l.barY = winH - 5 - 4 - 64;  // 控制栏 64px 高，位于进度条上方
    l.btnY = l.barY + (64 - l.btnSize) / 2;
    // 左侧播放控制组
    l.prevX = 16;
    l.playX = l.prevX + l.btnSize + l.gap;
    l.nextX = l.playX + l.btnSize + l.gap;
    // 右侧功能组（右对齐到 areaW）
    l.fsX = areaW - 16 - l.btnSize;
    l.volX = l.fsX - l.btnSize - l.gap;
    l.speedX = l.volX - l.btnSize - l.gap;
    l.modeX = l.speedX - l.btnSize - l.gap;
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

    // fade in/out animation
    if (controlsVisible_) {
        controlsAlpha_ = std::min(255, controlsAlpha_ + 30);
    } else {
        controlsAlpha_ = std::max(0, controlsAlpha_ - 30);
    }
    if (controlsAlpha_ <= 0) return;
    int a = controlsAlpha_;

    // ---- background: vertical gradient ----
    std::vector<SDL_Vertex> gv;
    SDL_Color cTop{ 0, 0, 0, (Uint8)(64 * a / 255) };
    SDL_Color cBot{ 0, 0, 0, (Uint8)(217 * a / 255) };
    SDL_Vertex vt1{ { 0, (float)barY }, cTop, { 0, 0 } };
    SDL_Vertex vt2{ { (float)winW, (float)barY }, cTop, { 0, 0 } };
    SDL_Vertex vt3{ { (float)winW, (float)winH }, cBot, { 0, 0 } };
    SDL_Vertex vt4{ { 0, (float)winH }, cBot, { 0, 0 } };
    gv.push_back(vt1);
    gv.push_back(vt2);
    gv.push_back(vt3);
    gv.push_back(vt1);
    gv.push_back(vt3);
    gv.push_back(vt4);
    SDL_RenderGeometry(renderer_, nullptr, gv.data(), (int)gv.size(), nullptr, 0);

    // ---- layout ----
    const int btnSize = lay.btnSize;
    const int btnY = lay.btnY;
    const int gap = lay.gap;

    struct Btn { int x, y, w, h; };
    Btn prevBtn{ lay.prevX, btnY, btnSize, btnSize };
    Btn playBtn{ lay.playX, btnY, btnSize, btnSize };
    Btn nextBtn{ lay.nextX, btnY, btnSize, btnSize };
    Btn modeBtn{ lay.modeX, btnY, btnSize, btnSize };
    Btn speedBtn{ lay.speedX, btnY, btnSize, btnSize };
    Btn volBtn{ lay.volX, btnY, btnSize, btnSize };
    Btn fsBtn{ lay.fsX, btnY, btnSize, btnSize };

    auto hit = [&](const Btn& b) {
        return mouseX_ >= b.x && mouseX_ < b.x + b.w && mouseY_ >= b.y && mouseY_ < b.y + b.h;
    };

    auto drawBtn = [&](const Btn& b, Icon icon, bool hover, bool active) {
        // 圆角背景：hover 时白色渐变，active 时蓝色高亮
        if (hover || active) {
            Uint8 bgA = active ? (Uint8)(70 * a / 255) : (Uint8)(45 * a / 255);
            Uint8 r = active ? 77 : 255, g = active ? 144 : 255, b2 = active ? 255 : 255;
            fillRoundedRect(renderer_, b.x, b.y, b.w, b.h, 8, r, g, b2, bgA);
        }
        // 图标：hover 时白色不透明度 255，否则 200；active 时图标缩放 1.1x
        int iconSize = hover ? 26 : 24;
        int iconAlpha = (int)((hover ? 255 : 200) * a / 255);
        drawIconOrTexture(renderer_, iconTexture(icon), icon,
                          b.x + b.w / 2, b.y + b.h / 2, iconSize, iconAlpha);
    };

    // ---- progress bar: full-width slim bar at the very bottom ----
    const int progX = lay.progX;
    const int progY = lay.progY;
    const int progressW = lay.progW;

    double pct = (stats.duration > 0) ? (stats.clock / stats.duration) : 0;
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;

    bool progHover = mouseX_ >= progX && mouseX_ < progX + progressW &&
                     mouseY_ >= progY - 10 && mouseY_ < progY + 12;
    int trackH = (progHover || stats.draggingVolume) ? 10 : 4;
    int trackY = progY - trackH / 2;

    // track 背景（深灰 + 圆角）
    fillRoundedRect(renderer_, progX, trackY, progressW, trackH, 3,
                    50, 50, 50, (Uint8)(200 * a / 255));
    // fill（蓝色渐变）
    int fillW = (int)(pct * progressW);
    if (fillW > 0) {
        fillRoundedRect(renderer_, progX, trackY, fillW, trackH, 3,
                        77, 144, 255, (Uint8)(255 * a / 255));
    }
    // hover/drag 时发光 thumb + 阴影
    if (progHover || stats.draggingVolume) {
        int thumbX = progX + fillW - 8;
        // 发光阴影
        fillRoundedRect(renderer_, thumbX - 4, trackY - 6, 24, trackH + 12, 8,
                        77, 144, 255, (Uint8)(60 * a / 255));
        // 圆形 thumb
        fillRoundedRect(renderer_, thumbX, trackY - 4, 16, trackH + 8, 8,
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
        }
    }

    // time text centered in the control row
    char curText[16], durText[16];
    formatTimeText(curText, sizeof(curText), stats.clock);
    formatTimeText(durText, sizeof(durText), stats.duration);
    char timeText[40];
    std::snprintf(timeText, sizeof(timeText), "%s / %s", curText, durText);
    int tw = 0;
    for (const char* p = timeText; *p; ++p) tw += 6 * 2;
    int timeX = (winW - tw) / 2;
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, (Uint8)(220 * a / 255));
    drawFontText(renderer_, timeX, barY + 22, 2, timeText);

    // ---- volume popup (above vol button) ----
    bool volHover = hit(volBtn);
    if (volHover || stats.draggingVolume) {
        int pw = 8, ph = 90;
        int px = volBtn.x + volBtn.w / 2 - pw / 2;
        int py = barY - ph - 12;
        // 背景
        fillRoundedRect(renderer_, px - 2, py - 2, pw + 4, ph + 4, 4,
                        30, 30, 30, (Uint8)(230 * a / 255));
        // 轨道
        fillRoundedRect(renderer_, px, py, pw, ph, 3, 60, 60, 60, (Uint8)(200 * a / 255));
        // 音量填充
        int vh = (int)((stats.muted ? 0.0f : stats.volume) * (ph - 8));
        if (vh > 0) {
            fillRoundedRect(renderer_, px, py + ph - 4 - vh, pw, vh, 3,
                            77, 144, 255, (Uint8)(255 * a / 255));
        }
        // thumb
        if (stats.draggingVolume) {
            int thumbY = py + ph - 4 - vh - 4;
            fillRoundedRect(renderer_, px - 3, thumbY, pw + 6, 8, 4,
                            255, 255, 255, (Uint8)(255 * a / 255));
        }
    }

    drawBtn(prevBtn, Icon::Prev, hit(prevBtn), false);
    drawBtn(playBtn, stats.playing && !stats.paused ? Icon::Pause : Icon::Play,
            hit(playBtn), false);
    drawBtn(nextBtn, Icon::Next, hit(nextBtn), false);
    drawBtn(volBtn, stats.muted ? Icon::Mute : Icon::Volume, volHover, false);
    drawBtn(fsBtn, stats.fullscreen ? Icon::ExitFullscreen : Icon::Fullscreen,
            hit(fsBtn), false);

    // play mode button (single/loop/shuffle)
    Icon modeIcon = stats.playMode == 0 ? Icon::Single
                   : stats.playMode == 1 ? Icon::Loop : Icon::Shuffle;
    drawBtn(modeBtn, modeIcon, hit(modeBtn), stats.playMode != 1);
    // speed button: text label showing current multiplier
    char spdText[16];
    std::snprintf(spdText, sizeof(spdText), "x%.2g", stats.speed);
    if (hit(speedBtn)) {
        fillRoundedRect(renderer_, speedBtn.x, speedBtn.y, speedBtn.w, speedBtn.h, 8,
                        255, 255, 255, (Uint8)(40 * a / 255));
    }
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, (Uint8)(220 * a / 255));
    drawFontText(renderer_, speedBtn.x + 7, speedBtn.y + 16, 2, spdText);

    // ---- speed menu (popup above speed button) ----
    if (speedMenuOpen_) {
        const int count = 8;
        float speeds[8] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f };
        int itemH = 28, menuW = 92;
        int menuX = speedBtn.x + speedBtn.w / 2 - menuW / 2;
        int menuBottom = lay.barY - 8;
        int menuY = menuBottom - count * itemH;
        fillRoundedRect(renderer_, menuX - 4, menuY - 4, menuW + 8, count * itemH + 8, 10,
                        20, 20, 20, (Uint8)(235 * a / 255));
        for (int i = 0; i < count; ++i) {
            SDL_Rect r = speedMenuItemRect(lay, i);
            bool cur = std::abs(stats.speed - speeds[i]) < 0.001f;
            if (cur) {
                fillRoundedRect(renderer_, r.x + 2, r.y + 2, r.w - 4, r.h - 4, 6,
                                77, 144, 255, (Uint8)(230 * a / 255));
            }
            char txt[16];
            std::snprintf(txt, sizeof(txt), "x%.2g", speeds[i]);
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, (Uint8)(220 * a / 255));
            drawFontText(renderer_, menuX + 28, r.y + 10, 2, txt);
        }
    }
}

void VideoRenderer::drawBackground() {
    if (!renderer_) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    const int titleH = 32;  // 标题栏高度
    int areaW = w - panelWidth_;  // 减去面板宽度
    // 视频区域纯黑 (0,0,0)，与标题栏 (30,30,30) 形成层次感
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_Rect videoArea{ 0, titleH, areaW, h - titleH };
    SDL_RenderFillRect(renderer_, &videoArea);
}

void VideoRenderer::render(const AVFrame* frame, const RenderStats& stats) {
    if (!renderer_ || !frame) return;

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
    const int titleH = 32;  // 标题栏高度
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
    drawSubtitle(stats);
    drawToast();
    drawControls(stats);
}

void VideoRenderer::clear() {
    destroySubtitleTexture();
    if (renderer_) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        drawBackground();
    }
}