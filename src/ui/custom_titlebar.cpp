#include "ui/custom_titlebar.h"
#include <SDL_image.h>
#include <cmath>
#include <cstring>
#include <string>

static const int kBtnW = 46;

static SDL_Texture* gLogoTex = nullptr;

static void ensureLogo(SDL_Renderer* renderer) {
    if (gLogoTex) return;
    const char* paths[] = {
        "F:/vedioplayer/ico/vplay.bmp",
        "assets/icons/vplay.bmp",
        "vplay.bmp",
    };
    for (auto path : paths) {
        SDL_Surface* surf = IMG_Load(path);
        if (!surf) continue;
        gLogoTex = SDL_CreateTextureFromSurface(renderer, surf);
        SDL_FreeSurface(surf);
        if (gLogoTex) { SDL_SetTextureBlendMode(gLogoTex, SDL_BLENDMODE_BLEND); return; }
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
    if (gLogoTex) { SDL_DestroyTexture(gLogoTex); gLogoTex = nullptr; }
    if (textTex_) { SDL_DestroyTexture(textTex_); textTex_ = nullptr; }
}

void CustomTitlebar::setTitle(const char* title) {
    if (title && title_ != title) {
        title_ = title;
        // 标题变化时清除文字纹理缓存，下次 draw 会重新生成
        if (textTex_) { SDL_DestroyTexture(textTex_); textTex_ = nullptr; }
        scrollOffset_ = 0;
        scrollDir_ = 1;
        scrollPaused_ = false;
        lastScrollTick_ = 0;
    }
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
            bool z = IsZoomed(hwnd);
            SendMessage(hwnd, WM_SYSCOMMAND, z ? SC_RESTORE : SC_MAXIMIZE, 0);
            return 0;
        }
        if (hit == 102) { SendMessage(hwnd, WM_CLOSE, 0, 0); return 0; }
        break;
    }
    case WM_MOUSEMOVE: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        int hit = hitTest(x, y);
        int nh = -1;
        if (hit == 100) nh = 0; else if (hit == 101) nh = 1; else if (hit == 102) nh = 2;
        if (nh != hoverBtn_) { hoverBtn_ = nh; InvalidateRect(hwnd, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        if (hoverBtn_ != -1) { hoverBtn_ = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        break;
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam), y = HIWORD(lParam);
        int hit = hitTest(x, y);
        if (hit == 100) SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        else if (hit == 101) { bool z = IsZoomed(hwnd); SendMessage(hwnd, WM_SYSCOMMAND, z ? SC_RESTORE : SC_MAXIMIZE, 0); }
        else if (hit == 102) SendMessage(hwnd, WM_CLOSE, 0, 0);
        break;
    }
    case WM_NCCALCSIZE: {
        if (wParam) {
            NCCALCSIZE_PARAMS* cs = (NCCALCSIZE_PARAMS*)lParam;
            cs->rgrc[0].top += 1; cs->rgrc[0].left += 1;
            cs->rgrc[0].right -= 1; cs->rgrc[0].bottom -= 1;
        }
        return 0;
    }
    }
    return CallWindowProcW(oldWndProc_, hwnd, msg, wParam, lParam);
}

