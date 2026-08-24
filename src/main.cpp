#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>

#include "core/config.h"
#include "core/mpv_backend.h"
#include "core/logger.h"

// ── helpers ──────────────────────────────────────────────────────────────
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

// ── GDI overlay drawing ──────────────────────────────────────────────────
struct OverlayState {
    bool   visible = false;
    float  alpha   = 0.0f;   // 0..255
    Uint32 hideAt  = 0;      // SDL_GetTicks deadline
    bool   seeking = false;
    int    mouseX  = -1, mouseY = -1;
    // seekbar
    bool   seekingDrag = false;
    double seekTarget  = 0.0;
};

static void drawOverlay(HDC hdc, int winW, int winH, const MpvBackend& mpv,
                         const OverlayState& ov) {
    if (ov.alpha < 1.0f) return;

    int a = (int)ov.alpha;
    // 渐变背景（底部控件区）
    for (int y = winH - 120; y < winH; ++y) {
        int localA = a * (y - (winH - 120)) / 120;
        if (localA > 80) localA = 80;
        RGBQUAD clr = { 0, 0, 0, (BYTE)localA };
        RECT rc = { 0, y, winW, y + 1 };
        // 使用 AlphaBlend 需要 DIB section; 这里用简单的半透明近似
    }

    // 简单 GDI 绘制（不透明度通过 SetBkMode 模拟）
    SetBkMode(hdc, TRANSPARENT);

    // 播放状态图标（居中大图标）
    if (mpv.state() == MpvBackend::State::Paused) {
        SetTextColor(hdc, RGB(255, 255, 255));
        HFONT hFont = CreateFontW(72, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
        HFONT old = (HFONT)SelectObject(hdc, hFont);
        const wchar_t* pauseGlyph = L"\uE769";
        SIZE sz;
        GetTextExtentPoint32W(hdc, pauseGlyph, 1, &sz);
        TextOutW(hdc, (winW - sz.cx) / 2, (winH - sz.cy) / 2 - 40, pauseGlyph, 1);
        SelectObject(hdc, old);
        DeleteObject(hFont);
    }

    // 底部控制栏背景
    {
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, winW, 120);
        SelectObject(memDC, bmp);
        RECT rcBg = { 0, 0, winW, 120 };
        HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &rcBg, br);
        DeleteObject(br);

        BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)a, 0 };
        AlphaBlend(hdc, 0, winH - 120, winW, 120, memDC, 0, 0, winW, 120, bf);

        DeleteObject(bmp);
        DeleteDC(memDC);
    }

    // 进度条
    double dur = mpv.duration();
    double pos = ov.seekingDrag ? ov.seekTarget : mpv.clock();
    if (dur > 0) {
        int barY = winH - 80;
        int barH = 4;
        int barX = 20;
        int barW = winW - 40;
        // 背景
        HBRUSH brBg = CreateSolidBrush(RGB(80, 80, 80));
        RECT rcBar = { barX, barY, barX + barW, barY + barH };
        FillRect(hdc, &rcBar, brBg);
        DeleteObject(brBg);
        // 进度
        int progW = (int)(barW * pos / dur);
        if (progW > 0) {
            HBRUSH brProg = CreateSolidBrush(RGB(255, 60, 60));
            RECT rcProg = { barX, barY, barX + progW, barY + barH };
            FillRect(hdc, &rcProg, brProg);
            DeleteObject(brProg);
        }
        // 时间文本
        SetTextColor(hdc, RGB(200, 200, 200));
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hdc, hFont);
        char cur[32], tot[32];
        formatTime(cur, sizeof(cur), pos);
        formatTime(tot, sizeof(tot), dur);
        char timeStr[80];
        std::snprintf(timeStr, sizeof(timeStr), "%s / %s", cur, tot);
        TextOutA(hdc, barX, barY + 10, timeStr, (int)std::strlen(timeStr));
        // 文件名
        std::string fname = std::filesystem::path(mpv.path()).filename().string();
        TextOutA(hdc, barX, barY + 30, fname.c_str(), (int)fname.size());
        // HW decode 标记
        if (mpv.hwDecodeActive()) {
            const char* hw = "[HW]";
            TextOutA(hdc, winW - 60, barY + 10, hw, 4);
        }
        SelectObject(hdc, old);
        DeleteObject(hFont);
    }
}

// ── Win32 window for mpv ─────────────────────────────────────────────────
static HWND g_mpvHwnd = nullptr;
static MpvBackend* g_mpv = nullptr;
static OverlayState g_ov;

