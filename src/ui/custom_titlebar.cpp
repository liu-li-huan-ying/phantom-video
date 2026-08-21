#include "ui/custom_titlebar.h"
#include <cmath>
#include <string>
#include <vector>

static const int kBtnW = 46;

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
}

void CustomTitlebar::setTitle(const char* title) {
    title_ = title ? title : "VPlayer";
}

LRESULT CALLBACK CustomTitlebar::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CustomTitlebar* self = (CustomTitlebar*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self)
        return self->handleMsg(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int CustomTitlebar::hitTest(int x, int y) {
    if (y >= 0 && y < height) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        int w = rc.right - rc.left;
        int btnStart = w - kBtnW * 3;
        if (x >= btnStart && x < btnStart + kBtnW) return 100;  // 最小化
        if (x >= btnStart + kBtnW && x < btnStart + kBtnW * 2) return 101;  // 最大化
        if (x >= btnStart + kBtnW * 2 && x < btnStart + kBtnW * 3) return 102;  // 关闭
        return 2;  // HTCAPTION (可拖动)
    }
    return 1;  // HTCLIENT
}

LRESULT CustomTitlebar::handleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST: {
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);
        POINT pt{ x, y };
        ScreenToClient(hwnd, &pt);
        int hit = hitTest(pt.x, pt.y);
        if (hit == 100) return HTMINBUTTON;
        if (hit == 101) return HTMAXBUTTON;
        if (hit == 102) return HTCLOSE;
        if (hit == 2) return HTCAPTION;
        return HTCLIENT;
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
            InvalidateRect(hwnd, nullptr, FALSE);  // 触发 SDL 重绘
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        if (hoverBtn_ != -1) {
            hoverBtn_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
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

// ---- SDL 绘制标题栏 ----

// 像素字体 5x7（和 video_renderer.cpp 一致）
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
};

static void drawFontGlyph(SDL_Renderer* r, int x, int y, int scale, const unsigned char* glyph) {
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
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
        if (*p < '0' || *p > '9') { x += 3 * scale + 1; continue; }
        drawFontGlyph(r, x, y, scale, kDigitFont[*p - '0']);
        x += 6 * scale;
    }
}

static int textWidth(SDL_Renderer* r, const char* text, int scale) {
    int w = 0;
    for (const char* p = text; *p; ++p) {
        if (*p >= '0' && *p <= '9') w += 6 * scale;
        else if (*p == ' ') w += 3 * scale + 1;
        else if (*p == ':' || *p == '.' || *p == '/') w += 6 * scale;
        else w += 3 * scale + 1;
    }
    return w;
}

// 圆角矩形填充（复用 video_renderer.cpp 的逻辑）
static void fillRoundedRect(SDL_Renderer* r, int x, int y, int w, int h, int rad,
                            Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_Rect rc{ x, y, w, h };
    SDL_RenderFillRect(r, &rc);
}

static void drawIcon(SDL_Renderer* r, int icon, int cx, int cy, int size, int alpha) {
    SDL_SetRenderDrawColor(r, 255, 255, 255, alpha);
    float s = (float)size / 24.0f;
    auto px = [&](float x) { return (int)std::lround(cx + x * s); };
    auto py = [&](float y) { return (int)std::lround(cy + y * s); };

    switch (icon) {
    case 0: // 最小化 — 横线
        SDL_RenderDrawLine(r, px(-6), py(0), px(6), py(0));
        break;
    case 1: // 最大化 — 方框
        { SDL_Rect o{ px(-6), py(-6), (int)(12 * s), (int)(12 * s) };
          SDL_RenderDrawRect(r, &o); }
        break;
    case 2: // 关闭 — ×
        SDL_RenderDrawLine(r, px(-6), py(-6), px(6), py(6));
        SDL_RenderDrawLine(r, px(6), py(-6), px(-6), py(6));
        break;
    }
}

void CustomTitlebar::draw(SDL_Renderer* renderer) {
    if (!renderer) return;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(SDL_RenderGetWindow(renderer), &winW, &winH);

    // 1. 标题栏背景 (24,24,24)
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    fillRoundedRect(renderer, 0, 0, winW, height, 0, 24, 24, 24, 255);

    // 2. 标题文字（左对齐，使用像素字体）
    int textScale = 2;
    int textY = (height - 7 * textScale) / 2;
    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
    drawFontText(renderer, 12, textY, textScale, title_.c_str());

    // 3. 按钮（右对齐）
    int btnStart = winW - kBtnW * 3;
    for (int i = 0; i < 3; ++i) {
        int bx = btnStart + i * kBtnW;
        bool hover = (hoverBtn_ == i);
        // hover 背景
        if (hover) {
            Uint8 r = (i == 2) ? 196 : 60;  // 关闭红色，其他灰色
            Uint8 g = (i == 2) ? 43 : 60;
            Uint8 b = (i == 2) ? 28 : 60;
            fillRoundedRect(renderer, bx, 0, kBtnW, height, 0, r, g, b, 255);
        }
        // 图标
        int iconAlpha = hover ? 255 : 160;
        drawIcon(renderer, i, bx + kBtnW / 2, height / 2, 20, iconAlpha);
    }
}
