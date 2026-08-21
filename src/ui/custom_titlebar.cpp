#include "ui/custom_titlebar.h"
#include <SDL_image.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

static const int kBtnW = 46;

// logo 纹理（全局缓存，只加载一次）
static SDL_Texture* gLogoTex = nullptr;
static int gLogoW = 0, gLogoH = 0;

static void ensureLogo(SDL_Renderer* renderer) {
    if (gLogoTex) return;
    // 尝试加载 vplay.bmp
    const char* paths[] = {
        "F:/vedioplayer/ico/vplay.bmp",
        "assets/icons/vplay.bmp",
        "vplay.bmp",
    };
    for (auto path : paths) {
        SDL_Surface* surf = IMG_Load(path);
        if (!surf) continue;
        gLogoTex = SDL_CreateTextureFromSurface(renderer, surf);
        gLogoW = surf->w;
        gLogoH = surf->h;
        SDL_FreeSurface(surf);
        if (gLogoTex) {
            SDL_SetTextureBlendMode(gLogoTex, SDL_BLENDMODE_BLEND);
            return;
        }
    }
}

void CustomTitlebar::init(HWND hwnd) {
    hwnd_ = hwnd;
    oldWndProc_ = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)wndProc);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
}

void CustomTitlebar::shutdown() {
    if (hwnd_ && oldWndProc_) {
        SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, (LONG_PTR)oldWndProc_);
        oldWndProc_ = nullptr;
    }
    if (gLogoTex) {
        SDL_DestroyTexture(gLogoTex);
        gLogoTex = nullptr;
    }
}

void CustomTitlebar::setTitle(const char* title) {
    title_ = title ? title : "VPlayer";
}

LRESULT CALLBACK CustomTitlebar::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CustomTitlebar* self = (CustomTitlebar*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self) return self->handleMsg(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int CustomTitlebar::hitTest(int x, int y) {
    if (y >= 0 && y < height) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        int w = rc.right - rc.left;
        int btnStart = w - kBtnW * 3;
        if (x >= btnStart && x < btnStart + kBtnW) return 100;
        if (x >= btnStart + kBtnW && x < btnStart + kBtnW * 2) return 101;
        if (x >= btnStart + kBtnW * 2 && x < btnStart + kBtnW * 3) return 102;
        return 2;
    }
    return 1;
}

LRESULT CustomTitlebar::handleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST: {
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);
        POINT pt{ x, y };
        ScreenToClient(hwnd, &pt);
        int hit = hitTest(pt.x, pt.y);
        if (hit == 2) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDOWN: {
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);
        POINT pt{ x, y };
        ScreenToClient(hwnd, &pt);
        int hit = hitTest(pt.x, pt.y);
        if (hit == 100) { SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0); return 0; }
        if (hit == 101) {
            bool isZoomed = IsZoomed(hwnd);
            SendMessage(hwnd, WM_SYSCOMMAND, isZoomed ? SC_RESTORE : SC_MAXIMIZE, 0);
            return 0;
        }
        if (hit == 102) { SendMessage(hwnd, WM_CLOSE, 0, 0); return 0; }
        break;
    }
    case WM_MOUSEMOVE: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        int hit = hitTest(x, y);
        int newHover = -1;
        if (hit == 100) newHover = 0;
        else if (hit == 101) newHover = 1;
        else if (hit == 102) newHover = 2;
        if (newHover != hoverBtn_) {
            hoverBtn_ = newHover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        if (hoverBtn_ != -1) { hoverBtn_ = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        break;
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        int hit = hitTest(x, y);
        if (hit == 100) SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        else if (hit == 101) {
            bool isZoomed = IsZoomed(hwnd);
            SendMessage(hwnd, WM_SYSCOMMAND, isZoomed ? SC_RESTORE : SC_MAXIMIZE, 0);
        }
        else if (hit == 102) SendMessage(hwnd, WM_CLOSE, 0, 0);
        break;
    }
    case WM_NCCALCSIZE: {
        if (wParam) {
            NCCALCSIZE_PARAMS* cs = (NCCALCSIZE_PARAMS*)lParam;
            cs->rgrc[0].top += 1;
            cs->rgrc[0].left += 1;
            cs->rgrc[0].right -= 1;
            cs->rgrc[0].bottom -= 1;
        }
        return 0;
    }
    }
    return CallWindowProcW(oldWndProc_, hwnd, msg, wParam, lParam);
}

// ---- 像素字体 5x7 ----
static const unsigned char kDigitFont[][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
};

static void drawPixelChar(SDL_Renderer* r, int x, int y, int scale, int ch, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
            if (kDigitFont[ch][row] & (0x10 >> col)) {
                SDL_Rect px{ x + col * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(r, &px);
            }
}

static void drawText(SDL_Renderer* r, int x, int y, int scale, const char* text, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c >= '0' && c <= '9') { drawPixelChar(r, x, y, scale, c - '0', cr, cg, cb, ca); x += 6 * scale; }
        else if (c == ' ') x += 3 * scale;
        else if (c == '-') { SDL_SetRenderDrawColor(r, cr, cg, cb, ca); SDL_Rect d{x,y+3*scale,4*scale,scale}; SDL_RenderFillRect(r,&d); x += 5*scale; }
        else { x += 4 * scale; }
    }
}