static LRESULT CALLBACK mpvWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_mpvHwnd) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(g_mpvHwnd, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;
    case WM_KEYDOWN:
        if (g_mpv) {
            switch (wp) {
            case VK_SPACE: g_mpv->togglePause(); break;
            case VK_LEFT:  g_mpv->seekRelative(-5.0); break;
            case VK_RIGHT: g_mpv->seekRelative(5.0); break;
            case VK_UP:    g_mpv->setVolume(g_mpv->volume() + 0.05f); break;
            case VK_DOWN:  g_mpv->setVolume(g_mpv->volume() - 0.05f); break;
            case 'M':      g_mpv->toggleMute(); break;
            case 'F': {
                DWORD style = GetWindowLongPtrW(hwnd, GWL_STYLE);
                if (style & WS_OVERLAPPEDWINDOW) {
                    SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
                    MONITORINFO mi = { sizeof(mi) };
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                    SetWindowPos(hwnd, HWND_TOP,
                        mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top,
                        SWP_FRAMECHANGED);
                } else {
                    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
                    SetWindowPos(hwnd, nullptr, 100, 100, 960, 540,
                        SWP_FRAMECHANGED | SWP_NOZORDER);
                }
                break;
            }
            case 'O':
                if (GetKeyState(VK_CONTROL) & 0x8000) {
                    std::string file = openFileDialog(hwnd);
                    if (!file.empty()) g_mpv->loadFile(file);
                }
                break;
            }
        }
        // 显示控件
        g_ov.visible = true;
        g_ov.hideAt = SDL_GetTicks() + 3000;
        g_ov.alpha = 255.0f;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        g_ov.mouseX = LOWORD(lp);
        g_ov.mouseY = HIWORD(lp);
        g_ov.visible = true;
        g_ov.hideAt = SDL_GetTicks() + 3000;
        g_ov.alpha = 255.0f;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lp), my = HIWORD(lp);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int barY = rc.bottom - 80;
        if (my >= barY - 10 && my <= barY + 30 && g_mpv && g_mpv->duration() > 0) {
            // seekbar click
            double ratio = (double)(mx - 20) / (rc.right - 40);
            if (ratio < 0) ratio = 0;
            if (ratio > 1) ratio = 1;
            g_mpv->seek(g_mpv->duration() * ratio);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (my > barY + 30) {
            // 控件区：切换暂停
            if (g_mpv) g_mpv->togglePause();
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (g_mpv) {
            short d = GET_WHEEL_DELTA_WPARAM(wp);
            g_mpv->setVolume(g_mpv->volume() + (d > 0 ? 0.05f : -0.05f));
            g_ov.visible = true;
            g_ov.hideAt = SDL_GetTicks() + 2000;
            g_ov.alpha = 255.0f;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wp;
        char path[MAX_PATH];
        if (DragQueryFileA(hDrop, 0, path, MAX_PATH)) {
            if (g_mpv) g_mpv->loadFile(path);
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_mpv && g_mpv->hasMedia()) {
            drawOverlay(hdc, ps.rcPaint.right - ps.rcPaint.left,
                       ps.rcPaint.bottom - ps.rcPaint.top, *g_mpv, g_ov);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            // overlay fade timer
            if (g_ov.visible && SDL_GetTicks() > g_ov.hideAt) {
                g_ov.alpha -= 15.0f;
                if (g_ov.alpha <= 0) {
                    g_ov.alpha = 0;
                    g_ov.visible = false;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── main ─────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    Logger::instance().init("vplayer", 7);
    bool diagMode = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--debug") { diagMode = true; break; }
    Logger::instance().setLevel(diagMode ? LogLevel::Trace : LogLevel::Warn);
    LOG_INFO("MAIN", "vplayer (mpv backend) starting");
    

    // config
    AppConfig cfg;
    loadConfig(configPath(), cfg);

    // Win32 window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = mpvWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VPlayerMpv";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        wc.lpszClassName, L"VPlayer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        LOG_ERROR("MAIN", "CreateWindow failed");
        return 1;
    }

    // DWM shadow + rounded corners
    MARGINS m = {0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd, &m);
    int pref = 2;
    DwmSetWindowAttribute(hwnd, 33, &pref, sizeof(pref));

    // icon
    {
        std::string base = exeDir();
        const char* rels[] = { "assets/icons/vplay.bmp", "ico/vplay.bmp", "ico/vplay.ico" };
        for (auto rel : rels) {
            std::string p = base + rel;
            HICON icon = (HICON)LoadImageA(nullptr, p.c_str(), IMAGE_ICON, 0, 0,
                                            LR_LOADFROMFILE | LR_DEFAULTSIZE);
            if (icon) {
                SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
                SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
                break;
            }
        }
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // mpv backend — 创建子窗口作为 mpv 渲染目标
    MpvBackend mpv;
    g_mpv = &mpv;
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_mpvHwnd = CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE,
            0, 0, rc.right, rc.bottom,
            hwnd, nullptr, wc.hInstance, nullptr);
        
        if (!mpv.init(g_mpvHwnd)) {
            LOG_ERROR("MAIN", "mpv init failed");
            return 1;
        }
    }

    // overlay timer (30fps redraw)
    SetTimer(hwnd, 1, 33, nullptr);

    // command line
    auto args = utf8Args();
    std::string initialFile;
    if (args.size() > 1 && !args[1].empty() && args[1] != "--debug") {
        initialFile = args[1];
    } else if (cfg.resume && !cfg.lastFile.empty()) {
        initialFile = cfg.lastFile;
    }

    mpv.setVolume(cfg.volume);
    if (cfg.speed >= 0.25f && cfg.speed <= 4.0f && std::abs(cfg.speed - 1.0f) > 0.01f)
        mpv.setSpeed(cfg.speed);

    mpv.onPlaybackEnded = [&]() {
        LOG_INFO("MAIN", "playback ended");
    };

    if (!initialFile.empty()) {
        mpv.loadFile(initialFile);
        // update history
        cfg.history[initialFile] = 0.0;
        cfg.lastFile = initialFile;
    }

    // message loop
    MSG msg;
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        WaitMessage();
    }

    mpv.close();
    saveConfig(configPath(), cfg);
    KillTimer(hwnd, 1);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    LOG_INFO("MAIN", "vplayer exiting");
    return 0;
}