// ---- GDI 渲染文字到 SDL_Texture ----
SDL_Texture* CustomTitlebar::renderTextGDI(SDL_Renderer* renderer, const char* text,
                                           int fontSize, Uint8 cr, Uint8 cg, Uint8 cb,
                                           int& outW, int& outH) {
    if (!text || !*text) return nullptr;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring wtext(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), wlen);

    HDC mem = CreateCompatibleDC(nullptr);
    if (!mem) return nullptr;

    HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(mem, font);

    // 测量文字尺寸
    RECT rc{ 0, 0, 4000, 200 };
    DrawTextW(mem, wtext.c_str(), -1, &rc, DT_CALCRECT | DT_NOPREFIX | DT_SINGLELINE);
    int tw = rc.right - rc.left + 8;
    int th = rc.bottom - rc.top + 4;
    if (tw < 4) tw = 4;
    if (th < 4) th = 4;

    // 创建 DIB
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tw;
    bmi.bmiHeader.biHeight = -th;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp) { SelectObject(mem, oldFont); DeleteObject(font); DeleteDC(mem); return nullptr; }

    HGDIOBJ oldBmp = SelectObject(mem, hbmp);
    // 透明背景
    RECT trc{ 0, 0, tw, th };
    HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    FillRect(mem, &trc, nullBrush);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(cr, cg, cb));
    RECT drc{ 4, 2, tw, th };
    DrawTextW(mem, wtext.c_str(), -1, &drc, DT_NOPREFIX | DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    // 转换为 SDL_Texture
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, tw, th);
    if (tex) {
        void* tbits = nullptr;
        int pitch = 0;
        if (SDL_LockTexture(tex, nullptr, &tbits, &pitch) == 0) {
            // 逐行复制（pitch 可能不同）
            Uint8* src = (Uint8*)bits;
            Uint8* dst = (Uint8*)tbits;
            int rowBytes = tw * 4;
            for (int row = 0; row < th; ++row) {
                memcpy(dst + row * pitch, src + row * rowBytes, rowBytes);
            }
            SDL_UnlockTexture(tex);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        } else {
            SDL_DestroyTexture(tex);
            tex = nullptr;
        }
    }

    SelectObject(mem, oldBmp);
    DeleteObject(hbmp);
    SelectObject(mem, oldFont);
    DeleteObject(font);
    DeleteDC(mem);

    outW = tw;
    outH = th;
    return tex;
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

    // 左侧：logo + "VPlayer"（GDI 渲染）
    ensureLogo(renderer);
    int logoLeft = 12;
    if (gLogoTex) {
        SDL_Rect dst{ logoLeft, (height - 20) / 2, 20, 20 };
        SDL_RenderCopy(renderer, gLogoTex, nullptr, &dst);
        logoLeft += 26;
    }

    // "VPlayer" 标签
    int labelW = 0, labelH = 0;
    SDL_Texture* labelTex = renderTextGDI(renderer, "VPlayer", 14, 200, 200, 200, labelW, labelH);
    if (labelTex) {
        SDL_Rect dst{ logoLeft, (height - labelH) / 2, labelW, labelH };
        SDL_RenderCopy(renderer, labelTex, nullptr, &dst);
        SDL_DestroyTexture(labelTex);
        logoLeft += labelW + 16;
    }

    // 右侧按钮
    int btnStart = winW - kBtnW * 3;
    for (int i = 0; i < 3; ++i) {
        int bx = btnStart + i * kBtnW;
        bool hover = (hoverBtn_ == i);
        if (hover) {
            Uint8 r, g, b;
            if (i == 2) { r = 196; g = 43; b = 28; } else { r = 55; g = 55; b = 55; }
            SDL_SetRenderDrawColor(renderer, r, g, b, 255);
            SDL_Rect btnRc{ bx, 0, kBtnW, height };
            SDL_RenderFillRect(renderer, &btnRc);
        }
        drawBtnIcon(renderer, i, bx + kBtnW / 2, height / 2, 18, hover ? 255 : 150);
    }

    // 中间：视频文件名
    int rightStart = btnStart - 8;
    int centerAreaW = rightStart - logoLeft;
    if (centerAreaW > 50 && !title_.empty()) {
        // 提取文件名
        const char* displayName = title_.c_str();
        const char* dash = strstr(title_.c_str(), " - ");
        if (dash) displayName = dash + 3;

        // 渲染文字纹理
        if (cachedText_ != displayName || !textTex_) {
            if (textTex_) { SDL_DestroyTexture(textTex_); textTex_ = nullptr; }
            cachedText_ = displayName;
            textTex_ = renderTextGDI(renderer, displayName, 14, 210, 210, 210, textTexW_, textTexH_);
        }

        if (textTex_) {
            if (textTexW_ <= centerAreaW) {
                // 居中显示
                int tx = logoLeft + (centerAreaW - textTexW_) / 2;
                SDL_Rect dst{ tx, (height - textTexH_) / 2, textTexW_, textTexH_ };
                SDL_RenderCopy(renderer, textTex_, nullptr, &dst);
            } else {
                // 滚动显示
                Uint32 now = SDL_GetTicks();
                if (lastScrollTick_ == 0) lastScrollTick_ = now;
                float dt = (float)(now - lastScrollTick_) / 1000.0f;
                lastScrollTick_ = now;
                if (dt > 0.1f) dt = 0.016f;

                float maxOffset = (float)(textTexW_ - centerAreaW + centerAreaW / 3);
                if (!scrollPaused_) {
                    scrollOffset_ += scrollDir_ * 60.0f * dt;
                    if (scrollOffset_ >= maxOffset) { scrollOffset_ = maxOffset; scrollPaused_ = true; pauseStart_ = now; }
                    else if (scrollOffset_ <= 0) { scrollOffset_ = 0; scrollPaused_ = true; pauseStart_ = now; }
                } else if (now - pauseStart_ > 1500) {
                    scrollPaused_ = false;
                    scrollDir_ = -scrollDir_;
                }

                SDL_Rect clipRc{ logoLeft, 0, centerAreaW, height };
                SDL_RenderSetClipRect(renderer, &clipRc);
                int tx = logoLeft + centerAreaW / 6 - (int)scrollOffset_;
                SDL_Rect dst{ tx, (height - textTexH_) / 2, textTexW_, textTexH_ };
                SDL_RenderCopy(renderer, textTex_, nullptr, &dst);
                SDL_RenderSetClipRect(renderer, nullptr);
            }
        }
    }
}
