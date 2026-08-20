#include "video/video_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

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
        else continue;
        drawFontGlyph(r, x, y, scale, g);
        x += 6 * scale;
    }
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

        // Convert UTF-8 to UTF-16
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

        // 32-bit DIB (BGRA), bottom-up
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = tw;
        bmi.bmiHeader.biHeight = -th;  // top-down
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
            // Draw shadow offsets then white text
            RECT drc{ 12, 12, tw - 12, th - 12 };
            SetTextColor(mem, RGB(0, 0, 0));
            DrawTextW(mem, wtext.c_str(), -1, &drc, DT_NOPREFIX | DT_CENTER | DT_WORDBREAK);
            RECT drc2{ 10, 10, tw - 10, th - 10 };
            SetTextColor(mem, RGB(255, 255, 255));
            DrawTextW(mem, wtext.c_str(), -1, &drc2, DT_NOPREFIX | DT_CENTER | DT_WORDBREAK);

            // Convert BGRA -> SDL surface (IYUV-like copy: just create ARGB texture)
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

void VideoRenderer::onMouseMove(int x, int y) {
    (void)x; (void)y;
    lastMouseMove_ = SDL_GetTicks();
    controlsVisible_ = true;
}

void VideoRenderer::drawControls(const RenderStats& stats) {
    int winW = 0, winH = 0;
    SDL_GetWindowSize(window_, &winW, &winH);
    const int h = 60;
    const int barY = winH - h;

    // Auto-hide: hide after 500ms of inactivity
    Uint32 now = SDL_GetTicks();
    if (now - lastMouseMove_ > 500) {
        controlsVisible_ = false;
    }

    if (!controlsVisible_) return;

    // Dark semi-transparent bar
    SDL_SetRenderDrawColor(renderer_, 24, 24, 24, 217);
    SDL_Rect bar = {0, barY, winW, h};
    SDL_RenderFillRect(renderer_, &bar);

    // Top highlight
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 25);
    SDL_RenderDrawLine(renderer_, 0, barY, winW, barY);

    // Bottom shadow
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 80);
    SDL_RenderDrawLine(renderer_, 0, winH-1, winW, winH-1);
    
    const int btnY = barY + 18;
    
    // Prev button
    SDL_Rect prevBtn = {16, btnY, 20, 24};
    SDL_SetRenderDrawColor(renderer_, 51, 51, 51, 255);
    SDL_RenderFillRect(renderer_, &prevBtn);
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 220);
    // Prev icon: double left-arrow
    SDL_RenderDrawLines(renderer_, (SDL_Point[3]){{prevBtn.x+5, prevBtn.y+4}, {prevBtn.x+13, prevBtn.y+12}, {prevBtn.x+5, prevBtn.y+20}}, 3);
    SDL_RenderDrawLines(renderer_, (SDL_Point[3]){{prevBtn.x+8, prevBtn.y+4}, {prevBtn.x+16, prevBtn.y+12}, {prevBtn.x+8, prevBtn.y+20}}, 3);
    
    // Play/Pause button
    const int playBtnX = 44;
    SDL_Rect playBtn = {playBtnX, btnY, 24, 24};
    SDL_SetRenderDrawColor(renderer_, 51, 51, 51, 255);
    SDL_RenderFillRect(renderer_, &playBtn);
    SDL_SetRenderDrawColor(renderer_, 220, 220, 220, 255);
    if (stats.playing && !stats.paused) {
        SDL_Rect bar1 = {playBtnX+6, btnY+4, 5, 16};
        SDL_Rect bar2 = {playBtnX+13, btnY+4, 5, 16};
        SDL_RenderFillRect(renderer_, &bar1);
        SDL_RenderFillRect(renderer_, &bar2);
    } else {
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        SDL_RenderDrawLines(renderer_, (SDL_Point[3]){{playBtnX+7, btnY+4}, {playBtnX+17, btnY+12}, {playBtnX+7, btnY+20}}, 3);
        SDL_RenderDrawLine(renderer_, playBtnX+7, btnY+4, playBtnX+7, btnY+20);
    }
    
    // Next button
    SDL_Rect nextBtn = {76, btnY, 20, 24};
    SDL_SetRenderDrawColor(renderer_, 51, 51, 51, 255);
    SDL_RenderFillRect(renderer_, &nextBtn);
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 220);
    SDL_RenderDrawLines(renderer_, (SDL_Point[3]){{nextBtn.x+4, nextBtn.y+4}, {nextBtn.x+12, nextBtn.y+12}, {nextBtn.x+4, nextBtn.y+20}}, 3);
    SDL_RenderDrawLines(renderer_, (SDL_Point[3]){{nextBtn.x+7, nextBtn.y+4}, {nextBtn.x+15, nextBtn.y+12}, {nextBtn.x+7, nextBtn.y+20}}, 3);

    // Progress bar
    const int progressX = 100, progressY = barY + 28, progressWidth = winW - 120;
    double pct = (stats.duration > 0) ? (stats.clock / stats.duration) : 0;
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;

    // Track
    SDL_SetRenderDrawColor(renderer_, 64, 64, 64, 255);
    SDL_Rect track = {progressX, progressY, progressWidth, 6};
    SDL_RenderFillRect(renderer_, &track);
    // Fill
    int fillW = (int)(pct * progressWidth);
    if (fillW > 0) {
        SDL_SetRenderDrawColor(renderer_, 77, 144, 255, 255);
        SDL_Rect filled = {progressX, progressY, fillW, 6};
        SDL_RenderFillRect(renderer_, &filled);
    }

    // Speed indicator (right of progress bar)
    char spdText[16];
    std::snprintf(spdText, sizeof(spdText), "x%.2g", stats.speed);
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 220);
    drawFontText(renderer_, progressX + progressWidth + 8, barY + 24, 2, spdText);

    // Volume button
    const int volX = winW - 60, volBtnY = barY + 18;
    SDL_SetRenderDrawColor(renderer_, 51, 51, 51, 255);
    SDL_Rect volBtn = {volX, volBtnY, 20, 24};
    SDL_RenderFillRect(renderer_, &volBtn);
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 220);
    SDL_Rect volIcon1 = {volX+3, volBtnY+8, 5, 6};
    SDL_RenderFillRect(renderer_, &volIcon1);
    SDL_Rect volIcon2 = {volX+6, volBtnY+6, 3, 10};
    SDL_RenderFillRect(renderer_, &volIcon2);

    // Fullscreen button
    const int fsX = winW - 40;
    SDL_Rect fsBtn = {fsX, btnY, 20, 24};
    SDL_SetRenderDrawColor(renderer_, 51, 51, 51, 255);
    SDL_RenderFillRect(renderer_, &fsBtn);
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 220);
    SDL_Rect outer = {fsX+2, btnY+4, 16, 16};
    SDL_RenderDrawRect(renderer_, &outer);
    SDL_Rect inner = {fsX+5, btnY+7, 10, 10};
    SDL_RenderDrawRect(renderer_, &inner);
}

void VideoRenderer::render(const AVFrame* frame, const RenderStats& stats) {
    onMouseMove(0, 0);  // Reset timer (mouse pos handled separately in main)
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