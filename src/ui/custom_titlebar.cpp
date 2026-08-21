#include "ui/custom_titlebar.h"
#include <wingdi.h>
#include <string>

static const int kTitlebarH = 32;
static const int kBtnW = 46;
static const int kBtnH = kTitlebarH;

void CustomTitlebar::init(HWND hwnd) {
    hwnd_ = hwnd;
    // Hook 窗口消息
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
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    // 标题栏区域
    if (y >= 0 && y < kTitlebarH) {
        // 按钮区域（右对齐）
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
    case WM_PAINT: {
        draw();
        return 0;
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
        // 去掉默认标题栏和边框，客户区填满整个窗口
        if (wParam) {
            NCCALCSIZE_PARAMS* cs = (NCCALCSIZE_PARAMS*)lParam;
            // 扩展客户区到整个窗口（减去1像素保留窗口边框阴影）
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

void CustomTitlebar::draw() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;

    // 标题栏背景（深色半透明渐变）
    HBRUSH bgBrush = CreateSolidBrush(RGB(24, 24, 24));
    RECT titleRc{ 0, 0, w, kTitlebarH };
    FillRect(hdc, &titleRc, bgBrush);
    DeleteObject(bgBrush);

    // 标题文字
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(hdc, font);
    RECT textRc{ 12, 0, w - kBtnW * 3 - 8, kTitlebarH };
    DrawTextA(hdc, title_.c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
    DeleteObject(font);

    // 按钮
    int btnStart = w - kBtnW * 3;
    const wchar_t* btnSymbols[] = { L"\u2500", L"\u25A1", L"\u00D7" };  // — □ ×
    for (int i = 0; i < 3; ++i) {
        RECT btnRc{ btnStart + i * kBtnW, 0, btnStart + (i + 1) * kBtnW, kBtnH };
        bool hover = (hoverBtn_ == i);
        // hover 背景
        if (hover) {
            HBRUSH hoverBrush;
            if (i == 2)  // 关闭按钮红色高亮
                hoverBrush = CreateSolidBrush(RGB(196, 43, 28));
            else
                hoverBrush = CreateSolidBrush(RGB(60, 60, 60));
            FillRect(hdc, &btnRc, hoverBrush);
            DeleteObject(hoverBrush);
        }
        // 按钮符号
        SetTextColor(hdc, hover ? RGB(255, 255, 255) : RGB(180, 180, 180));
        HFONT btnFont = CreateFontW(-16, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
        HGDIOBJ oldBtnFont = SelectObject(hdc, btnFont);
        DrawTextW(hdc, btnSymbols[i], -1, &btnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldBtnFont);
        DeleteObject(btnFont);
    }

    EndPaint(hwnd_, &ps);
}
