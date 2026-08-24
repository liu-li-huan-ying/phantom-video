#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_syswm.h>

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>

#include "core/config.h"
#include "core/mpv_backend.h"
#include "core/logger.h"
#include "ui/theme.h"
#include "ui/gdi_text.h"
#include "ui/svgicon.h"

// ---- helpers ----
static std::vector<std::string> utf8Args() {
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return out;
    for (int i = 0; i < argc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    LocalFree(wargv);
    return out;
}

static void formatTime(char* buf, size_t n, double sec) {
    int s = (int)(sec + 0.5);
    if (s < 0) s = 0;
    int h = s / 3600, m = (s % 3600) / 60, ss = s % 60;
    if (h > 0) std::snprintf(buf, n, "%d:%02d:%02d", h, m, ss);
    else       std::snprintf(buf, n, "%02d:%02d", m, ss);
}

static std::string openFileDialog(HWND hwnd) {
    char file[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        "Video\0*.mp4;*.avi;*.mkv;*.mov;*.flv;*.wmv;*.rmvb;*.rm;*.3gp;*.mpg;*.mpeg;*.webm;*.ts;*.m2ts\0"
        "All\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(file);
    return "";
}

// ---- UI state ----
static const int CONTROL_BAR_H = 100;

struct UiState {
    bool   visible   = true;
    Uint32 hideAt    = 3000;
    int    mouseX    = -1;
    int    mouseY    = -1;
    int    winW      = 960;
    int    winH      = 540;
    bool   fullscreen = false;

    // seekbar
    bool   seekbarHover  = false;
    bool   seekingDrag   = false;
    double seekTarget    = 0.0;

    // speed popup
    bool   speedMenuOpen = false;

    // volume slider
    bool   volumeSliderOpen = false;
    bool   volumeDragging   = false;

    // toast
    bool   toastActive = false;
    Uint32 toastStart  = 0;
    char   toastMsg[128] = {};

    // settings panel
    bool   settingsOpen = false;
};

// ---- globals ----
static HWND          g_parentHwnd = nullptr;
static HWND          g_mpvHwnd    = nullptr;
static MpvBackend*   g_mpv        = nullptr;
static SDL_Window*   g_sdlWin     = nullptr;
static SDL_Renderer* g_sdlRdr     = nullptr;
static GdiTextCache  g_text;
static UiState       g_ui;
static AppConfig     g_cfg;

// ---- seekbar geometry ----
static const int SB_MARGIN = 20;
static const float SPEED_PRESETS[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
static const int SPEED_PRESET_COUNT = 8;

static int sbTopY()    { return g_ui.winH - CONTROL_BAR_H; }
static int sbTrackY()  { return sbTopY() + 10; }
static int sbLeftX()   { return SB_MARGIN; }
static int sbRightX()  { return g_ui.winW - SB_MARGIN; }
static int sbWidth()   { return sbRightX() - sbLeftX(); }

static void showToast(const char* msg) {
    std::snprintf(g_ui.toastMsg, sizeof(g_ui.toastMsg), "%s", msg);
    g_ui.toastActive = true;
    g_ui.toastStart = SDL_GetTicks();
}

// ---- topbar icon hit test ----
static int hitTestTopbarIcon(int mx, int my, int winW) {
    if (my < 0 || my > ui::TOPBAR_H) return -1;
    int iconY = ui::TOPBAR_H / 2;
    int iconHalf = 12;
    int rx = winW - 20;
    struct IDef { const char* id; int idIdx; };
    static const IDef icons[] = {
        {"close", 0}, {"maximize", 1}, {"minimize", 2},
        {"list", 3}, {"pip", 4}, {"camera", 5}
    };
    for (int i = 0; i < 6; ++i) {
        if (mx >= rx - iconHalf && mx <= rx + iconHalf &&
            my >= iconY - iconHalf && my <= iconY + iconHalf)
            return icons[i].idIdx;
        rx -= 34;
    }
    return -1;
}

// ---- Win32 WndProc ----
static LRESULT CALLBACK parentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        g_ui.winW = rc.right; g_ui.winH = rc.bottom;
        if (g_mpvHwnd) MoveWindow(g_mpvHwnd, 0, 0, rc.right, rc.bottom, TRUE);
        if (g_sdlWin) {
            POINT pt = {0,0}; ClientToScreen(hwnd, &pt);
            SDL_SetWindowPosition(g_sdlWin, pt.x, pt.y);
            SDL_SetWindowSize(g_sdlWin, rc.right, rc.bottom);
        }
        return 0;
    }
    case WM_MOVE: {
        if (g_sdlWin) {
            POINT pt = {0,0}; ClientToScreen(hwnd, &pt);
            SDL_SetWindowPosition(g_sdlWin, pt.x, pt.y);
        }
        return 0;
    }

    // ---- keyboard ----
    case WM_KEYDOWN: {
        if (g_mpv) {
            switch (wp) {
            case VK_SPACE: g_mpv->togglePause(); break;
            case VK_LEFT:  g_mpv->seekRelative(-5.0); break;
            case VK_RIGHT: g_mpv->seekRelative(5.0); break;
            case VK_UP:    g_mpv->setVolume(g_mpv->volume() + 0.05f); break;
            case VK_DOWN:  g_mpv->setVolume(g_mpv->volume() - 0.05f); break;
            case 'M': {
                g_mpv->toggleMute();
                showToast(g_mpv->muted() ? "Muted" : "Unmuted");
                break;
            }
            case 'N': g_mpv->seekRelative( 10.0); break;
            case 'P': g_mpv->seekRelative(-10.0); break;
            case '[': {
                g_mpv->setSpeed(g_mpv->speed() - 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "Speed: %.2fx", g_mpv->speed());
                showToast(msg);
                break;
            }
            case ']': {
                g_mpv->setSpeed(g_mpv->speed() + 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "Speed: %.2fx", g_mpv->speed());
                showToast(msg);
                break;
            }
            case VK_ESCAPE:
                if (g_ui.speedMenuOpen) g_ui.speedMenuOpen = false;
                else if (g_ui.volumeSliderOpen) g_ui.volumeSliderOpen = false;
                break;
            case 'F': {
                DWORD sty = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
                if (sty & WS_OVERLAPPEDWINDOW) {
                    SetWindowLongPtrW(hwnd, GWL_STYLE, sty & ~WS_OVERLAPPEDWINDOW);
                    MONITORINFO mi = {sizeof(mi)};
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                    SetWindowPos(hwnd, HWND_TOP,
                        mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right  - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
                    g_ui.fullscreen = true;
                } else {
                    SetWindowLongPtrW(hwnd, GWL_STYLE, sty | WS_OVERLAPPEDWINDOW);
                    SetWindowPos(hwnd, nullptr, 100, 100, 960, 540,
                        SWP_FRAMECHANGED | SWP_NOZORDER);
                    g_ui.fullscreen = false;
                }
                break;
            }
            case 'O':
                if (GetKeyState(VK_CONTROL) & 0x8000) {
                    std::string f = openFileDialog(hwnd);
                    if (!f.empty()) g_mpv->loadFile(f);
                }
                break;
            }
        }
        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
        return 0;
    }

    // ---- mouse ----
    case WM_MOUSEMOVE: {
        g_ui.mouseX = (short)LOWORD(lp);
        g_ui.mouseY = (short)HIWORD(lp);

        int barTop = sbTopY();
        bool onSB = (g_ui.mouseY >= barTop - 6 && g_ui.mouseY <= barTop + 22 &&
                     g_ui.mouseX >= sbLeftX()   && g_ui.mouseX <= sbRightX());
        g_ui.seekbarHover = onSB;

        bool onTopbar = (g_ui.mouseY >= 0 && g_ui.mouseY <= ui::TOPBAR_H);
        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + (onTopbar ? 4000 : ui::CTRLBAR_HIDE_MS);

        // volume slider drag
        if (g_ui.volumeDragging && g_mpv) {
            int sliderW = ui::VOLSIDER_W;
            int sliderX = g_ui.winW - 54 - sliderW - 10;
            float ratio = (float)(g_ui.mouseX - sliderX) / sliderW;
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_mpv->setVolume(ratio);
        }

        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_ui.seekbarHover = false;
        g_ui.mouseX = g_ui.mouseY = -1;
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        int barTop = sbTopY();

        // --- topbar icon clicks ---
        if (my >= 0 && my <= ui::TOPBAR_H) {
            int icon = hitTestTopbarIcon(mx, my, g_ui.winW);
            switch (icon) {
            case 0: // close
                PostMessage(hwnd, WM_CLOSE, 0, 0);
                return 0;
            case 1: { // maximize / fullscreen
                DWORD sty = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
                if (sty & WS_OVERLAPPEDWINDOW) {
                    SetWindowLongPtrW(hwnd, GWL_STYLE, sty & ~WS_OVERLAPPEDWINDOW);
                    MONITORINFO mi = {sizeof(mi)};
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                    SetWindowPos(hwnd, HWND_TOP,
                        mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
                    g_ui.fullscreen = true;
                } else {
                    SetWindowLongPtrW(hwnd, GWL_STYLE, sty | WS_OVERLAPPEDWINDOW);
                    SetWindowPos(hwnd, nullptr, 100, 100, 960, 540,
                        SWP_FRAMECHANGED | SWP_NOZORDER);
                    g_ui.fullscreen = false;
                }
                return 0;
            }
            case 2: // minimize
                ShowWindow(hwnd, SW_MINIMIZE);
                return 0;
            case 3: // playlist (TODO)
                return 0;
            case 4: // PIP (TODO)
                return 0;
            case 5: // camera/screenshot (TODO)
                return 0;
            default:
                break;
            }
            // no icon hit -> window drag
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        // --- seekbar ---
        if (g_mpv && my >= barTop - 6 && my <= barTop + 22 &&
            mx >= sbLeftX() && mx <= sbRightX() && g_mpv->duration() > 0) {
            g_ui.seekingDrag = true;
            double ratio = (double)(mx - sbLeftX()) / sbWidth();
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_ui.seekTarget = g_mpv->duration() * ratio;
            SetCapture(hwnd);
        }
        // --- speed popup ---
        else if (g_ui.speedMenuOpen) {
            int menuW = 80, itemH = 30;
            int menuX = g_ui.winW - 150;
            int menuY = barTop - SPEED_PRESET_COUNT * itemH - 5;
            if (mx >= menuX && mx <= menuX + menuW && my >= menuY && my <= menuY + SPEED_PRESET_COUNT * itemH) {
                int idx = (my - menuY) / itemH;
                if (idx >= 0 && idx < SPEED_PRESET_COUNT) {
                    g_mpv->setSpeed(SPEED_PRESETS[idx]);
                    char msg[32];
                    std::snprintf(msg, sizeof(msg), "Speed: %.2fx", SPEED_PRESETS[idx]);
                    showToast(msg);
                }
            }
            g_ui.speedMenuOpen = false;
        }
        // --- speed label click (toggle speed popup) ---
        else if (g_mpv && mx >= g_ui.winW - 110 && mx <= g_ui.winW - 70 &&
                 my >= barTop + 36 && my <= barTop + 60) {
            g_ui.speedMenuOpen = !g_ui.speedMenuOpen;
        }
        // --- settings gear click ---
        else if (g_mpv && mx >= g_ui.winW - 88 && mx <= g_ui.winW - 68 &&
                 my >= barTop + 40 && my <= barTop + 60) {
            g_ui.settingsOpen = !g_ui.settingsOpen;
        }
        // --- settings modal backdrop click (close) ---
        else if (g_ui.settingsOpen && my < sbTopY()) {
            g_ui.settingsOpen = false;
        }
        // --- volume icon click ---
        else if (g_mpv && mx >= g_ui.winW - 64 && mx <= g_ui.winW - 44 &&
                 my >= barTop + 40 && my <= barTop + 60) {
            g_ui.volumeSliderOpen = !g_ui.volumeSliderOpen;
        }
        // --- volume slider drag ---
        else if (g_ui.volumeSliderOpen && g_mpv) {
            int sliderW = ui::VOLSIDER_W;
            int sliderX = g_ui.winW - 54 - sliderW - 10;
            int sliderY = barTop + 50 - 2;
            if (mx >= sliderX && mx <= sliderX + sliderW && my >= sliderY - 8 && my <= sliderY + 12) {
                g_ui.volumeDragging = true;
                float ratio = (float)(mx - sliderX) / sliderW;
                if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
                g_mpv->setVolume(ratio);
                SetCapture(hwnd);
            } else {
                g_ui.volumeSliderOpen = false;
                if (g_mpv) g_mpv->togglePause();
            }
        }
        // --- click on video area ---
        else {
            if (g_mpv) g_mpv->togglePause();
        }

        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_ui.seekingDrag) {
            g_ui.seekingDrag = false;
            if (g_mpv) g_mpv->seek(g_ui.seekTarget);
        }
        if (g_ui.volumeDragging) {
            g_ui.volumeDragging = false;
        }
        ReleaseCapture();
        return 0;

    case WM_MOUSEWHEEL: {
        if (g_mpv) {
            short d = GET_WHEEL_DELTA_WPARAM(wp);
            g_mpv->setVolume(g_mpv->volume() + (d > 0 ? 0.05f : -0.05f));
        }
        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + 2000;
        return 0;
    }

    // ---- drag-drop ----
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wp;
        char path[MAX_PATH];
        if (DragQueryFileA(hDrop, 0, path, MAX_PATH)) {
            if (g_mpv) g_mpv->loadFile(path);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- SDL2 overlay ----
static const Uint8 TRANSPARENT_R = 255;
static const Uint8 TRANSPARENT_G = 0;
static const Uint8 TRANSPARENT_B = 255;

static bool createOverlay(HWND parent, int w, int h) {
    g_sdlWin = SDL_CreateWindow("VPlayer UI",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    if (!g_sdlWin) {
        LOG_ERROR("MAIN", "SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }

    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_sdlWin, &info)) {
        LOG_ERROR("MAIN", "SDL_GetWindowWMInfo failed");
        return false;
    }
    HWND sdlHwnd = info.info.win.window;

    LONG ex = (LONG)GetWindowLongPtrW(sdlHwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(sdlHwnd, GWL_EXSTYLE,
        ex | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST);
    SetLayeredWindowAttributes(sdlHwnd, RGB(TRANSPARENT_R, TRANSPARENT_G, TRANSPARENT_B),
                               0, LWA_COLORKEY);

    g_sdlRdr = SDL_CreateRenderer(g_sdlWin, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_sdlRdr) {
        LOG_ERROR("MAIN", "SDL_CreateRenderer: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);

    g_text.init(g_sdlRdr);

    POINT pt = {0,0}; ClientToScreen(parent, &pt);
    SDL_SetWindowPosition(g_sdlWin, pt.x, pt.y);
    SDL_SetWindowSize(g_sdlWin, w, h);

    LOG_INFO("MAIN", "overlay created (%dx%d)", w, h);
    return true;
}

static void destroyOverlay() {
    g_text.shutdown();
    svgicon::shutdown();
    if (g_sdlRdr) { SDL_DestroyRenderer(g_sdlRdr); g_sdlRdr = nullptr; }
    if (g_sdlWin) { SDL_DestroyWindow(g_sdlWin);   g_sdlWin = nullptr; }
}

// ---- dithered gradient helper ----
static void drawGradientBar(SDL_Renderer* r, int x, int y, int w, int h,
                             Uint8 cr, Uint8 cg, Uint8 cb, Uint8 aTop, Uint8 aBot) {
    static const int bayer[4][4] = {
        {  0, 136,  34, 170},
        {204,  68, 238, 102},
        { 51, 187,  17, 153},
        {255, 119, 221,  85}
    };
    for (int dy = 0; dy < h; ++dy) {
        int a = aTop + (aBot - aTop) * dy / h;
        int by = dy % 4;
        for (int dx = 0; dx < w; ++dx) {
            int bx = dx % 4;
            if (a > bayer[by][bx]) {
                SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
                SDL_Rect px = {x + dx, y + dy, 1, 1};
                SDL_RenderFillRect(r, &px);
            }
        }
    }
}

// ---- rendering ----
static void renderOverlay() {
    if (!g_sdlRdr) return;

    SDL_SetRenderDrawColor(g_sdlRdr, TRANSPARENT_R, TRANSPARENT_G, TRANSPARENT_B, 255);
    SDL_RenderClear(g_sdlRdr);

    int w = g_ui.winW, h = g_ui.winH;

    if (!g_mpv || !g_mpv->hasMedia()) {
        // --- welcome page ---
        int w = g_ui.winW, h = g_ui.winH;

        // topbar still visible
        drawGradientBar(g_sdlRdr, 0, 0, w, ui::TOPBAR_H, 11, 11, 11, 220, 0);
        // title
        g_text.drawText(20, 14, "VPlayer", 14, 255, 255, 255);
        // topbar icons
        int iconY = ui::TOPBAR_H / 2;
        int rx = w - 20;
        svgicon::draw(g_sdlRdr, "close",    rx, iconY, 20, 255, 255, 255, 200); rx -= 34;
        svgicon::draw(g_sdlRdr, "maximize", rx, iconY, 20, 161, 161, 166, 200); rx -= 34;
        svgicon::draw(g_sdlRdr, "minimize", rx, iconY, 20, 161, 161, 166, 200);

        // logo
        svgicon::draw(g_sdlRdr, "play", w / 2, h / 2 - 80, 64, 37, 99, 235, 255);
        g_text.drawText(w / 2 - 40, h / 2 - 30, "VPlayer", 28, 255, 255, 255);

        // drop zone (dashed border)
        int dzW = 400, dzH = 120;
        int dzX = (w - dzW) / 2, dzY = h / 2 + 10;
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 30);
        // draw dashed border (approximate with segments)
        int dashLen = 8, gapLen = 5;
        for (int side = 0; side < 4; ++side) {
            int x0, y0, x1, y1;
            if (side == 0) { x0 = dzX; y0 = dzY; x1 = dzX + dzW; y1 = dzY; }
            else if (side == 1) { x0 = dzX + dzW; y0 = dzY; x1 = dzX + dzW; y1 = dzY + dzH; }
            else if (side == 2) { x0 = dzX + dzW; y0 = dzY + dzH; x1 = dzX; y1 = dzY + dzH; }
            else { x0 = dzX; y0 = dzY + dzH; x1 = dzX; y1 = dzY; }
            int dx = x1 - x0, dy = y1 - y0;
            float len = std::sqrt((float)(dx*dx + dy*dy));
            float nx = dx / len, ny = dy / len;
            float pos = 0;
            while (pos < len) {
                float segEnd = std::min(pos + dashLen, len);
                SDL_RenderDrawLine(g_sdlRdr,
                    (int)(x0 + nx * pos), (int)(y0 + ny * pos),
                    (int)(x0 + nx * segEnd), (int)(y0 + ny * segEnd));
                pos = segEnd + gapLen;
            }
        }
        g_text.drawText(w / 2 - 60, dzY + 30, "Drop video here", 13, 161, 161, 166);
        g_text.drawText(w / 2 - 55, dzY + 55, "or press Ctrl+O", 12, 100, 100, 100);

        // recent files grid (from config history)
        if (!g_cfg.history.empty()) {
            g_text.drawText(w / 2 - 60, dzY + dzH + 30, "Recent Files", 14, 161, 161, 166);
            int cardW = 140, cardH = 80, gap = 12;
            int cols = std::min(4, (int)g_cfg.history.size());
            int gridW = cols * cardW + (cols - 1) * gap;
            int gridX = (w - gridW) / 2;
            int gridY = dzY + dzH + 55;
            int idx = 0;
            for (auto it = g_cfg.history.rbegin(); it != g_cfg.history.rend() && idx < 8; ++it, ++idx) {
                int col = idx % cols, row = idx / cols;
                int cx = gridX + col * (cardW + gap);
                int cy = gridY + row * (cardH + gap);
                SDL_Rect cardRc = {cx, cy, cardW, cardH};
                SDL_SetRenderDrawColor(g_sdlRdr, 21, 21, 21, 255);
                SDL_RenderFillRect(g_sdlRdr, &cardRc);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 15);
                SDL_RenderDrawRect(g_sdlRdr, &cardRc);
                // file name
                std::string fn = std::filesystem::path(it->first).filename().string();
                if (fn.size() > 18) fn = fn.substr(0, 15) + "...";
                g_text.drawText(cx + 8, cy + 10, fn, 11, 200, 200, 200);
                // duration hint
                char timeBuf[16];
                formatTime(timeBuf, sizeof(timeBuf), it->second);
                g_text.drawText(cx + 8, cy + 35, timeBuf, 10, 100, 100, 100);
            }
        }

        // keyboard hints
        g_text.drawText(20, h - 30, "Space=Play/Pause  Left/Right=Seek  F=Fullscreen  M=Mute  [/]=Speed", 10, 80, 80, 80);

        SDL_RenderPresent(g_sdlRdr);
        return;
    }

    double dur = g_mpv->duration();
    double pos = g_ui.seekingDrag ? g_ui.seekTarget : g_mpv->clock();

    // --- topbar (gradient opaque->transparent from top) ---
    {
        drawGradientBar(g_sdlRdr, 0, 0, w, ui::TOPBAR_H, 11, 11, 11, 220, 0);

        // title (left)
        std::string title = g_mpv->title();
        if (title.empty()) title = "VPlayer";
        if (title.size() > 55) title = title.substr(0, 52) + "...";
        g_text.drawText(20, 14, title, 14, 255, 255, 255);

        // icons (right) - same order as design mockup
        int iconY = ui::TOPBAR_H / 2;
        int rx = w - 20;
        svgicon::draw(g_sdlRdr, "close",    rx, iconY, 20, 255, 255, 255, 200); rx -= 34;
        svgicon::draw(g_sdlRdr, "maximize", rx, iconY, 20, 161, 161, 166, 200); rx -= 34;
        svgicon::draw(g_sdlRdr, "minimize", rx, iconY, 20, 161, 161, 166, 200); rx -= 34;
        svgicon::draw(g_sdlRdr, "list",     rx, iconY, 20, 161, 161, 166, 200); rx -= 34;
        svgicon::draw(g_sdlRdr, "pip",      rx, iconY, 20, 161, 161, 166, 200); rx -= 34;
        svgicon::draw(g_sdlRdr, "camera",   rx, iconY, 20, 161, 161, 166, 200);
    }

    int barTop = sbTopY();

    // --- gradient background (top transparent -> bottom opaque) ---
    drawGradientBar(g_sdlRdr, 0, barTop, w, 60, 11, 11, 11, 0, 220);
    // solid bottom portion
    SDL_Rect solidRc = {0, barTop + 60, w, CONTROL_BAR_H - 60};
    SDL_SetRenderDrawColor(g_sdlRdr, 11, 11, 11, 240);
    SDL_RenderFillRect(g_sdlRdr, &solidRc);

    // --- seekbar (at very top of bar) ---
    if (dur > 0) {
        int tx = sbLeftX(), tw = sbWidth();
        int ty = barTop + 4;
        int th = g_ui.seekbarHover ? ui::SEEKBAR_TRACK_H_HOVER : ui::SEEKBAR_TRACK_H;

        // track background
        SDL_Rect bgRc = {tx, ty, tw, th};
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);

        // buffer fill (behind progress)
        double buf = g_mpv->bufferFill();
        if (buf > 0.0 && buf < 1.0) {
            int bufW = (int)(tw * buf);
            SDL_Rect bufRc = {tx, ty, bufW, th};
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 45);
            SDL_RenderFillRect(g_sdlRdr, &bufRc);
        }

        // progress
        int progW = (int)(tw * pos / dur);
        if (progW > 0) {
            SDL_Rect prRc = {tx, ty, progW, th};
            SDL_SetRenderDrawColor(g_sdlRdr, 37, 99, 235, 255);
            SDL_RenderFillRect(g_sdlRdr, &prRc);
        }

        // thumb (on hover or drag)
        if (g_ui.seekbarHover || g_ui.seekingDrag) {
            int cx = tx + progW;
            int cy = ty + th / 2;
            int r = ui::SEEKTHUMB_D / 2;
            SDL_Rect tRc = {cx - r, cy - r, ui::SEEKTHUMB_D, ui::SEEKTHUMB_D};
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
            SDL_RenderFillRect(g_sdlRdr, &tRc);
        }
    }

    // --- transport row: centered prev/play/next ---
    {
        int cy = barTop + 50;
        svgicon::draw(g_sdlRdr, "prev", w / 2 - 50, cy, 20, 161, 161, 166, 200);
        const char* icon = (g_mpv->state() == MpvBackend::State::Paused) ? "play" : "pause";
        svgicon::draw(g_sdlRdr, icon, w / 2, cy, ui::PLAYBTN_SIZE, 255, 255, 255, 255);
        svgicon::draw(g_sdlRdr, "next", w / 2 + 50, cy, 20, 161, 161, 166, 200);
    }

    // --- left side: time + title ---
    {
        char cur[32], tot[32], ts[80];
        formatTime(cur, sizeof(cur), pos);
        formatTime(tot, sizeof(tot), dur);
        std::snprintf(ts, sizeof(ts), "%s / %s", cur, tot);
        g_text.drawText(20, barTop + 38, ts, 14, 161, 161, 166);
    }
    {
        std::string title = g_mpv->title();
        if (title.empty()) title = std::filesystem::path(g_mpv->path()).filename().string();
        if (title.size() > 50) title = title.substr(0, 47) + "...";
        g_text.drawText(20, barTop + 60, title, 13, 255, 255, 255);
    }

    // --- right side: HW badge + speed + gear + volume + fullscreen ---
    {
        int rx = w - 20;
        // fullscreen (rightmost)
        const char* fid = g_ui.fullscreen ? "exitfull" : "full";
        svgicon::draw(g_sdlRdr, fid, rx, barTop + 50, 20, 161, 161, 166, 200);
        rx -= 34;
        // volume
        const char* vid = g_mpv->muted() ? "mute" : "volume";
        svgicon::draw(g_sdlRdr, vid, rx, barTop + 50, 20, 161, 161, 166, 200);
        rx -= 34;
        // settings gear
        svgicon::draw(g_sdlRdr, "gear", rx, barTop + 50, 20, 161, 161, 166, 200);
        rx -= 34;
        // speed label
        {
            char spd[16];
            float s = g_mpv->speed();
            if (s == (int)s) std::snprintf(spd, sizeof(spd), "%.0fx", s);
            else             std::snprintf(spd, sizeof(spd), "%.1fx", s);
            g_text.drawText(rx - 16, barTop + 42, spd, 12, 161, 161, 166);
        }
        rx -= 40;
        // HW badge
        if (g_mpv->hwDecodeActive()) {
            g_text.drawText(rx, barTop + 42, "[HW]", 11, 37, 99, 235);
        }
    }

    // --- buffering indicator ---
    if (g_mpv->bufferFill() < 0.5) {
        g_text.drawText(w / 2 - 20, barTop + 75, "Buffering...", 12, 161, 161, 166);
    }

    // --- speed popup menu ---
    if (g_ui.speedMenuOpen) {
        int menuW = 80, itemH = 30;
        int menuH = SPEED_PRESET_COUNT * itemH;
        int menuX = w - 150;
        int menuY = barTop - menuH - 5;

        // background
        SDL_Rect bgRc = {menuX, menuY, menuW, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 21, 21, 21, 245);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        // border
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
        SDL_RenderDrawRect(g_sdlRdr, &bgRc);

        float curSpeed = g_mpv->speed();
        for (int i = 0; i < SPEED_PRESET_COUNT; ++i) {
            int iy = menuY + i * itemH;
            bool highlight = (std::abs(curSpeed - SPEED_PRESETS[i]) < 0.01f);
            if (highlight) {
                SDL_Rect hlRc = {menuX + 1, iy + 1, menuW - 2, itemH - 1};
                SDL_SetRenderDrawColor(g_sdlRdr, 37, 99, 235, 255);
                SDL_RenderFillRect(g_sdlRdr, &hlRc);
            }
            char label[16];
            float sp = SPEED_PRESETS[i];
            if (sp == (int)sp) std::snprintf(label, sizeof(label), "%.0fx", sp);
            else               std::snprintf(label, sizeof(label), "%.2fx", sp);
            g_text.drawText(menuX + 20, iy + 7, label, 13, 255, 255, 255);
        }
    }

    // --- volume slider (appears left of volume icon) ---
    if (g_ui.volumeSliderOpen || g_ui.volumeDragging) {
        int sliderW = ui::VOLSIDER_W;
        int sliderH = 4;
        int volIconX = w - 54;
        int sliderX = volIconX - sliderW - 10;
        int sliderY = barTop + 50 - sliderH / 2;

        // track bg
        SDL_Rect sBg = {sliderX, sliderY, sliderW, sliderH};
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
        SDL_RenderFillRect(g_sdlRdr, &sBg);
        // filled
        float vol = g_mpv->volume();
        int fillW = (int)(sliderW * vol);
        if (fillW > 0) {
            SDL_Rect sFill = {sliderX, sliderY, fillW, sliderH};
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 200);
            SDL_RenderFillRect(g_sdlRdr, &sFill);
        }
        // thumb
        int thumbX = sliderX + fillW;
        int thumbR = 5;
        SDL_Rect tRc = {thumbX - thumbR, sliderY + sliderH/2 - thumbR, thumbR*2, thumbR*2};
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
        SDL_RenderFillRect(g_sdlRdr, &tRc);

        // volume percentage
        char vStr[16];
        std::snprintf(vStr, sizeof(vStr), "%d%%", (int)(vol * 100));
        g_text.drawText(sliderX + sliderW/2 - 10, sliderY - 16, vStr, 11, 161, 161, 166);
    }

    // --- settings modal panel ---
    if (g_ui.settingsOpen) {
        // semi-transparent backdrop
        SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 180);
        SDL_Rect fullRc = {0, 0, w, h};
        SDL_RenderFillRect(g_sdlRdr, &fullRc);

        // panel
        int panelW = 380, panelH = 360;
        int panelX = (w - panelW) / 2;
        int panelY = (h - panelH) / 2;
        SDL_Rect panelRc = {panelX, panelY, panelW, panelH};
        SDL_SetRenderDrawColor(g_sdlRdr, 21, 21, 21, 250);
        SDL_RenderFillRect(g_sdlRdr, &panelRc);
        // border
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
        SDL_RenderDrawRect(g_sdlRdr, &panelRc);

        // title
        g_text.drawText(panelX + 20, panelY + 16, "Settings", 16, 255, 255, 255);

        // close button
        svgicon::draw(g_sdlRdr, "close", panelX + panelW - 22, panelY + 22, 18, 161, 161, 166, 200);

        // setting rows
        int toggleVals[5] = {0, 0, g_cfg.resume, 0, g_cfg.subAutoLoad};
        const char* rowLabels[] = {
            "Hardware Decode (requires restart)",
            "Volume Normalization",
            "Resume Playback",
            "Auto Next",
            "Subtitle Auto-Load",
        };
        int rowY = panelY + 55;
        for (int i = 0; i < 5; ++i) {
            g_text.drawText(panelX + 20, rowY + 4, rowLabels[i], 13, 200, 200, 200);

            // toggle switch
            int swX = panelX + panelW - 60;
            int swW = 40, swH = 20;
            bool on = (toggleVals[i] != 0);
            SDL_Rect swRc = {swX, rowY, swW, swH};
            SDL_SetRenderDrawColor(g_sdlRdr, on ? 37 : 80, on ? 99 : 80, on ? 235 : 80, 255);
            SDL_RenderFillRect(g_sdlRdr, &swRc);
            // thumb
            int thumbX = on ? swX + swW - swH : swX;
            SDL_Rect tRc = {thumbX + 2, rowY + 2, swH - 4, swH - 4};
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
            SDL_RenderFillRect(g_sdlRdr, &tRc);

            rowY += 44;
        }

        // language row
        g_text.drawText(panelX + 20, rowY + 4, "Language", 13, 200, 200, 200);
        const char* langs[] = {"CN", "EN", "JP"};
        for (int i = 0; i < 3; ++i) {
            int lx = panelX + panelW - 130 + i * 40;
            SDL_Rect lr = {lx, rowY, 34, 22};
            SDL_SetRenderDrawColor(g_sdlRdr, 37, 99, 235, 255);
            SDL_RenderFillRect(g_sdlRdr, &lr);
            g_text.drawText(lx + 8, rowY + 3, langs[i], 11, 255, 255, 255);
        }
        rowY += 36;

        // theme row
        g_text.drawText(panelX + 20, rowY + 4, "Theme", 13, 200, 200, 200);
        const char* themes[] = {"Dark", "Light"};
        for (int i = 0; i < 2; ++i) {
            int lx = panelX + panelW - 100 + i * 50;
            SDL_Rect tr = {lx, rowY, 44, 22};
            SDL_SetRenderDrawColor(g_sdlRdr, i == 0 ? 37 : 80, i == 0 ? 99 : 80, i == 0 ? 235 : 80, 255);
            SDL_RenderFillRect(g_sdlRdr, &tr);
            g_text.drawText(lx + 6, rowY + 3, themes[i], 11, 255, 255, 255);
        }
    }

    // --- toast notification ---
    if (g_ui.toastActive) {
        Uint32 elapsed = SDL_GetTicks() - g_ui.toastStart;
        if (elapsed > ui::TOAST_MS) {
            g_ui.toastActive = false;
        } else {
            float alpha = 1.0f;
            if (elapsed > ui::TOAST_MS - 300)
                alpha = 1.0f - (float)(elapsed - (ui::TOAST_MS - 300)) / 300.0f;
            Uint8 a = (Uint8)(alpha * 255);
            g_text.drawText(w / 2 - 30, 70, g_ui.toastMsg, 13, 255, 255, 255);
        }
    }

    SDL_RenderPresent(g_sdlRdr);
}

// ---- main ----
int main(int argc, char** argv) {
    (void)argc; (void)argv;

    Logger::instance().init("vplayer", 7);
    bool diag = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--debug") { diag = true; break; }
    Logger::instance().setLevel(diag ? LogLevel::Trace : LogLevel::Warn);
    LOG_INFO("MAIN", "vplayer (mpv + SDL2 overlay) starting");

    loadConfig(configPath(), g_cfg);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        LOG_ERROR("MAIN", "SDL_Init: %s", SDL_GetError());
        return 1;
    }

    // ---- Win32 parent window ----
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = parentProc;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName  = L"VPlayerParent";
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    g_parentHwnd = CreateWindowExW(WS_EX_ACCEPTFILES,
        wc.lpszClassName, L"VPlayer", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_parentHwnd) { LOG_ERROR("MAIN", "CreateWindow failed"); return 1; }

    MARGINS mg = {0,0,0,0};
    DwmExtendFrameIntoClientArea(g_parentHwnd, &mg);
    int dpPref = 2;
    DwmSetWindowAttribute(g_parentHwnd, 33, &dpPref, sizeof(dpPref));

    // icon
    {
        std::string base = exeDir();
        const char* rels[] = {"assets/icons/vplay.bmp","ico/vplay.bmp","ico/vplay.ico"};
        for (auto r : rels) {
            HICON ic = (HICON)LoadImageA(nullptr, (base+r).c_str(), IMAGE_ICON, 0, 0,
                                          LR_LOADFROMFILE | LR_DEFAULTSIZE);
            if (ic) { SendMessage(g_parentHwnd, WM_SETICON, ICON_BIG, (LPARAM)ic);
                       SendMessage(g_parentHwnd, WM_SETICON, ICON_SMALL, (LPARAM)ic); break; }
        }
    }

    // ---- mpv child window ----
    RECT rc; GetClientRect(g_parentHwnd, &rc);
    g_mpvHwnd = CreateWindowExW(0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE, 0, 0, rc.right, rc.bottom,
        g_parentHwnd, nullptr, wc.hInstance, nullptr);

    MpvBackend mpv;
    g_mpv = &mpv;
    mpv.setVolume(g_cfg.volume);
    if (g_cfg.speed >= 0.25f && g_cfg.speed <= 4.0f && std::abs(g_cfg.speed - 1.0f) > 0.01f)
        mpv.setSpeed(g_cfg.speed);
    mpv.onPlaybackEnded = [](){ LOG_INFO("MAIN", "playback ended"); };

    if (!mpv.init(g_mpvHwnd)) { LOG_ERROR("MAIN", "mpv init failed"); return 1; }

    // ---- SDL2 overlay ----
    if (!createOverlay(g_parentHwnd, rc.right, rc.bottom)) { return 1; }

    ShowWindow(g_parentHwnd, SW_SHOW);
    UpdateWindow(g_parentHwnd);

    // ---- command line / resume ----
    auto args = utf8Args();
    std::string initialFile;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--debug") continue;
        if (!args[i].empty() && args[i][0] != '-') {
            initialFile = args[i];
            break;
        }
    }
    if (initialFile.empty() && g_cfg.resume && !g_cfg.lastFile.empty())
        initialFile = g_cfg.lastFile;

    if (!initialFile.empty()) {
        mpv.loadFile(initialFile);
        g_cfg.history[initialFile] = 0.0;
        g_cfg.lastFile = initialFile;
    }

    LOG_INFO("MAIN", "entering main loop");

    // ---- main loop ----
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        if (g_ui.visible && SDL_GetTicks() > g_ui.hideAt)
            g_ui.visible = false;

        renderOverlay();
        Sleep(1);
    }

    mpv.close();
    saveConfig(configPath(), g_cfg);
    destroyOverlay();
    DestroyWindow(g_parentHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    SDL_Quit();
    LOG_INFO("MAIN", "vplayer exiting");
    return 0;
}
