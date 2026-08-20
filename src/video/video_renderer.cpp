#include "video/video_renderer.h"

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

enum class Icon { Play, Pause, Prev, Next, Volume, Mute, Fullscreen, ExitFullscreen };

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
        SDL_RenderDrawLine(r, px(-9), py(-8), px(-9), py(8));
        SDL_RenderDrawLine(r, px(-8), py(-8), px(5), py(0));
        SDL_RenderDrawLine(r, px(-8), py(8), px(5), py(0));
        SDL_RenderDrawLine(r, px(5), py(-8), px(5), py(8));
        break;
    }
    case Icon::Next: {
        SDL_RenderDrawLine(r, px(-5), py(-8), px(-5), py(8));
        SDL_RenderDrawLine(r, px(8), py(-8), px(-5), py(0));
        SDL_RenderDrawLine(r, px(8), py(8), px(-5), py(0));
        SDL_RenderDrawLine(r, px(9), py(-8), px(9), py(8));
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

void VideoRenderer::drawControls(const RenderStats& stats) {
    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);
    const int h = 64;
    const int barY = winH - h;

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
    const int btnSize = 40;
    const int btnY = barY + (h - btnSize) / 2;
    const int gap = 12;
    int cx = gap + btnSize / 2;  // center of Prev

    struct Btn { int x, y, w, h; };
    Btn prevBtn{ gap, btnY, btnSize, btnSize };
    Btn playBtn{ gap + btnSize + gap, btnY, btnSize, btnSize };
    Btn nextBtn{ gap + 2 * (btnSize + gap), btnY, btnSize, btnSize };
    Btn volBtn{ winW - gap - 2 * (btnSize + gap) + gap, btnY, btnSize, btnSize };
    Btn fsBtn{ winW - gap - btnSize, btnY, btnSize, btnSize };

    auto hit = [&](const Btn& b) {
        return mouseX_ >= b.x && mouseX_ < b.x + b.w && mouseY_ >= b.y && mouseY_ < b.y + b.h;
    };

    auto drawBtn = [&](const Btn& b, Icon icon, bool hover, bool active) {
        if (hover || active) {
            fillRoundedRect(renderer_, b.x, b.y, b.w, b.h, 8,
                            255, 255, 255, (Uint8)(hover ? 40 * a / 255 : 60 * a / 255));
        }
        drawIcon(renderer_, icon, b.x + b.w / 2, b.y + b.h / 2, 24,
                 (int)((hover ? 255 : 200) * a / 255));
    };

    // ---- progress bar (left of volume group) ----
    int timeW = 96;
    int progressW = fsBtn.x - (nextBtn.x + nextBtn.w + gap) - 24 - timeW;
    if (progressW < 100) progressW = 100;
    const int progY = barY + (h - 6) / 2;
    const int progX = nextBtn.x + nextBtn.w + gap + 12;

    double pct = (stats.duration > 0) ? (stats.clock / stats.duration) : 0;
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;

    bool progHover = mouseX_ >= progX - 6 && mouseX_ < progX + progressW + 6 &&
                     mouseY_ >= progY - 8 && mouseY_ < progY + 14;

    // track
    fillRoundedRect(renderer_, progX, progY, progressW, 6, 3,
                    80, 80, 80, (Uint8)(180 * a / 255));
    // fill
    int fillW = (int)(pct * progressW);
    if (fillW > 0) {
        fillRoundedRect(renderer_, progX, progY, fillW, 6, 3,
                        77, 144, 255, (Uint8)(255 * a / 255));
    }
    // hover thumb
    if (progHover || stats.draggingVolume) {
        int thumbX = progX + fillW - 5;
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, (Uint8)(255 * a / 255));
        SDL_Rect thumb{ thumbX, progY - 4, 14, 14 };
        SDL_RenderFillRect(renderer_, &thumb);
    }

    // speed indicator
    char spdText[16];
    std::snprintf(spdText, sizeof(spdText), "x%.2g", stats.speed);
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, (Uint8)(180 * a / 255));
    drawFontText(renderer_, progX + progressW + 10, barY + 22, 2, spdText);

    // time text (current / total) below progress bar
    char curText[16], durText[16];
    formatTimeText(curText, sizeof(curText), stats.clock);
    formatTimeText(durText, sizeof(durText), stats.duration);
    char timeText[40];
    std::snprintf(timeText, sizeof(timeText), "%s / %s", curText, durText);
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, (Uint8)(220 * a / 255));
    drawFontText(renderer_, progX + progressW + 10, barY + 34, 2, timeText);

    // ---- volume popup (above vol button) ----
    bool volHover = hit(volBtn);
    if (volHover || stats.draggingVolume) {
        int pw = 6, ph = 90;
        int px = volBtn.x + volBtn.w / 2 - pw / 2;
        int py = barY - ph - 12;
        fillRoundedRect(renderer_, px, py, pw, ph, 3, 30, 30, 30, (Uint8)(230 * a / 255));
        int vh = (int)((stats.muted ? 0.0f : stats.volume) * (ph - 12));
        if (vh > 0) {
            fillRoundedRect(renderer_, px, py + ph - 6 - vh, pw, vh, 3,
                            77, 144, 255, (Uint8)(255 * a / 255));
        }
    }

    drawBtn(prevBtn, Icon::Prev, hit(prevBtn), false);
    drawBtn(playBtn, stats.playing && !stats.paused ? Icon::Pause : Icon::Play,
            hit(playBtn), false);
    drawBtn(nextBtn, Icon::Next, hit(nextBtn), false);
    drawBtn(volBtn, stats.muted ? Icon::Mute : Icon::Volume, volHover, false);
    drawBtn(fsBtn, stats.fullscreen ? Icon::ExitFullscreen : Icon::Fullscreen,
            hit(fsBtn), false);
}

void VideoRenderer::render(const AVFrame* frame, const RenderStats& stats) {
    if (!renderer_ || !frame) return;

    if (frame->width != fw_ || frame->height != fh_) {
        if (texture_) SDL_DestroyTexture(texture_);
        fw_ = frame->width;
        fh_ = frame->height;
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV,
                                     SDL_TEXTUREACCESS_STREAMING, fw_, fh_);
        if (!texture_) return;
    }

    SDL_UpdateYUVTexture(texture_, nullptr,
                         frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1],
                         frame->data[2], frame->linesize[2]);

    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);
    float scale = std::min((float)winW / fw_, (float)winH / fh_);
    SDL_Rect dst;
    dst.w = (int)(fw_ * scale);
    dst.h = (int)(fh_ * scale);
    dst.x = (winW - dst.w) / 2;
    dst.y = (winH - dst.h) / 2;

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
    drawSubtitle(stats);
    drawControls(stats);
}

void VideoRenderer::clear() {
    destroySubtitleTexture();
    if (renderer_) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
    }
}