static int textPixelWidth(const char* text, int scale) {
    int w = 0;
    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c >= '0' && c <= '9') w += 6 * scale;
        else if (c == ' ') w += 3 * scale;
        else if (c == '-') w += 5 * scale;
        else w += 4 * scale;
    }
    return w;
}

// ---- 按钮图标 ----
static void drawBtnIcon(SDL_Renderer* r, int icon, int cx, int cy, int size, Uint8 alpha) {
    SDL_SetRenderDrawColor(r, 210, 210, 210, alpha);
    float s = (float)size / 20.0f;
    auto px = [&](float x) { return (int)std::lround(cx + x * s); };
    auto py = [&](float y) { return (int)std::lround(cy + y * s); };
    switch (icon) {
    case 0: SDL_RenderDrawLine(r, px(-5), py(1), px(5), py(1)); break;
    case 1: { SDL_Rect o{px(-5),py(-5),(int)(10*s+1),(int)(10*s+1)}; SDL_RenderDrawRect(r,&o); } break;
    case 2: SDL_RenderDrawLine(r,px(-5),py(-5),px(5),py(5)); SDL_RenderDrawLine(r,px(5),py(-5),px(-5),py(5)); break;
    }
}

// ---- 绘制 ----
void CustomTitlebar::draw(SDL_Renderer* renderer) {
    if (!renderer) return;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(SDL_RenderGetWindow(renderer), &winW, &winH);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // 标题栏背景 (30,30,30)
    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
    SDL_Rect titleBg{ 0, 0, winW, height };
    SDL_RenderFillRect(renderer, &titleBg);

    // 底部分隔线 (55,55,55)
    SDL_SetRenderDrawColor(renderer, 55, 55, 55, 255);
    SDL_Rect sepLine{ 0, height - 1, winW, 1 };
    SDL_RenderFillRect(renderer, &sepLine);

    int textScale = 2;
    int textY = (height - 7 * textScale) / 2;

    // 左侧：vplay.bmp logo + "VPlayer" 文字
    ensureLogo(renderer);
    int logoLeft = 12;
    if (gLogoTex) {
        // logo 缩放到 20x20
        int logoSize = 20;
        SDL_Rect dst{ logoLeft, (height - logoSize) / 2, logoSize, logoSize };
        SDL_RenderCopy(renderer, gLogoTex, nullptr, &dst);
        logoLeft += logoSize + 6;
    }
    // "VPlayer" 用浅灰白 (200,200,200)
    drawText(renderer, logoLeft, textY, textScale, "VPlayer", 200, 200, 200, 255);

    // 右侧：三个按钮
    int btnStart = winW - kBtnW * 3;
    for (int i = 0; i < 3; ++i) {
        int bx = btnStart + i * kBtnW;
        bool hover = (hoverBtn_ == i);
        if (hover) {
            Uint8 r, g, b;
            if (i == 2) { r = 196; g = 43; b = 28; }
            else { r = 55; g = 55; b = 55; }
            SDL_SetRenderDrawColor(renderer, r, g, b, 255);
            SDL_Rect btnRc{ bx, 0, kBtnW, height };
            SDL_RenderFillRect(renderer, &btnRc);
        }
        drawBtnIcon(renderer, i, bx + kBtnW / 2, height / 2, 18, hover ? 255 : 150);
    }

    // 中间：视频文件名（白灰色 (210,210,210)，超出滚动）
    int leftEnd = logoLeft + textPixelWidth("VPlayer", textScale) + 24;
    int rightStart = btnStart - 10;
    int centerAreaW = rightStart - leftEnd;
    if (centerAreaW > 50 && !title_.empty()) {
        const char* displayName = title_.c_str();
        const char* dash = strstr(title_.c_str(), " - ");
        if (dash) displayName = dash + 3;

        int titleTextW = textPixelWidth(displayName, textScale);

        if (titleTextW <= centerAreaW) {
            int titleX = leftEnd + (centerAreaW - titleTextW) / 2;
            drawText(renderer, titleX, textY, textScale, displayName, 210, 210, 210, 255);
        } else {
            // 滚动
            Uint32 now = SDL_GetTicks();
            if (lastScrollTick_ == 0) lastScrollTick_ = now;
            float dt = (float)(now - lastScrollTick_) / 1000.0f;
            lastScrollTick_ = now;
            if (dt > 0.1f) dt = 0.016f;

            float maxOffset = (float)(titleTextW - centerAreaW + centerAreaW / 3);
            if (!scrollPaused_) {
                float speed = 60.0f;
                scrollOffset_ += scrollDir_ * speed * dt;
                if (scrollOffset_ >= maxOffset) { scrollOffset_ = maxOffset; scrollPaused_ = true; pauseStart_ = now; }
                else if (scrollOffset_ <= 0) { scrollOffset_ = 0; scrollPaused_ = true; pauseStart_ = now; }
            } else if (now - pauseStart_ > 1500) {
                scrollPaused_ = false;
                scrollDir_ = -scrollDir_;
            }

            SDL_Rect clipRc{ leftEnd, 0, centerAreaW, height };
            SDL_RenderSetClipRect(renderer, &clipRc);
            int titleX = leftEnd + centerAreaW / 6 - (int)scrollOffset_;
            drawText(renderer, titleX, textY, textScale, displayName, 210, 210, 210, 255);
            SDL_RenderSetClipRect(renderer, nullptr);
        }
    }
}

void CustomTitlebar::updateScroll() {}
