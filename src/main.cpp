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
#include <algorithm>
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

// ---- UTF-8 <-> UTF-16 ----
// mpv/std::string 用 UTF-8; Win32 文件 API 与 fs::path(w) 用 UTF-16。
// DragQueryFileA/fs::path(窄)/GetOpenFileNameA 都按 ANSI(GBK) 解读 ->
// 中文路径必坏, 所有边界必须显式转换
static std::wstring Utf8ToWide(const std::string& u8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, w.data(), n);
    return w;
}

static std::string WideToUtf8(const std::wstring& ws) {
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string openFileDialog(HWND hwnd) {
    wchar_t file[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        L"Video\0*.mp4;*.avi;*.mkv;*.mov;*.flv;*.wmv;*.rmvb;*.rm;*.3gp;*.mpg;*.mpeg;*.webm;*.ts;*.m2ts\0"
        L"All\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) return WideToUtf8(file);
    return "";
}

// 从 UTF-8 路径安全提取文件名（fs::path 窄构造会按 ANSI 误读）
static std::string fileNameOf(const std::string& utf8path) {
    namespace fs = std::filesystem;
    try {
        fs::path p(Utf8ToWide(utf8path));
        return WideToUtf8(p.filename().wstring());
    } catch (...) { return utf8path; }
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

    // playlist panel（右侧独立区域: 打开时窗口扩展, 视频不被遮挡）
    bool   playlistOpen = false;
    int    playlistTargetW = 0;  // target window width when playlist open
    int    playlistAnimW = 0;   // current animation width
    int    playlistScroll = 0;  // 滚动偏移(px)
    int    totalW = 960;        // 整个客户区宽(含列表区); winW 仅视频区

    // 单击暂停延迟判定（双击全屏互斥）
    bool   pendingPause = false;

    // 控件淡入淡出（M30 缓动恢复: alpha 0..1, 控制栏滑出/顶栏滑升）
    float  ctrlAlpha = 1.0f;

    // 音量滑条 hover 自动展开/收起
    Uint32 volHoverAt = 0;

    // 播放列表拖拽排序
    int    plDragFrom = -1;     // 按下的项 index
    int    plDownY = 0;        // 按下时 y
    bool   plDragging = false;
    int    plDragY = 0;         // 拖拽中鼠标 y

    // 播放列表滚动条（M33d: 悬停常驻/拖拽/轨道跳页）
    bool   sbHover = false;
    bool   sbDragging = false;
    int    sbGrabOff = 0;       // 按下点相对 bar 顶的偏移
    int    sbTrackX = -1, sbTrackY = 0, sbTrackW = 0, sbTrackH = 0;
    int    sbBarY = 0, sbBarH = 0;
    SDL_Rect plCloseRect = {};   // 列表面板头部关闭钮

    // PIP / mini mode（置顶迷你小窗）
    bool   miniMode  = false;
    RECT   savedRect  = {};     // 还原用窗口 rect
    DWORD  savedStyle = 0;      // 还原用 style

    // OSD 信息叠加
    bool   osdActive = false;
    Uint32 osdStart  = 0;
};

// ---- globals ----
static HWND          g_parentHwnd = nullptr;
static HWND          g_mpvHwnd    = nullptr;
static HWND          g_overlayHwnd = nullptr;   // overlay 原生句柄(z序调整用)

// ---- 按需渲染 ----
// 任何 Win32 消息都可能改变视觉状态 -> proc 入口置脏;
// 播放进度秒变/定时到期在主循环轮询置脏; 空闲时 MsgWait 阻塞零占用
#include <atomic>
static std::atomic<bool> g_dirty{ true };
static MpvBackend*   g_mpv        = nullptr;
static SDL_Window*   g_sdlWin     = nullptr;
static SDL_Renderer* g_sdlRdr     = nullptr;
static GdiTextCache  g_text;
static UiState       g_ui;
static AppConfig     g_cfg;

// ---- mpv 子窗口鼠标/键盘消息中继 ----
// overlay(WS_EX_TRANSPARENT) 点击会命中 mpv 的 STATIC 子窗口而非 parent，
// 导致所有鼠标交互失效；此处把输入类消息转发给 parent 统一处理。
// mpv 子窗口与 parent 客户区完全重合(0,0)，lParam 客户坐标可直接透传。
static WNDPROC g_mpvOldProc = nullptr;
static LRESULT CALLBACK mpvRelayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
        SetFocus(g_parentHwnd);   // 键盘焦点收回 parent
        [[fallthrough]];
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR:
        return SendMessageW(g_parentHwnd, msg, wp, lp);
    }
    return CallWindowProcW(g_mpvOldProc, hwnd, msg, wp, lp);
}

// ---- seekbar geometry ----
static const int SB_MARGIN = 20;
static const float SPEED_PRESETS[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
static const int SPEED_PRESET_COUNT = 8;
static const int TIMER_SINGLECLICK = 2;   // 单击暂停延迟定时器

// ---- DPI 缩放 ----
// g_dpi = 当前显示器 DPI/96。像素度量(图标/边距/条高)经 S() 缩放；
// 文字 pt 不缩放——GdiTextCache 内部已按 LOGPIXELSY 换算，双重缩放会过大。
static float g_dpi = 1.0f;
static int S(int v) { return (int)(v * g_dpi + 0.5f); }

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static void updateDpiForWindow(HWND hwnd) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        using Fn = UINT(WINAPI*)(HWND);
        auto f = (Fn)(void*)GetProcAddress(u32, "GetDpiForWindow");
        if (f) { g_dpi = f(hwnd) / 96.0f; return; }
    }
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, dc);
}

static int sbTopY()    { return g_ui.winH - S(CONTROL_BAR_H); }
static int sbTrackY()  { return sbTopY() + S(10); }
static int sbLeftX()   { return S(SB_MARGIN); }
static int sbRightX()  { return g_ui.winW - S(SB_MARGIN); }
static int sbWidth()   { return sbRightX() - sbLeftX(); }

// ---- 窗口位置保存（须在窗口销毁前调用） ----
static void saveWindowPos(HWND hwnd) {
    if (!IsWindow(hwnd)) return;
    if (g_ui.fullscreen || g_ui.miniMode || IsIconic(hwnd) || IsZoomed(hwnd)) return;
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return;
    // 防御: 异常尺寸不入库
    if (wr.right - wr.left < S(300) || wr.bottom - wr.top < S(200)) return;
    g_cfg.posX = wr.left; g_cfg.posY = wr.top;
    g_cfg.posW = wr.right - wr.left; g_cfg.posH = wr.bottom - wr.top;
}

// ---- 控制栏 row1 布局（渲染/命中共用单一事实来源）----
// 效果图: [prev][PLAY白底42][next][time] ...spacer... [字幕][倍速 x.x][至臻画质][音量wrap][设置][全屏]
struct Row1Layout {
    SDL_Rect prev, play, next;         // 按钮
    int timeX;                          // 时间文本左缘
    SDL_Rect subBtn, speedBtn, qualityBtn, setBtn, fullBtn;
    int volIconCx;                      // 音量图标中心
    int volSliderX, volSliderW;        // 滑条(展开态)
    int cy;                             // 行中心 y
};

static void layoutRow1(int w, int h, bool volOpen, Row1Layout& L) {
    const int pad = S(16);
    int cy = h - S(CONTROL_BAR_H) + S(50);
    L.cy = cy;
    int x = pad;
    auto iconBtn = [&](SDL_Rect& rc) {
        rc = {x, cy - S(17), S(34), S(34)};
        x += S(34);
    };
    iconBtn(L.prev);
    L.play = {x, cy - S(21), S(42), S(42)};
    x += S(42) + S(2);
    iconBtn(L.next);
    // time
    L.timeX = x + S(10);
    char tbuf[80];
    formatTime(tbuf, sizeof(tbuf), 0);
    std::string sample = std::string(tbuf) + " / " + tbuf;
    x += S(10) + g_text.measureText(sample, 12) + S(12);

    // 右侧组: 从右往左
    int xr = w - pad;
    auto placeRight = [&](SDL_Rect& rc, int bw) {
        xr -= bw;
        rc = {xr, cy - S(17), bw, S(34)};
        xr -= S(4);
    };
    placeRight(L.fullBtn, S(34));
    placeRight(L.setBtn, S(58));                 // "设置"+gear
    L.volSliderW = volOpen ? S(80) : 0;
    placeRight(L.qualityBtn, g_text.measureText("至臻画质", 12) + S(18));
    {
        char spd[16];
        float s = g_mpv ? g_mpv->speed() : 1.0f;
        if (s == (int)s) std::snprintf(spd, sizeof(spd), "%.0fx", s);
        else             std::snprintf(spd, sizeof(spd), "%.2fx", s);
        placeRight(L.speedBtn, g_text.measureText(spd, 12) + g_text.measureText("倍速", 12) + S(24));
    }
    placeRight(L.subBtn, g_text.measureText("字幕", 12) + S(26));
    // 音量 wrap: slider(open) + icon
    L.volIconCx = xr - S(17);
    if (volOpen) { L.volSliderX = xr - S(34) - S(80) + S(5); xr -= S(84); }
}

static void showToast(const char* msg) {
    std::snprintf(g_ui.toastMsg, sizeof(g_ui.toastMsg), "%s", msg);
    g_ui.toastActive = true;
    g_ui.toastStart = SDL_GetTicks();
}

// ---- OSD：mpv 属性查询 ----
static std::string mpvStr(const char* prop) {
    if (!g_mpv || !g_mpv->mpv()) return {};
    char* s = mpv_get_property_string(g_mpv->mpv(), prop);
    if (!s) return {};
    std::string r(s);
    mpv_free(s);
    return r;
}

static std::string formatBitrate(const std::string& bpsStr) {
    long long bps = std::atoll(bpsStr.c_str());
    if (bps <= 0) return "";
    char buf[32];
    if (bps >= 1000000) std::snprintf(buf, sizeof(buf), "%.1f Mbps", bps / 1000000.0);
    else                std::snprintf(buf, sizeof(buf), "%d kbps", (int)(bps / 1000));
    return buf;
}

// 运行时写 mpv 属性（字符串）
static void mpvSetOpt(const char* prop, const char* val) {
    if (!g_mpv || !g_mpv->mpv()) return;
    int r = mpv_set_property_string(g_mpv->mpv(), prop, val);
    LOG_DBG("MAIN", "set %s=%s ret=%d", prop, val, r);
}

// 按 volNorm/nightMode 重建 af 滤镜链
static void rebuildAudioFilters() {
    std::string af;
    if (g_cfg.volNorm) af += "loudnorm";
    if (g_cfg.nightMode) {
        if (!af.empty()) af += ",";
        af += "@night:acompressor=threshold=-25dB:ratio=6";
    }
    mpvSetOpt("af", af.c_str());
}

// 运动插值：display-resample 时钟 + oversample（低开销去 judder）
static void applyMotionInterp(bool on) {
    mpvSetOpt("video-sync", on ? "display-resample" : "audio");
    mpvSetOpt("interpolation", on ? "yes" : "no");
    if (on) mpvSetOpt("tscale", "oversample");
}

// ---- 设置面板：几何与行定义（渲染/命中共用） ----
struct SettingsGeom {
    int panelX, panelY, panelW, panelH;
    int closeCx, closeCy, closeR;
    int swX, swW, swH;          // 开关
    int rowY[9];                // 9 个开关行
    int modeRowY;               // 播放模式行
    int chipY, chipH, chipW;    // 模式 chips
};
static const int SET_ROW_COUNT = 9;

static SettingsGeom settingsGeom(int w, int h) {
    SettingsGeom g;
    g.panelW = S(400); g.panelH = S(500);
    g.panelX = (w - g.panelW) / 2;
    g.panelY = (h - g.panelH) / 2;
    g.closeCx = g.panelX + g.panelW - S(22);
    g.closeCy = g.panelY + S(22);
    g.closeR = S(12);
    g.swX = g.panelX + g.panelW - S(60);
    g.swW = S(40); g.swH = S(20);
    for (int i = 0; i < SET_ROW_COUNT; ++i)
        g.rowY[i] = g.panelY + S(55) + i * S(40);
    g.modeRowY = g.rowY[7] + S(44);
    g.chipY = g.modeRowY;
    g.chipH = S(24); g.chipW = S(56);
    return g;
}

// 应用设置变更到 mpv（开关翻转时调用）
static void applySetting(const char* key, int value) {
    if      (std::strcmp(key, "hw") == 0)      mpvSetOpt("hwdec", value ? "auto-safe" : "no");
    else if (std::strcmp(key, "sub") == 0)     mpvSetOpt("sub-auto", value ? "fuzzy" : "no");
    else if (std::strcmp(key, "excl") == 0)    mpvSetOpt("audio-exclusive", value ? "yes" : "no");
    else if (std::strcmp(key, "vol") == 0 ||
             std::strcmp(key, "night") == 0)   rebuildAudioFilters();
    else if (std::strcmp(key, "interp") == 0)  applyMotionInterp(value != 0);
    else if (std::strcmp(key, "hiq") == 0) {
        const char* s = value ? "ewa_lanczossharp" : "spline36";
        mpvSetOpt("scale", s);
        mpvSetOpt("cscale", s);
    }
    // thumbCache/resume 纯本地，无需通知 mpv
}

// ---- overlay z 序 ----
static void raiseOverlayAbove() {
    if (!g_overlayHwnd) return;
    // parent 变 TOPMOST/还原后，把 overlay 重新提到同层最上
    SetWindowPos(g_overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// 播放列表开/关 -> 主窗口宽度增减 S(430)（右侧独立区域，不遮挡视频）
// 注意: 无边框窗口的 GetWindowRect 比 client 多出隐藏边框(~18px),
// 必须以 client 增量换算回 window 增量, 否则列表区宽度漂移
static void applyPlaylistWindow(HWND hwnd) {
    RECT rc, wr;
    GetClientRect(hwnd, &rc);
    GetWindowRect(hwnd, &wr);
    int frameW = (wr.right - wr.left) - (rc.right - rc.left);
    int frameH = (wr.bottom - wr.top) - (rc.bottom - rc.top);
    int newWinW = (rc.right - rc.left) + (g_ui.playlistOpen ? S(430) : -S(430)) + frameW;
    SetWindowPos(hwnd, nullptr, 0, 0, newWinW,
                 (rc.bottom - rc.top) + frameH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // WM_SIZE 中按 playlistOpen 分配视频区/列表区
}

// ---- 全屏切换（无边框窗口：仅移动窗口至显示器尺寸，不改样式） ----
static void toggleFullscreen(HWND hwnd) {
    if (!g_ui.fullscreen) {
        GetWindowRect(hwnd, &g_ui.savedRect);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_ui.fullscreen = true;
        LOG_INFO("MAIN", "fullscreen ON");
    } else {
        SetWindowPos(hwnd, nullptr,
            g_ui.savedRect.left, g_ui.savedRect.top,
            g_ui.savedRect.right - g_ui.savedRect.left,
            g_ui.savedRect.bottom - g_ui.savedRect.top,
            SWP_NOZORDER | SWP_FRAMECHANGED);
        g_ui.fullscreen = false;
        LOG_INFO("MAIN", "fullscreen OFF");
    }
    raiseOverlayAbove();
}

static void toggleMini(HWND hwnd) {
    if (!g_ui.miniMode) {
        GetWindowRect(hwnd, &g_ui.savedRect);
        int w = S(480), h = S(270);
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(hwnd, HWND_TOPMOST,
            wa.right - w - S(20), wa.bottom - h - S(20), w, h,
            SWP_NOZORDER | SWP_NOACTIVATE);
        g_ui.miniMode = true;
        LOG_INFO("MAIN", "pip mini ON (%dx%d)", w, h);
        showToast("Picture-in-picture: ON");
    } else {
        SetWindowPos(hwnd, HWND_NOTOPMOST,
            g_ui.savedRect.left, g_ui.savedRect.top,
            g_ui.savedRect.right  - g_ui.savedRect.left,
            g_ui.savedRect.bottom - g_ui.savedRect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        g_ui.miniMode = false;
        LOG_INFO("MAIN", "pip mini OFF");
        showToast("Picture-in-picture: OFF");
    }
    raiseOverlayAbove();
}

// ---- 播放队列（稳定顺序，文件夹扫描生成，不随播放重排） ----
static std::vector<std::string> g_playlist;

static void clampPlaylistScroll() {
    int contentH = (int)g_playlist.size() * S(72);
    int viewH = g_ui.winH - S(ui::TOPBAR_H) - S(55);
    if (g_ui.playlistScroll > contentH - viewH) g_ui.playlistScroll = contentH - viewH;
    if (g_ui.playlistScroll < 0) g_ui.playlistScroll = 0;
}

static const char* kVideoExts[] = {
    ".mp4",".mkv",".avi",".mov",".flv",".wmv",".webm",".ts",".m2ts",
    ".rmvb",".rm",".3gp",".mpg",".mpeg"
};
static const size_t PLAYLIST_MAX = 2000;

// 自然排序: 数字段按数值比较(V2<V10), 非数字段按码点; 行为对齐资源管理器
static bool naturalLess(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        wchar_t ca = a[i], cb = b[j];
        bool da = (ca >= L'0' && ca <= L'9');
        bool db = (cb >= L'0' && cb <= L'9');
        if (da && db) {
            size_t ie = i, je = j;
            while (ie < a.size() && a[ie] >= L'0' && a[ie] <= L'9') ++ie;
            while (je < b.size() && b[je] >= L'0' && b[je] <= L'9') ++je;
            size_t zi = i, zj = j;                       // 跳前导零
            while (zi + 1 < ie && a[zi] == L'0') ++zi;
            while (zj + 1 < je && b[zj] == L'0') ++zj;
            if (ie - zi != je - zj) return ie - zi < je - zj;   // 数值长度
            int c = a.compare(zi, ie - zi, b, zj, je - zj);
            if (c != 0) return c < 0;
            // 数值相等: 前导零少者在前(01<001)
            if (ie - i != je - j) return ie - i < je - j;
            i = ie; j = je;
        } else {
            wchar_t la = ca, lb = cb;
            if (la >= L'A' && la <= L'Z') la += 32;
            if (lb >= L'A' && lb <= L'Z') lb += 32;
            if (la != lb) return la < lb;
            if (ca != cb) return ca < cb;                // 大小写稳定序
            ++i; ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

// 以 file 所在目录扫描视频文件构建播放队列（自然顺序）
static void buildPlaylistAround(const std::string& file) {
    namespace fs = std::filesystem;
    g_playlist.clear();
    fs::path p(Utf8ToWide(file));                       // 宽字符构造, 杜绝 ANSI 误读
    fs::path dir = p.parent_path();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) { g_playlist.push_back(file); return; }
    std::vector<fs::path> found;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (found.size() >= PLAYLIST_MAX) break;
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().string();   // 扩展名 ASCII, ANSI 读取安全
        for (auto* ve : kVideoExts) {
            if (_stricmp(ext.c_str(), ve) == 0) { found.push_back(e.path()); break; }
        }
    }
    if (found.empty()) { g_playlist.push_back(file); return; }
    std::sort(found.begin(), found.end(), [](const fs::path& x, const fs::path& y) {
        return naturalLess(x.filename().wstring(), y.filename().wstring());
    });
    for (auto& f : found) g_playlist.push_back(WideToUtf8(f.wstring()));   // 宽->UTF-8
    if (!g_playlist.empty()) {
        auto nameOf = [](const std::string& p) {
            size_t s = p.find_last_of("\\/");
            return (s == std::string::npos) ? p : p.substr(s + 1);
        };
        LOG_INFO("MAIN", "sorted[0..2]: %s | %s | %s",
                 nameOf(g_playlist[0]).c_str(),
                 nameOf(g_playlist.size() > 1 ? g_playlist[1] : "").c_str(),
                 nameOf(g_playlist.size() > 2 ? g_playlist[2] : "").c_str());
    }
}

static int playlistIndexOf(const std::string& path) {
    for (size_t i = 0; i < g_playlist.size(); ++i)
        if (g_playlist[i] == path) return (int)i;
    return -1;
}

// ---- 缩略图服务：worker 解码出 RGB，渲染线程惰性上传为纹理 ----
#include "core/thumbnail_extractor.h"

static std::mutex g_thumbMtx;
static std::vector<std::string> g_thumbWant;                   // 当前可见待提取集合
struct ThumbRgb { int w = 0, h = 0; std::vector<uint8_t> px; };
static std::map<std::string, ThumbRgb> g_thumbRgb;              // path -> RGB24(空 px=失败标记)
static std::map<std::string, SDL_Texture*> g_thumbTex;          // 渲染线程专用
static std::atomic<bool> g_thumbQuit{false};
static std::thread g_thumbThread;

// ---- 缩略图磁盘缓存：exe/cache/thumbs/<fnv1a64>.bin = "VPT1"+w+h+RGB24 ----
static std::string thumbCacheDir() {
    return exeDir() + "cache\\thumbs";
}

// 启动时清理超过 keepDays 天的缓存文件
static void thumbCacheCleanup(int keepDays) {
    if (keepDays <= 0) keepDays = 7;
    std::string dir = thumbCacheDir();
    WIN32_FIND_DATAA fd;
    std::string pattern = dir + "\\*.bin";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ulNow = {{now.dwLowDateTime, now.dwHighDateTime}};
    int removed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER ulFile = {{fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}};
        long long ageDays = (long long)(ulNow.QuadPart - ulFile.QuadPart) / (10000000LL * 86400);
        if (ageDays > keepDays) {
            DeleteFileA((dir + "\\" + fd.cFileName).c_str());
            ++removed;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (removed) LOG_INFO("MAIN", "thumb cache cleanup: removed %d files", removed);
}

static uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

static std::string thumbDiskPath(const std::string& path) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx.bin", (unsigned long long)fnv1a64(path));
    return thumbCacheDir() + "\\" + buf;
}

// 命中返回 true 并填充 out；文件损坏则删除
static bool thumbDiskLoad(const std::string& path, ThumbRgb& out) {
    FILE* f = fopen(thumbDiskPath(path).c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    int w = 0, h = 0;
    bool ok = false;
    do {
        if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "VPT1", 4) != 0) break;
        if (fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1) break;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) break;
        size_t need = (size_t)w * h * 3;
        out.px.resize(need);
        if (fread(out.px.data(), 1, need, f) != need) { out.px.clear(); break; }
        out.w = w; out.h = h;
        ok = true;
    } while (false);
    fclose(f);
    if (!ok) DeleteFileA(thumbDiskPath(path).c_str());   // 损坏即删
    return ok;
}

static void thumbDiskSave(const std::string& path, const ThumbRgb& t) {
    if (t.px.empty()) return;
    FILE* f = fopen(thumbDiskPath(path).c_str(), "wb");
    if (!f) return;
    fwrite("VPT1", 1, 4, f);
    fwrite(&t.w, 4, 1, f);
    fwrite(&t.h, 4, 1, f);
    fwrite(t.px.data(), 1, t.px.size(), f);
    fclose(f);
}

static void thumbWorkerMain() {
    ThumbnailExtractor ex;
    while (!g_thumbQuit.load()) {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            for (auto& p : g_thumbWant) {
                if (!g_thumbRgb.count(p)) { path = p; break; }
            }
            if (!path.empty())
                g_thumbWant.erase(std::remove(g_thumbWant.begin(), g_thumbWant.end(), path),
                                  g_thumbWant.end());
        }
        if (path.empty()) { Sleep(150); continue; }

        ThumbRgb out;
        bool diskHit = false;
        if (g_cfg.thumbCache) {
            diskHit = thumbDiskLoad(path, out);
            if (diskHit) LOG_DBG("MAIN", "thumb disk hit %s", path.c_str());
        }
        if (!diskHit) {
            uint8_t* px = nullptr; int w = 0, h = 0;
            if (ex.open(path) && ex.getFrame(3.0, &px, w, h) && px && w > 0 && h > 0) {
                out.w = w; out.h = h;
                out.px.assign(px, px + (size_t)w * h * 3);
                ThumbnailExtractor::freePixels(px);
                if (g_cfg.thumbCache) thumbDiskSave(path, out);
                LOG_DBG("MAIN", "thumb ok %dx%d %s", w, h, path.c_str());
            } else {
                LOG_DBG("MAIN", "thumb fail %s", path.c_str());
            }
            ex.close();
        }
        {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            g_thumbRgb[path] = std::move(out);   // 失败也记空标记，避免反复重试
        }
    }
}

// 渲染线程调用：把就绪的 RGB 转成纹理
static void uploadThumbs(SDL_Renderer* r) {
    std::lock_guard<std::mutex> lk(g_thumbMtx);
    for (auto it = g_thumbRgb.begin(); it != g_thumbRgb.end(); ) {
        auto& t = it->second;
        if (t.px.empty()) { ++it; continue; }                 // 失败标记跳过
        if (g_thumbTex.count(it->first)) { it = g_thumbRgb.erase(it); continue; }
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, t.w, t.h, 24, SDL_PIXELFORMAT_RGB24);
        if (surf) {
            SDL_LockSurface(surf);
            for (int y = 0; y < t.h; ++y)
                memcpy((uint8_t*)surf->pixels + y * surf->pitch,
                       t.px.data() + (size_t)y * t.w * 3, (size_t)t.w * 3);
            SDL_UnlockSurface(surf);
            SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
            SDL_FreeSurface(surf);
            if (tex) {
                g_thumbTex[it->first] = tex;
                it = g_thumbRgb.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// 统一播放入口：记录待续播位置 + 更新 lastFile + 面板跟随当前项
static double g_pendingResumePos = -1.0;   // >0 表示 FILE_LOADED 后 seek 到此
static void playPath(const std::string& path) {
    if (!g_mpv || path.empty()) return;
    g_pendingResumePos = -1.0;
    auto it = g_cfg.history.find(path);
    if (g_cfg.resume && it != g_cfg.history.end() && it->second > 1.0)
        g_pendingResumePos = it->second;
    g_mpv->loadFile(path);
    g_cfg.lastFile = path;

    // 播放列表面板打开时，滚动到当前项附近
    if (g_ui.playlistOpen) {
        int idx = playlistIndexOf(path);
        if (idx >= 0) {
            int itemH = S(72);
            int viewH = g_ui.winH - S(ui::TOPBAR_H) - S(55);
            int target = idx * itemH - viewH / 2 + itemH / 2;
            int contentH = (int)g_playlist.size() * itemH;
            if (target < 0) target = 0;
            if (contentH > viewH && target > contentH - viewH) target = contentH - viewH;
            g_ui.playlistScroll = target;
        }
    }
}

static void playIndex(int idx, bool relative = false) {
    if (relative) {
        int cur = playlistIndexOf(g_mpv ? g_mpv->path() : "");
        if (cur < 0) cur = 0;
        idx = cur + idx;
    }
    if (idx < 0 || idx >= (int)g_playlist.size()) return;
    playPath(g_playlist[idx]);
}


// ---- 音量交互区（图标+滑条范围） ----
static bool inVolumeArea(int mx, int my) {
    int barTop = sbTopY();
    return my >= barTop + S(34) && my <= barTop + S(66) &&
           mx >= g_ui.winW - S(170) && mx <= g_ui.winW - S(38);
}

// ---- topbar icon hit test ----
static int hitTestTopbarIcon(int mx, int my, int winW) {
    if (my < 0 || my > S(ui::TOPBAR_H)) return -1;
    int iconY = S(ui::TOPBAR_H) / 2;
    int iconHalf = S(12);
    int rx = winW - S(20);
    struct IDef { const char* id; int idIdx; };
    static const IDef icons[] = {
        {"close", 0}, {"maximize", 1}, {"minimize", 2},
        {"list", 3}, {"pip", 4}, {"camera", 5}
    };
    for (int i = 0; i < 6; ++i) {
        if (mx >= rx - iconHalf && mx <= rx + iconHalf &&
            my >= iconY - iconHalf && my <= iconY + iconHalf)
            return icons[i].idIdx;
        rx -= S(34);
    }
    return -1;
}

// ---- Win32 WndProc ----
static LRESULT CALLBACK parentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    g_dirty.store(true);   // 任何消息都视为潜在视觉变化（入口统一置脏）
    switch (msg) {

    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0;
        RECT rc; GetClientRect(hwnd, &rc);
        g_ui.totalW = rc.right;
        // 列表面板打开(非全屏)时: 右侧独立区域, mpv/overlay 只占视频区
        int panelExtra = (g_ui.playlistOpen && !g_ui.fullscreen) ? S(430) : 0;
        g_ui.winW = rc.right - panelExtra;
        g_ui.winH = rc.bottom;
        LOG_DBG("MAIN", "WM_SIZE %ux%u wp=%u panelExtra=%d winW=%d",
                rc.right, rc.bottom, wp, panelExtra, g_ui.winW);
        if (g_mpvHwnd) MoveWindow(g_mpvHwnd, 0, 0, g_ui.winW, rc.bottom, TRUE);
        if (g_sdlWin) {
            // overlay 覆盖整个客户区(含右侧列表区); 视频区布局由 winW 约束
            POINT pt = {0,0}; ClientToScreen(hwnd, &pt);
            SDL_SetWindowPosition(g_sdlWin, pt.x, pt.y);
            SDL_SetWindowSize(g_sdlWin, rc.right, rc.bottom);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        // 显示器 DPI 变化（拖到不同缩放屏/改系统缩放）
        int newDpi = HIWORD(wp);
        g_dpi = newDpi / 96.0f;
        RECT* prc = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                     prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        LOG_INFO("MAIN", "dpi changed to %d (scale %.2f)", newDpi, g_dpi);
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
            case '[':
            case 0xDB:   // VK_OEM_4: Windows 对 [ 键发的虚拟码(≠ASCII 0x5B)
            {
                g_mpv->setSpeed(g_mpv->speed() - 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "Speed: %.2fx", g_mpv->speed());
                showToast(msg);
                break;
            }
            case ']':
            case 0xDD:   // VK_OEM_6
            {
                g_mpv->setSpeed(g_mpv->speed() + 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "Speed: %.2fx", g_mpv->speed());
                showToast(msg);
                break;
            }
            case 'C': {
                bool vis = !g_mpv->subVisible();
                g_mpv->setSubVisibility(vis);
                showToast(vis ? "Subtitles ON" : "Subtitles OFF");
                break;
            }
            case 'X': {
                g_mpv->addSubDelay(-0.5);
                char msg[40];
                std::snprintf(msg, sizeof(msg), "Sub delay: %.1fs", -g_mpv->subDelay());
                showToast(msg);
                break;
            }
            case 'Z': {
                g_mpv->addSubDelay(0.5);
                char msg[40];
                std::snprintf(msg, sizeof(msg), "Sub delay: %.1fs", -g_mpv->subDelay());
                showToast(msg);
                break;
            }
            case 'I':
                g_ui.osdActive = !g_ui.osdActive;
                g_ui.osdStart = SDL_GetTicks();
                LOG_DBG("MAIN", "osd -> %d", g_ui.osdActive ? 1 : 0);
                break;
            case VK_ESCAPE:
                if (g_ui.speedMenuOpen) g_ui.speedMenuOpen = false;
                else if (g_ui.volumeSliderOpen) g_ui.volumeSliderOpen = false;
                break;
            case 'F':
                toggleFullscreen(hwnd);
                break;
            case 'O':
                if (GetKeyState(VK_CONTROL) & 0x8000) {
                    std::string f = openFileDialog(hwnd);
                    if (!f.empty()) { buildPlaylistAround(f); playPath(f); }
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
        bool onSB = (g_ui.mouseY >= barTop - S(6) && g_ui.mouseY <= barTop + S(22) &&
                     g_ui.mouseX >= sbLeftX()   && g_ui.mouseX <= sbRightX());
        g_ui.seekbarHover = onSB;

        bool onTopbar = (g_ui.mouseY >= 0 && g_ui.mouseY <= S(ui::TOPBAR_H));
        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + (onTopbar ? 4000 : ui::CTRLBAR_HIDE_MS);

        // 音量滑条 hover 自动展开；离开 1.2s 后收起（拖拽中不收）
        if (inVolumeArea(g_ui.mouseX, g_ui.mouseY)) {
            if (!g_ui.volumeSliderOpen) {
                g_ui.volumeSliderOpen = true;
                LOG_DBG("MAIN", "volume slider hover-expand");
            }
            g_ui.volHoverAt = SDL_GetTicks();
        } else if (g_ui.volumeSliderOpen && !g_ui.volumeDragging &&
                   SDL_GetTicks() > g_ui.volHoverAt + 1200) {
            g_ui.volumeSliderOpen = false;
            LOG_DBG("MAIN", "volume slider auto-collapse");
        }

        // 列表滚动条: hover 高亮 + 拖拽滚动
        {
            bool over = g_ui.sbTrackX >= 0 &&
                        g_ui.mouseX >= g_ui.sbTrackX - S(5) &&
                        g_ui.mouseX <= g_ui.sbTrackX + g_ui.sbTrackW + S(5) &&
                        g_ui.mouseY >= g_ui.sbTrackY &&
                        g_ui.mouseY <= g_ui.sbTrackY + g_ui.sbTrackH;
            g_ui.sbHover = over || g_ui.sbDragging;
            if (g_ui.sbDragging && g_ui.sbTrackX >= 0) {
                int contentH = (int)g_playlist.size() * S(72);
                int viewH = g_ui.sbTrackH;
                if (viewH - g_ui.sbBarH > 0) {
                    g_ui.playlistScroll =
                        (g_ui.mouseY - g_ui.sbGrabOff - g_ui.sbTrackY) *
                        (contentH - viewH) / (viewH - g_ui.sbBarH);
                    clampPlaylistScroll();
                    g_dirty.store(true);
                }
            }
        }

        // 列表拖拽排序：位移超阈值进入拖拽态
        if (!g_ui.sbDragging && g_ui.plDragFrom >= 0 && !g_ui.plDragging &&
            std::abs(g_ui.mouseY - g_ui.plDownY) > S(8)) {
            g_ui.plDragging = true;
            LOG_DBG("MAIN", "playlist drag start from=%d", g_ui.plDragFrom);
        }
        if (g_ui.plDragging) {
            g_ui.plDragY = g_ui.mouseY;
            // 自动滚动：拖到面板上下边缘时滚动列表
            int panelTop = S(ui::TOPBAR_H), panelBottom = g_ui.winH - S(10);
            if (g_ui.mouseY < panelTop + S(30) && g_ui.playlistScroll > 0)
                g_ui.playlistScroll -= S(12);
            int contentH = (int)g_playlist.size() * S(72);
            int viewH = g_ui.winH - S(ui::TOPBAR_H) - S(55);
            if (g_ui.mouseY > panelBottom - S(30) && g_ui.playlistScroll < contentH - viewH)
                g_ui.playlistScroll += S(12);
        }

        // volume slider drag
        if (g_ui.volumeDragging && g_mpv) {
            Row1Layout L;
            layoutRow1(g_ui.winW, g_ui.winH, true, L);
            float ratio = (float)(g_ui.mouseX - L.volSliderX) / S(70);
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

    case WM_NCCALCSIZE: {
        // 无边框自绘：客户区=整个窗口(移除系统标题栏)，保留 DWM 阴影
        if (!wp) break;
        auto* params = (NCCALCSIZE_PARAMS*)lp;
        if (IsZoomed(hwnd)) {   // 最大化时收进屏幕边框厚度
#ifndef SM_CXPADDEDBORDER
#define SM_CXPADDEDBORDER 92
#endif
            int fx = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            int fy = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            params->rgrc[0].left   += fx;
            params->rgrc[0].right  -= fx;
            params->rgrc[0].top    += fy;
            params->rgrc[0].bottom -= fy;
        }
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        RECT rc; GetClientRect(hwnd, &rc);
        // 边缘缩放区（非全屏/迷你）
        if (!g_ui.fullscreen && !g_ui.miniMode) {
            const int m = S(6);
            bool L = pt.x < m, R = pt.x >= rc.right - m;
            bool T = pt.y < m, B = pt.y >= rc.bottom - m;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
        }
        // 顶栏：非图标区作为拖拽把手
        if (pt.y >= 0 && pt.y <= S(ui::TOPBAR_H)) {
            if (hitTestTopbarIcon(pt.x, pt.y, g_ui.winW) < 0)
                return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        int barTop = sbTopY();

        // --- topbar icon clicks ---
        if (my >= 0 && my <= S(ui::TOPBAR_H)) {
            int icon = hitTestTopbarIcon(mx, my, g_ui.winW);
            LOG_TRACE("MAIN", "topbar click mx=%d my=%d winW=%d icon=%d", mx, my, g_ui.winW, icon);
            switch (icon) {
            case 0: // close
                PostMessage(hwnd, WM_CLOSE, 0, 0);
                return 0;
            case 1: // maximize / fullscreen
                toggleFullscreen(hwnd);
                return 0;
            case 2: // minimize
                ShowWindow(hwnd, SW_MINIMIZE);
                return 0;
            case 3: // playlist（右侧独立区域：窗口扩展）
                g_ui.playlistOpen = !g_ui.playlistOpen;
                LOG_INFO("MAIN", "pl toggle -> %d (mx=%d my=%d winW=%d)",
                         g_ui.playlistOpen ? 1 : 0, mx, my, g_ui.winW);
                if (!g_ui.fullscreen) applyPlaylistWindow(hwnd);
                else g_dirty.store(true);
                return 0;
            case 4: { // PIP 置顶迷你小窗
                if (g_mpv && g_mpv->hasMedia()) {
                    if (g_ui.fullscreen) toggleFullscreen(hwnd);  // 全屏先退出
                    toggleMini(hwnd);
                }
                return 0;
            }
            case 5: { // camera/screenshot
                if (g_mpv && g_mpv->mpv()) {
                    const char* cmd[] = { "screenshot", NULL };
                    int r = mpv_command(g_mpv->mpv(), cmd);
                    LOG_INFO("MAIN", "screenshot ret=%d (%s)", r,
                             r < 0 ? mpv_error_string(r) : "ok");
                    showToast(r < 0 ? "Screenshot failed" : "Screenshot saved");
                }
                return 0;
            }
            default:
                break;
            }
            // no icon hit -> window drag
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        // --- seekbar ---
        if (g_mpv && my >= barTop - S(6) && my <= barTop + S(22) &&
            mx >= sbLeftX() && mx <= sbRightX() && g_mpv->duration() > 0) {
            g_ui.seekingDrag = true;
            double ratio = (double)(mx - sbLeftX()) / sbWidth();
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_ui.seekTarget = g_mpv->duration() * ratio;
            SetCapture(hwnd);
        }
        // --- controlbar row1 命中（与渲染共用 Row1Layout）---
        {
            bool volOpen = (g_ui.volumeSliderOpen || g_ui.volumeDragging);
            Row1Layout L;
            layoutRow1(g_ui.winW, g_ui.winH, volOpen, L);
            auto inRc = [&](const SDL_Rect& r) {
                return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
            };
            // 音量滑条(展开时)拖拽起点 —— 先于图标判定
            if (volOpen && mx >= L.volSliderX - S(6) &&
                mx <= L.volSliderX + S(76) &&
                my >= L.cy - S(12) && my <= L.cy + S(12)) {
                g_ui.volumeDragging = true;
                float ratio = (float)(mx - L.volSliderX) / S(70);
                if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
                g_mpv->setVolume(ratio);
                SetCapture(hwnd);
            }
            else if (inRc(L.prev)) {
                int idx = playlistIndexOf(g_mpv->path());
                if (idx > 0) { playIndex(idx - 1); showToast("Previous"); }
                else showToast("No previous track");
            }
            else if (inRc(L.play)) {
                g_mpv->togglePause();
            }
            else if (inRc(L.next)) {
                int idx = playlistIndexOf(g_mpv->path());
                int n = (int)g_playlist.size();
                if (idx >= 0 && idx + 1 < n) { playIndex(idx + 1); showToast("Next"); }
                else showToast("No next track");
            }
            else if (inRc(L.subBtn)) {
                bool vis = !g_mpv->subVisible();
                g_mpv->setSubVisibility(vis);
                std::string trk = g_mpv->currentSubTrack();
                char msg[96];
                std::snprintf(msg, sizeof(msg), vis ? "Subtitles ON %s" : "Subtitles OFF",
                              trk.empty() ? "" : ("[" + trk + "]").c_str());
                showToast(msg);
            }
            else if (inRc(L.speedBtn)) {
                g_ui.speedMenuOpen = !g_ui.speedMenuOpen;
            }
            else if (inRc(L.qualityBtn)) {
                showToast("至臻画质");
            }
            else if (mx >= L.volIconCx - S(17) && mx <= L.volIconCx + S(17) &&
                     my >= L.cy - S(17) && my <= L.cy + S(17)) {
                g_mpv->toggleMute();
                showToast(g_mpv->muted() ? "Muted" : "Unmuted");
                LOG_INFO("MAIN", "mute toggled -> %d", g_mpv->muted() ? 1 : 0);
            }
            else if (inRc(L.setBtn)) {
                g_ui.settingsOpen = !g_ui.settingsOpen;
            }
            else if (inRc(L.fullBtn)) {
                toggleFullscreen(hwnd);
            }
            else if (g_ui.speedMenuOpen) {
                // 点菜单外任意处 = 关闭菜单（不触发视频暂停）
            }
            else {
                goto videoAreaClick;
            }
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            return 0;
        }
    videoAreaClick:;
        // --- speed popup 选择（与渲染共用几何） ---
        if (g_ui.speedMenuOpen) {
            Row1Layout L;
            layoutRow1(g_ui.winW, g_ui.winH, false, L);
            int itemH = S(32);
            int menuW = S(132);
            int menuH = SPEED_PRESET_COUNT * itemH + S(12);
            int menuX = L.speedBtn.x;
            int menuY = L.speedBtn.y - menuH - S(6);  // 向上展开
            if (menuY < 0) menuY = L.speedBtn.y + L.speedBtn.h + S(6);  // 空间不足时回退向下
            if (menuX + menuW > g_ui.winW - S(8)) menuX = g_ui.winW - menuW - S(8);
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
        // --- settings modal interactions ---
        else if (g_ui.settingsOpen) {
            SettingsGeom sg = settingsGeom(g_ui.winW, g_ui.winH);
            bool inside = (mx >= sg.panelX && mx <= sg.panelX + sg.panelW &&
                           my >= sg.panelY && my <= sg.panelY + sg.panelH);
            if (!inside) {                       // 点外 = 关闭
                g_ui.settingsOpen = false;
            }
            else if (std::abs(mx - sg.closeCx) <= sg.closeR &&
                     std::abs(my - sg.closeCy) <= sg.closeR) {
                g_ui.settingsOpen = false;
                saveConfig(configPath(), g_cfg);
            }
            else {
                int* vals[SET_ROW_COUNT] = { &g_cfg.hwDecode, &g_cfg.volNorm,
                    &g_cfg.subAutoLoad, &g_cfg.thumbCache, &g_cfg.resume,
                    &g_cfg.nightMode, &g_cfg.audioExclusive, &g_cfg.motionInterp,
                    &g_cfg.hiQScale };
                const char* keys[SET_ROW_COUNT] = { "hw", "vol", "sub", "thumb",
                    "resume", "night", "excl", "interp", "hiq" };
                const char* names[SET_ROW_COUNT] = { "Hardware Decode", "Volume Norm",
                    "Sub Auto-Load", "Thumb Cache", "Resume",
                    "Night Mode", "Exclusive Audio", "Motion Interp", "HQ Scaling" };
                bool handled = false;
                for (int i = 0; i < SET_ROW_COUNT && !handled; ++i) {
                    if (my >= sg.rowY[i] - S(5) && my <= sg.rowY[i] + sg.swH + S(5) &&
                        mx >= sg.panelX + S(12)) {
                        *vals[i] = *vals[i] ? 0 : 1;
                        applySetting(keys[i], *vals[i]);
                        showToast(names[i]);
                        LOG_INFO("MAIN", "setting %s -> %d", keys[i], *vals[i]);
                        handled = true;
                    }
                }
                if (!handled && my >= sg.chipY && my <= sg.chipY + sg.chipH) {
                    for (int i = 0; i < 3; ++i) {
                        int lx = sg.swX - S(180) + i * (sg.chipW + S(6));
                        if (mx >= lx && mx <= lx + sg.chipW) {
                            g_cfg.playMode = i;
                            showToast(i == 0 ? "Mode: Single" :
                                      i == 1 ? "Mode: Loop" : "Mode: Shuffle");
                            LOG_INFO("MAIN", "playmode -> %d", i);
                            handled = true;
                            break;
                        }
                    }
                }
                saveConfig(configPath(), g_cfg);
            }
        }
        // --- 音量图标点击：切换静音（滑条由 hover 展开） ---
        else if (g_mpv && mx >= g_ui.winW - S(68) && mx <= g_ui.winW - S(40) &&
                 my >= barTop + S(36) && my <= barTop + S(64)) {
            g_mpv->toggleMute();
            showToast(g_mpv->muted() ? "Muted" : "Unmuted");
            LOG_INFO("MAIN", "mute toggled -> %d", g_mpv->muted() ? 1 : 0);
        }
        // --- 播放列表面板区域：关闭钮 -> 滚动条 -> 列表项候选 ---
        else if (g_ui.playlistOpen && mx >= g_ui.winW) {
            if (mx >= g_ui.plCloseRect.x && mx <= g_ui.plCloseRect.x + g_ui.plCloseRect.w &&
                my >= g_ui.plCloseRect.y && my <= g_ui.plCloseRect.y + g_ui.plCloseRect.h) {
                g_ui.playlistOpen = false;
                applyPlaylistWindow(hwnd);
                return 0;
            }
            if (g_ui.sbTrackX >= 0 &&
                mx >= g_ui.sbTrackX - S(5) && mx <= g_ui.sbTrackX + g_ui.sbTrackW + S(5) &&
                my >= g_ui.sbTrackY && my <= g_ui.sbTrackY + g_ui.sbTrackH) {
                SetCapture(hwnd);
                if (my < g_ui.sbBarY || my > g_ui.sbBarY + g_ui.sbBarH) {
                    // 轨道跳页: bar 中心对齐点击处
                    int contentH = (int)g_playlist.size() * S(72);
                    int viewH = g_ui.sbTrackH;
                    g_ui.playlistScroll =
                        (my - g_ui.sbGrabOff - g_ui.sbTrackY - g_ui.sbBarH / 2) *
                        (contentH - viewH) / (viewH - g_ui.sbBarH);
                    clampPlaylistScroll();
                }
                g_ui.sbDragging = true;
                g_ui.sbGrabOff = my - g_ui.sbBarY;
            } else {
                int panelY = S(ui::TOPBAR_H);
                int itemH = S(72);
                int rel = my - (panelY + S(45)) + g_ui.playlistScroll;
                g_ui.plDragFrom = -1; g_ui.plDragging = false;
                if (rel >= 0) {
                    int itemIdx = rel / itemH;
                    if (itemIdx < (int)g_playlist.size()) {
                        g_ui.plDragFrom = itemIdx;
                        g_ui.plDownY = my;
                        SetCapture(hwnd);   // 拖拽/松手都在面板外也能跟踪
                    }
                }
            }
        }
        // --- click on video area（延迟执行暂停，双击留给全屏） ---
        else {
            if (g_mpv) {
                g_ui.pendingPause = true;
                SetTimer(hwnd, TIMER_SINGLECLICK, 250, nullptr);
            }
        }

        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        int barTop = sbTopY();
        // 视频区双击 -> 全屏切换（取消待定暂停）
        if (g_mpv && my > S(ui::TOPBAR_H) && my < barTop - S(6)) {
            KillTimer(hwnd, TIMER_SINGLECLICK);
            g_ui.pendingPause = false;
            toggleFullscreen(hwnd);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_SINGLECLICK) {
            KillTimer(hwnd, TIMER_SINGLECLICK);
            if (g_ui.pendingPause && g_mpv) {
                g_mpv->togglePause();
            }
            g_ui.pendingPause = false;
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_ui.seekingDrag) {
            g_ui.seekingDrag = false;
            if (g_mpv) g_mpv->seek(g_ui.seekTarget);
        }
        if (g_ui.volumeDragging) {
            g_ui.volumeDragging = false;
        }
        if (g_ui.sbDragging) {
            g_ui.sbDragging = false;
        }
        // 列表拖拽落位 / 单击播放
        if (g_ui.plDragFrom >= 0) {
            if (g_ui.plDragging) {
                int itemH = S(72);
                int topY = S(ui::TOPBAR_H) + S(45);
                float rel = (float)(g_ui.plDragY - topY) + g_ui.playlistScroll;
                int drop = (int)(rel / itemH + 0.5f);
                int n = (int)g_playlist.size();
                if (drop < 0) drop = 0;
                if (drop > n) drop = n;
                int from = g_ui.plDragFrom;
                if (drop != from && drop != from + 1) {
                    std::string p = g_playlist[from];
                    g_playlist.erase(g_playlist.begin() + from);
                    int ins = drop; if (ins > from) --ins;
                    if (ins > (int)g_playlist.size()) ins = (int)g_playlist.size();
                    g_playlist.insert(g_playlist.begin() + ins, p);
                    LOG_INFO("MAIN", "playlist move %d -> %d", from, ins);
                    showToast("Playlist reordered");
                }
            } else {
                playIndex(g_ui.plDragFrom);   // 未拖动 = 单击播放
            }
            g_ui.plDragFrom = -1; g_ui.plDragging = false;
        }
        ReleaseCapture();
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT rc; GetClientRect(hwnd, &rc);
        ScreenToClient(hwnd, &pt);
        short d = GET_WHEEL_DELTA_WPARAM(wp);

        // 播放列表面板区域：滚动列表
        if (g_ui.playlistOpen && pt.x >= g_ui.winW) {
            int panelH = g_ui.winH - S(ui::TOPBAR_H);
            int contentH = (int)g_playlist.size() * S(72);
            int viewH = panelH - S(55);
            int step = S(72) * 2;
            g_ui.playlistScroll -= (d > 0 ? step : -step);
            if (g_ui.playlistScroll < 0) g_ui.playlistScroll = 0;
            if (contentH > viewH && g_ui.playlistScroll > contentH - viewH)
                g_ui.playlistScroll = contentH - viewH;
            else if (contentH <= viewH) g_ui.playlistScroll = 0;
        }
        else if (g_mpv) {
            g_mpv->setVolume(g_mpv->volume() + (d > 0 ? 0.05f : -0.05f));
            char msg[24];
            std::snprintf(msg, sizeof(msg), "Volume %d%%", (int)(g_mpv->volume() * 100 + 0.5f));
            showToast(msg);
        }
        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + 2000;
        return 0;
    }

    // ---- drag-drop ----
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wp;
        wchar_t wpath[MAX_PATH * 2];
        if (DragQueryFileW(hDrop, 0, wpath, (UINT)(MAX_PATH * 2)) > 0) {
            // ANSI 版会返回 GBK 字节, mpv 按 UTF-8 解析 -> 中文路径截断乱码
            int u8len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                            nullptr, 0, nullptr, nullptr);
            std::string path(u8len > 0 ? u8len - 1 : 0, '\0');
            if (u8len > 1)
                WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                    path.data(), u8len, nullptr, nullptr);
            LOG_INFO("MAIN", "drop file (%zu bytes): %s", path.size(), path.c_str());
            buildPlaylistAround(path);
            playPath(path);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_CLOSE:
        saveWindowPos(hwnd);          // 销毁前抓取位置
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- SDL2 overlay ----
// 透明键 = 纯黑(0,0,0)。UI 为亮字暗底风格且不含纯黑;
// 半透明色叠加在黑底上 = 自然变暗, 无品红(colorkey 方案旧缺陷)染色
static const Uint8 TRANSPARENT_R = 0;
static const Uint8 TRANSPARENT_G = 0;
static const Uint8 TRANSPARENT_B = 0;

static bool createOverlay(HWND parent, int w, int h) {
    // 顶层无边框窗口（本系统不支持 WS_EX_LAYERED 子窗口，实测 err=87）
    // 通过 OWNER 关联 + TOOLWINDOW 融入主窗口：不进任务栏/Alt+Tab，随主窗口关闭
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
    HWND ov = info.info.win.window;
    g_overlayHwnd = ov;   // 供 mini 模式 z 序调整使用

    LONG_PTR ex = GetWindowLongPtrW(ov, GWL_EXSTYLE);
    SetWindowLongPtrW(ov, GWL_EXSTYLE,
        ex | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW);
    SetLayeredWindowAttributes(ov,
        RGB(TRANSPARENT_R, TRANSPARENT_G, TRANSPARENT_B), 0, LWA_COLORKEY);

    // 设为 parent 的 Owned 窗口：置顶于父、父最小化时联动、无独立任务栏项
    SetWindowLongPtrW(ov, GWLP_HWNDPARENT, (LONG_PTR)parent);
    SetWindowPos(ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    g_sdlRdr = SDL_CreateRenderer(g_sdlWin, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_sdlRdr) {
        LOG_WARN("MAIN", "accelerated renderer failed (%s), trying software", SDL_GetError());
        g_sdlRdr = SDL_CreateRenderer(g_sdlWin, -1, 0);
    }
    if (!g_sdlRdr) {
        LOG_ERROR("MAIN", "SDL_CreateRenderer: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);

    g_text.init(g_sdlRdr);

    POINT pt = {0,0}; ClientToScreen(parent, &pt);
    SetWindowPos(ov, nullptr, pt.x, pt.y, w, h, SWP_NOACTIVATE);

    LOG_INFO("MAIN", "overlay created (%dx%d, owned)", w, h);
    return true;
}

// 实心圆(扫描线近似; 用于暂停中央按钮底)
static void fillCircle(SDL_Renderer* r, int cx, int cy, int rad,
                       Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (rad <= 0) return;
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    for (int dy = -rad; dy <= rad; ++dy) {
        int dx = (int)std::sqrt((float)rad * rad - (float)dy * dy);
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void destroyGradCache();   // 定义于 drawGradientBar（渐变纹理缓存）

static void destroyOverlay() {
    g_text.shutdown();
    svgicon::shutdown();
    destroyGradCache();
    if (g_sdlRdr) { SDL_DestroyRenderer(g_sdlRdr); g_sdlRdr = nullptr; }
    if (g_sdlWin) { SDL_DestroyWindow(g_sdlWin);   g_sdlWin = nullptr; }  // 连同 HWND 一起销毁
    g_overlayHwnd = nullptr;
}

// ---- dithered gradient helper ----
// 渐变条逐像素绘制代价 ~13 万次 FillRect/帧; 缓存为纹理后每帧一次 RenderCopy。
// 透明区(alpha<阈值)写入 0 —— 与品红 colorkey 兼容, 视频照常穿透。
struct GradKey {
    int w = 0, h = 0;
    Uint8 cr = 0, cg = 0, cb = 0, aTop = 0, aBot = 0;
    bool operator==(const GradKey& o) const {
        return w == o.w && h == o.h && cr == o.cr && cg == o.cg &&
               cb == o.cb && aTop == o.aTop && aBot == o.aBot;
    }
};
static SDL_Texture* g_gradTex[2]   = { nullptr, nullptr };
static GradKey       g_gradKey[2]  = {};

static void destroyGradCache() {
    for (auto& t : g_gradTex)
        if (t) { SDL_DestroyTexture(t); t = nullptr; }
}

static void drawGradientBar(SDL_Renderer* r, int slot, int x, int y, int w, int h,
                             Uint8 cr, Uint8 cg, Uint8 cb, Uint8 aTop, Uint8 aBot) {
    static const int bayer[4][4] = {
        {  0, 136,  34, 170},
        {204,  68, 238, 102},
        { 51, 187,  17, 153},
        {255, 119, 221,  85}
    };
    if (w <= 0 || h <= 0) return;
    GradKey key{ w, h, cr, cg, cb, aTop, aBot };

    if (!g_gradTex[slot] || !(g_gradKey[slot] == key)) {
        if (g_gradTex[slot]) { SDL_DestroyTexture(g_gradTex[slot]); g_gradTex[slot] = nullptr; }
        SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!tex) return;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        Uint32* pixels = nullptr; int pitch = 0;
        if (SDL_LockTexture(tex, nullptr, (void**)&pixels, &pitch) == 0) {
            Uint32 rgb = ((Uint32)cr << 16) | ((Uint32)cg << 8) | cb;
            for (int dy = 0; dy < h; ++dy) {
                int a = aTop + ((int)(aBot - aTop)) * dy / h;
                int by = dy % 4;
                Uint32* row = (Uint32*)((Uint8*)pixels + dy * pitch);
                for (int dx = 0; dx < w; ++dx) {
                    row[dx] = (a > bayer[by][dx % 4])
                            ? (0xFF000000u | rgb)
                            : 0x00000000u;   // 透明键兼容
                }
            }
            SDL_UnlockTexture(tex);
        }
        g_gradTex[slot] = tex;
        g_gradKey[slot] = key;
    }
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(r, g_gradTex[slot], nullptr, &dst);
}

// ---- rendering ----
static void renderOverlay() {
    if (!g_sdlRdr) return;

    uploadThumbs(g_sdlRdr);   // 惰性上传就绪的缩略图纹理

    SDL_SetRenderDrawColor(g_sdlRdr, TRANSPARENT_R, TRANSPARENT_G, TRANSPARENT_B, 255);
    SDL_RenderClear(g_sdlRdr);

    int w = g_ui.winW, h = g_ui.winH, totalW = g_ui.totalW;

    if (!g_mpv || !g_mpv->hasMedia()) {
        // --- welcome page ---
        int w = g_ui.winW, h = g_ui.winH, totalW = g_ui.totalW;

        // topbar still visible (glass: 半透明)
        drawGradientBar(g_sdlRdr, 0, 0, 0, w, S(ui::TOPBAR_H), 11, 11, 11, 150, 0);
        // title
        g_text.drawText(S(20), S(14), "VPlayer", 14, 255, 255, 255);
        // topbar icons
        int iconY = S(ui::TOPBAR_H) / 2;
        int rx = w - S(20);
        svgicon::draw(g_sdlRdr, "close",    rx, iconY, S(20), 255, 255, 255, 200); rx -= S(34);
        svgicon::draw(g_sdlRdr, "maximize", rx, iconY, S(20), 161, 161, 166, 200); rx -= S(34);
        svgicon::draw(g_sdlRdr, "minimize", rx, iconY, S(20), 161, 161, 166, 200);

        // logo
        svgicon::draw(g_sdlRdr, "play", w / 2, h / 2 - S(80), S(64), 37, 99, 235, 255);
        g_text.drawText(w / 2 - S(40), h / 2 - S(30), "VPlayer", 28, 255, 255, 255);

        // drop zone (dashed border)
        int dzW = S(400), dzH = S(120);
        int dzX = (w - dzW) / 2, dzY = h / 2 + S(10);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 30);
        // draw dashed border (approximate with segments)
        int dashLen = S(8), gapLen = S(5);
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
        g_text.drawText(w / 2 - S(60), dzY + S(30), "Drop video here", 13, 161, 161, 166);
        g_text.drawText(w / 2 - S(55), dzY + S(55), "or press Ctrl+O", 12, 100, 100, 100);

        // 播放队列网格（当前文件夹扫描结果）
        if (!g_playlist.empty()) {
            g_text.drawText(w / 2 - S(60), dzY + dzH + S(30), "Playlist", 14, 161, 161, 166);
            int cardW = S(140), cardH = S(80), gap = S(12);
            int cols = std::min(4, (int)g_playlist.size());
            int gridW = cols * cardW + (cols - 1) * gap;
            int gridX = (w - gridW) / 2;
            int gridY = dzY + dzH + S(55);
            int idx = 0;
            for (size_t pi = 0; pi < g_playlist.size() && idx < 8; ++pi, ++idx) {
                int col = idx % cols, row = idx / cols;
                int cx = gridX + col * (cardW + gap);
                int cy = gridY + row * (cardH + gap);
                bool isCur = (g_mpv && g_mpv->path() == g_playlist[pi]);
                SDL_Rect cardRc = {cx, cy, cardW, cardH};
                SDL_SetRenderDrawColor(g_sdlRdr, 21, 21, 21, 255);
                SDL_RenderFillRect(g_sdlRdr, &cardRc);
                SDL_SetRenderDrawColor(g_sdlRdr,
                    isCur ? 37 : 255, isCur ? 99 : 255, isCur ? 235 : 255, isCur ? 200 : 15);
                SDL_RenderDrawRect(g_sdlRdr, &cardRc);
                std::string fn = fileNameOf(g_playlist[pi]);
                if (fn.size() > 18) fn = fn.substr(0, 15) + "...";
                g_text.drawText(cx + S(8), cy + S(10), fn, 11, isCur ? 255 : 200, isCur ? 255 : 200, isCur ? 255 : 200);
                double hpos = 0;
                auto hit = g_cfg.history.find(g_playlist[pi]);
                if (hit != g_cfg.history.end()) hpos = hit->second;
                char timeBuf[16] = "--:--";
                if (hpos > 0) {
                    std::snprintf(timeBuf, sizeof(timeBuf), "@%02d:%02d",
                        (int)(hpos / 60), (int)hpos % 60);
                }
                g_text.drawText(cx + S(8), cy + S(35), timeBuf, 10, 100, 100, 100);
            }
        }

        // keyboard hints
        g_text.drawText(S(20), h - S(30), "Space=Play/Pause  Left/Right=Seek  F=Fullscreen  M=Mute  [/]=Speed", 10, 80, 80, 80);

        SDL_RenderPresent(g_sdlRdr);
        return;
    }

    double dur = g_mpv->duration();
    // 速度切换后短暂冻结进度条, 防止 time-pos 跳变导致抖动
    static double s_lastPos = 0.0;
    static Uint32 s_freezeStart = 0;
    double pos;
    if (g_ui.seekingDrag) {
        pos = g_ui.seekTarget;
    } else if (g_mpv->seekbarFrozen()) {
        // 冻结期间: 用冻结前的 pos + 经过的墙钟时间 * 新速度 推进
        if (s_freezeStart == 0) { s_lastPos = g_mpv->clock(); s_freezeStart = SDL_GetTicks(); }
        double elapsed = (SDL_GetTicks() - s_freezeStart) / 1000.0;
        pos = s_lastPos + elapsed * g_mpv->speed();
        double d = g_mpv->duration();
        if (d > 0 && pos > d) pos = d;
    } else {
        pos = g_mpv->clock();
        s_freezeStart = 0;  // 解冻: 重置
    }

    // 控件淡出动画: alpha=0 时控制栏滑出屏、顶栏滑出屏顶
    float fa = g_ui.ctrlAlpha;
    Uint8 fade = (Uint8)(fa * 255.0f);
    int topOff = -(int)((1.0f - fa) * S(ui::TOPBAR_H) + 0.5f);

    // --- topbar (gradient: glass 半透明效果, 视频隐约可见) ---
    {
        drawGradientBar(g_sdlRdr, 0, 0, topOff, w, S(ui::TOPBAR_H), 11, 11, 11,
                        (Uint8)(150 * fa), 0);

        // title (left)
        std::string title = g_mpv->title();
        if (title.empty()) title = "VPlayer";
        if (title.size() > 55) title = title.substr(0, 52) + "...";
        g_text.drawText(S(20), S(14) + topOff, title, 14, 255, 255, 255);

        // icons (right) - same order as design mockup
        int iconY = S(ui::TOPBAR_H) / 2 + topOff;
        auto A = [&](Uint8 base) { return (Uint8)(base * fa); };
        int rx = w - S(20);
        svgicon::draw(g_sdlRdr, "close",    rx, iconY, S(20), 255, 255, 255, A(200)); rx -= S(34);
        svgicon::draw(g_sdlRdr, "maximize", rx, iconY, S(20), 161, 161, 166, A(200)); rx -= S(34);
        svgicon::draw(g_sdlRdr, "minimize", rx, iconY, S(20), 161, 161, 166, A(200)); rx -= S(34);
        svgicon::draw(g_sdlRdr, "list",     rx, iconY, S(20), 161, 161, 166, A(200)); rx -= S(34);
        svgicon::draw(g_sdlRdr, "pip",      rx, iconY, S(20),
            g_ui.miniMode ? 37 : 161, g_ui.miniMode ? 99 : 161,
            g_ui.miniMode ? 235 : 166, A(200)); rx -= S(34);
        svgicon::draw(g_sdlRdr, "camera",   rx, iconY, S(20), 161, 161, 166, A(200));
    }

    // 控件淡出: 控制栏随 alpha 滑出屏底
    int barTop = sbTopY() + (int)((1.0f - fa) * S(CONTROL_BAR_H) + 0.5f);

    // --- 暂停压暗遮罩 + 中央圆形播放钮 (M32g.5 效果, 用户确认保留) ---
    if (fa > 0.01f && g_mpv->state() == MpvBackend::State::Paused) {
        int top = S(ui::TOPBAR_H);
        SDL_Rect dim = {0, top, w, (barTop > top ? barTop - top : 0)};
        // 半透明压暗(非纯黑条), 让视频仍隐约可见
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, (Uint8)(100 * fa));
        SDL_RenderFillRect(g_sdlRdr, &dim);
        int ccx = w / 2, ccy = top + (barTop - top) / 2;
        // 效果图 .center-play: 白描边圆(white.75) + 黑底(blur 近似) + play 右偏
        fillCircle(g_sdlRdr, ccx, ccy, S(37), 191, 191, 196, (Uint8)(fa * 255));
        fillCircle(g_sdlRdr, ccx, ccy, S(35), 22, 22, 25, (Uint8)(fa * 255));
        svgicon::draw(g_sdlRdr, "play", ccx + S(3), ccy, S(30),
                      255, 255, 255, (Uint8)(235 * fa));
    }

    // --- gradient background (glass: 半透明) ---
    drawGradientBar(g_sdlRdr, 1, 0, barTop, w, S(60), 11, 11, 11, 0, 150);
    // solid bottom portion (glass: 降低不透明度)
    SDL_Rect solidRc = {0, barTop + S(60), w, S(CONTROL_BAR_H) - S(60)};
    SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_sdlRdr, 11, 11, 11, 150);
    SDL_RenderFillRect(g_sdlRdr, &solidRc);

    // --- seekbar (at very top of bar) ---
    if (dur > 0) {
        int tx = sbLeftX(), tw = sbWidth();
        int ty = barTop + S(4);
        int th = g_ui.seekbarHover ? S(ui::SEEKBAR_TRACK_H_HOVER) : S(ui::SEEKBAR_TRACK_H);

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

        // thumb (on hover or drag) + 时间预览 (M29)
        if (g_ui.seekbarHover || g_ui.seekingDrag) {
            int cx = tx + progW;
            int cy = ty + th / 2;
            int r = S(ui::SEEKTHUMB_D) / 2;
            SDL_Rect tRc = {cx - r, cy - r, S(ui::SEEKTHUMB_D), S(ui::SEEKTHUMB_D)};
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
            SDL_RenderFillRect(g_sdlRdr, &tRc);

            // 预览时间戳气泡
            double hoverPos = dur * ((double)(g_ui.mouseX - tx) / tw);
            char pv[16];
            formatTime(pv, sizeof(pv), hoverPos);
            int bw = S(56), bh = S(24);
            int bx = g_ui.mouseX - bw / 2;
            if (bx < tx) bx = tx;
            if (bx + bw > tx + tw) bx = tx + tw - bw;
            int by = ty - bh - S(8);
            SDL_Rect bubble = {bx, by, bw, bh};
            SDL_SetRenderDrawColor(g_sdlRdr, 20, 20, 22, 235);
            SDL_RenderFillRect(g_sdlRdr, &bubble);
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 40);
            SDL_RenderDrawRect(g_sdlRdr, &bubble);
            g_text.drawText(bx + S(12), by + S(4), pv, 11, 255, 255, 255);
        }
    }

    // --- controlbar row1 (效果图复刻): prev/PLAY白底/next/time ... 字幕/倍速/画质/音量/设置/全屏 ---
    {
        Row1Layout L;
        bool volOpen = (g_ui.volumeSliderOpen || g_ui.volumeDragging);
        layoutRow1(w, h, volOpen, L);
        auto A = [&](Uint8 base) { return (Uint8)(base * fa); };
        const int iconC = 228, text2 = 161;

        // prev
        svgicon::draw(g_sdlRdr, "prev", L.prev.x + S(17), L.prev.y + S(17), S(18),
                      iconC, iconC, 231, A(255));
        // PLAY 白底圆角方(r8) + 黑图标 —— 效果图 .ctrlbtn.play
        {
            SDL_SetRenderDrawColor(g_sdlRdr, A(235), A(235), A(235), 255);
            SDL_RenderFillRect(g_sdlRdr, &L.play);
            SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
            const char* pi = (g_mpv->state() == MpvBackend::State::Paused) ? "play" : "pause";
            svgicon::draw(g_sdlRdr, pi, L.play.x + L.play.w / 2, L.play.y + L.play.h / 2,
                          S(20), 11, 11, 11, 255);
        }
        // next
        svgicon::draw(g_sdlRdr, "next", L.next.x + S(17), L.next.y + S(17), S(18),
                      iconC, iconC, 231, A(255));
        // time（tabular 观感: 等宽由字体保证）
        {
            char cur[32], tot[32], ts[80];
            formatTime(cur, sizeof(cur), pos);
            formatTime(tot, sizeof(tot), dur);
            std::snprintf(ts, sizeof(ts), "%s / %s", cur, tot);
            g_text.drawText(L.timeX, L.cy - S(9), ts, 12, 161, 161, 166);
        }

        // 右侧 textbtn 组
        auto drawTextBtnIcon = [&](const SDL_Rect& rc, const char* label,
                                   const char* iconId, Uint8 ir, Uint8 ig, Uint8 ib) {
            int tw = g_text.measureText(label, 12);
            int tx = rc.x + S(9);
            g_text.drawText(tx, rc.y + S(10), label, 12, 200, 200, 205);
            svgicon::draw(g_sdlRdr, iconId, tx + tw + S(11), rc.y + S(17), S(15),
                          ir, ig, ib, A(255));
        };
        // 字幕
        {
            Uint8 ic = g_mpv->subVisible() ? 235 : 110;
            drawTextBtnIcon(L.subBtn, "字幕", "cc", ic, ic, ic);
        }
        // 倍速: "倍速" text2 + 值 accent2 蓝粗
        {
            char spd[16];
            float s = g_mpv->speed();
            if (s == (int)s) std::snprintf(spd, sizeof(spd), "%.0fx", s);
            else             std::snprintf(spd, sizeof(spd), "%.2fx", s);
            int lw = g_text.measureText("倍速", 12);
            g_text.drawText(L.speedBtn.x + S(9), L.speedBtn.y + S(10), "倍速", 12, 200, 200, 205);
            g_text.drawText(L.speedBtn.x + S(9) + lw + S(5), L.speedBtn.y + S(10), spd, 12,
                            59, 130, 246);
        }
        // 至臻画质
        g_text.drawText(L.qualityBtn.x + S(9), L.qualityBtn.y + S(10), "至臻画质", 12, 200, 200, 205);
        // 音量图标
        {
            const char* vid = g_mpv->muted() ? "mute" : "volume";
            svgicon::draw(g_sdlRdr, vid, L.volIconCx, L.cy, S(18),
                          iconC, iconC, 231, A(255));
        }
        // 设置(文字+gear)
        drawTextBtnIcon(L.setBtn, "设置", "gear", 200, 200, 205);
        // 全屏
        const char* fid = g_ui.fullscreen ? "exitfull" : "full";
        svgicon::draw(g_sdlRdr, fid, L.fullBtn.x + S(17), L.fullBtn.y + S(17), S(18),
                      iconC, iconC, 231, A(255));

        // 音量滑条(展开态, 在 Row1Layout 内用 L.volIconCx 定位)
        if (volOpen && L.volSliderW > 0) {
            int sldW = S(70);
            int sx = L.volSliderX;
            int sy = L.cy - S(2);
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 51);
            SDL_Rect trk = {sx, sy, sldW, S(4)};
            SDL_RenderFillRect(g_sdlRdr, &trk);
            float v = g_mpv->volume();
            int fw = (int)(sldW * v);
            if (fw > 0) {
                SDL_Rect fl = {sx, sy, fw, S(4)};
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
                SDL_RenderFillRect(g_sdlRdr, &fl);
            }
        }

        // HW 徽标(顶栏右侧下方小字, 效果图无此项但保留信息)
        if (g_mpv->hwDecodeActive()) {
            g_text.drawText(w - S(60), barTop - S(2), "[HW]", 10, 59, 130, 246);
        }
    }

    // --- buffering indicator ---
    if (g_mpv->bufferFill() < 0.5) {
        g_text.drawText(w / 2 - S(30), barTop + S(75), "Buffering...", 12, 161, 161, 166);
    }

    // --- speed popup menu（效果图规格: 圆角r8/向上展开/k标注） ---
    if (g_ui.speedMenuOpen) {
        Row1Layout L;
        layoutRow1(w, h, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = S(32);
        int menuW = S(132);
        int menuH = SPEED_PRESET_COUNT * itemH + S(12);
        int menuX = L.speedBtn.x;                        // 与按钮左对齐
        int menuY = L.speedBtn.y - menuH - S(6);        // 向上展开
        if (menuY < 0) menuY = L.speedBtn.y + L.speedBtn.h + S(6);  // 空间不足时回退向下
        if (menuX + menuW > w - S(8)) menuX = w - menuW - S(8);

        // 圆角矩形: 先画矩形主体, 再用圆填充四角
        int cr = S(8);  // corner radius
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        // 中间无圆角部分(上下条)
        SDL_Rect midH = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH);
        // 四角圆
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        // 边框(简化: 只画直线段)
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);

        float curSpeed = g_mpv->speed();
        for (int i = 0; i < SPEED_PRESET_COUNT; ++i) {
            int iy = menuY + S(6) + i * itemH;
            bool sel = (std::abs(curSpeed - SPEED_PRESETS[i]) < 0.01f);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            char label[16];
            float sp = SPEED_PRESETS[i];
            if (sp == (int)sp) std::snprintf(label, sizeof(label), "%.2fx", sp);
            else               std::snprintf(label, sizeof(label), "%.2fx", sp);
            g_text.drawText(menuX + S(10), iy + S(6), label, 13, tr, tg, tb);
            // k 标注: 慢/正常/快
            const char* k = (sp < 0.99f) ? "慢" : (sp < 1.01f) ? "正常" :
                            (sp < 2.01f) ? nullptr : "快";
            if (k) {
                int kw = g_text.measureText(k, 11);
                g_text.drawText(menuX + menuW - kw - S(10), iy + S(7), k, 11, 161, 161, 166);
            }
        }
    }

    // --- playlist panel (右侧独立区域) ---
    if (g_ui.playlistOpen) {
        int panelW, panelX;
        if (!g_ui.fullscreen) {
            panelW = totalW - w;                 // 窗口扩展出的独立区域
            panelX = w;
        } else {                                  // 全屏无法扩窗: 覆盖式
            panelW = S(430);
            panelX = w - panelW;
        }
        if (panelW < S(200)) { panelW = S(200); panelX = w - panelW; }   // 兜底
        int panelH = h - S(ui::TOPBAR_H);
        int panelY = S(ui::TOPBAR_H);

        // panel background（独立区域不透明）
        SDL_Rect pRc = {panelX, panelY, panelW, panelH};
        SDL_SetRenderDrawColor(g_sdlRdr, 16, 16, 17, 255);
        SDL_RenderFillRect(g_sdlRdr, &pRc);
        // left border
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
        SDL_RenderDrawLine(g_sdlRdr, panelX, panelY, panelX, panelY + panelH);

        // title + 关闭钮（效果图 .pl-head）
        g_text.drawText(panelX + S(14), panelY + S(16), "播放列表", 13, 255, 255, 255);
        int closeX = panelX + panelW - S(40);
        int closeY = panelY + S(10);
        SDL_Rect closeRc = {closeX, closeY, S(28), S(28)};
        svgicon::draw(g_sdlRdr, "close", closeX + S(14), closeY + S(14), S(14),
                      212, 212, 216, 255);
        g_ui.plCloseRect = closeRc;

        // items from playlist queue（卡片化: thumb100×56+dur角标+title+state）
        int itemY = panelY + S(45);
        int itemH = S(72);                       // 卡片高度(56 thumb+padding)
        int scroll = g_ui.playlistScroll;
        std::vector<std::string> visiblePaths;
        for (size_t pi = 0; pi < g_playlist.size(); ++pi) {
            int iy = itemY + (int)pi * itemH - scroll;
            if (iy + itemH < itemY - S(60)) continue;
            if (iy >= panelY + panelH - S(10)) break;
            const std::string& p = g_playlist[pi];
            visiblePaths.push_back(p);
            bool isCurrent = (g_mpv && g_mpv->path() == p);

            double hpos = 0;
            auto hit = g_cfg.history.find(p);
            if (hit != g_cfg.history.end()) hpos = hit->second;

            // hover 背景（鼠标在本项内）
            bool hovered = (g_ui.mouseX >= panelX + S(8) &&
                            g_ui.mouseX <= panelX + panelW - S(8) &&
                            g_ui.mouseY >= iy && g_ui.mouseY <= iy + itemH - S(6));
            if (isCurrent || hovered) {
                SDL_Rect hlRc = {panelX + S(7), iy, panelW - S(15), itemH - S(4)};
                if (isCurrent) SDL_SetRenderDrawColor(g_sdlRdr, 37, 99, 235, 46);
                else           SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 15);
                SDL_RenderFillRect(g_sdlRdr, &hlRc);
            }

            // 缩略图 100×56 r7（渐变占位底 #26262c→#15151a 近似）
            SDL_Rect thRc = {panelX + S(12), iy + S(8), S(100), S(56)};
            SDL_SetRenderDrawColor(g_sdlRdr, 33, 33, 38, 255);
            SDL_RenderFillRect(g_sdlRdr, &thRc);
            auto texIt = g_thumbTex.find(p);
            if (texIt != g_thumbTex.end()) {
                SDL_RenderCopy(g_sdlRdr, texIt->second, nullptr, &thRc);
            } else {
                svgicon::draw(g_sdlRdr, "play", thRc.x + S(50), thRc.y + S(28), S(20),
                              255, 255, 255, 64);
            }
            // dur 角标(right4 bottom4 黑.72)
            {
                char durBuf[16] = "";
                if (hpos > 1.0) {
                    std::snprintf(durBuf, sizeof(durBuf), "%02d:%02d",
                                  (int)(hpos / 60), (int)hpos % 60);
                    int dw = g_text.measureText(durBuf, 9) + S(8);
                    int dx = thRc.x + thRc.w - dw - S(4);
                    int dy = thRc.y + thRc.h - S(18);
                    SDL_Rect db = {dx, dy, dw, S(15)};
                    SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 184);
                    SDL_RenderFillRect(g_sdlRdr, &db);
                    g_text.drawText(dx + S(4), dy + S(2), durBuf, 9, 255, 255, 255);
                }
            }

            // meta: title 一行 + state 行
            std::string fn = fileNameOf(p);
            int maxTw = panelW - S(140);
            if (maxTw < S(80)) maxTw = S(80);
            {
                // 按像素宽截断
                if (g_text.measureText(fn, 12) > maxTw) {
                    while (fn.size() > 4 && g_text.measureText(fn + "...", 12) > maxTw)
                        fn.pop_back();
                    fn += "...";
                }
                Uint8 tr = isCurrent ? 191 : 240, tg = isCurrent ? 214 : 240,
                      tb = isCurrent ? 255 : 240;   // playing #bfd6ff
                g_text.drawText(thRc.x + thRc.w + S(10), iy + S(10), fn, 12, tr, tg, tb);
            }
            // state: 正在播放(accent2)/已播放(#6b7280)/未播放(#3f3f46)
            {
                const char* st; Uint8 sr, sg_, sb_;
                if (isCurrent) { st = "正在播放"; sr = 59; sg_ = 130; sb_ = 246; }
                else if (hpos > 1.0) { st = "已播放"; sr = 107; sg_ = 114; sb_ = 128; }
                else { st = "未播放"; sr = 63; sg_ = 63; sb_ = 70; }
                g_text.drawText(thRc.x + thRc.w + S(10), iy + S(32), st, 11, sr, sg_, sb_);
            }
        }

        // 拖拽排序视觉反馈：插入指示线 + 被拖项高亮
        if (g_ui.plDragging && g_ui.plDragFrom >= 0) {
            int itemH = S(72);
            int topY = panelY + S(45);
            float rel = (float)(g_ui.plDragY - topY) + g_ui.playlistScroll;
            int drop = (int)(rel / itemH + 0.5f);
            int n = (int)g_playlist.size();
            if (drop < 0) drop = 0;
            if (drop > n) drop = n;
            int lineY = topY + drop * itemH - g_ui.playlistScroll - itemH / 2 + itemH / 2;
            lineY = topY + drop * itemH - g_ui.playlistScroll;
            if (lineY >= panelY && lineY <= panelY + panelH) {
                SDL_Rect line = {panelX + S(4), lineY - S(2), panelW - S(10), S(3)};
                SDL_SetRenderDrawColor(g_sdlRdr, 37, 99, 235, 255);
                SDL_RenderFillRect(g_sdlRdr, &line);
            }
            int fromY = topY + g_ui.plDragFrom * itemH - g_ui.playlistScroll;
            if (fromY >= panelY && fromY <= panelY + panelH) {
                SDL_Rect hl = {panelX + S(4), fromY - S(2), panelW - S(8), itemH};
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 30);
                SDL_RenderFillRect(g_sdlRdr, &hl);
            }
        }

        // 提交可见集给缩略图 worker（仅缺图的）
        {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            std::vector<std::string> missing;
            for (auto& p : visiblePaths)
                if (!g_thumbRgb.count(p) && !g_thumbTex.count(p))
                    missing.push_back(p);
            g_thumbWant.swap(missing);
        }

        // scrollbar（M33d: 悬停加亮/拖拽/轨道跳页）
        {
            int contentH = (int)g_playlist.size() * itemH;
            int viewH = panelH - S(55);
            if (contentH > viewH && contentH > 0) {
                int trackW = S(6);
                int trackX = panelX + panelW - trackW - S(4);
                int trackY = panelY + S(45);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 18);
                SDL_Rect trk = {trackX, trackY, trackW, viewH};
                SDL_RenderFillRect(g_sdlRdr, &trk);
                int barH = std::max(S(30), viewH * viewH / contentH);
                int barY = trackY + g_ui.playlistScroll * (viewH - barH) / (contentH - viewH);
                // hover/拖拽时加亮
                Uint8 ba = (g_ui.sbHover || g_ui.sbDragging) ? 160 : 70;
                SDL_SetRenderDrawColor(g_sdlRdr, 235, 235, 240, ba);
                SDL_Rect br = {trackX, barY, trackW, barH};
                SDL_RenderFillRect(g_sdlRdr, &br);

                // 暴露几何给命中测试
                g_ui.sbTrackX = trackX; g_ui.sbTrackY = trackY;
                g_ui.sbTrackW = trackW; g_ui.sbTrackH = viewH;
                g_ui.sbBarY = barY;     g_ui.sbBarH = barH;
            } else {
                g_ui.sbTrackX = -1;
            }
        }

        if (g_playlist.empty()) {
            g_text.drawText(panelX + S(16), itemY + S(10), "No files in playlist", 12, 100, 100, 100);
        }
    }

    // --- settings modal panel ---
    if (g_ui.settingsOpen) {
        SettingsGeom sg = settingsGeom(w, h);

        // backdrop（黑 key 下不能用纯黑+alpha, 用不透明深灰压暗观感）
        SDL_SetRenderDrawColor(g_sdlRdr, 14, 14, 16, 255);
        SDL_Rect fullRc = {0, 0, w, h};
        SDL_RenderFillRect(g_sdlRdr, &fullRc);

        // panel
        SDL_Rect panelRc = {sg.panelX, sg.panelY, sg.panelW, sg.panelH};
        SDL_SetRenderDrawColor(g_sdlRdr, 21, 21, 21, 250);
        SDL_RenderFillRect(g_sdlRdr, &panelRc);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
        SDL_RenderDrawRect(g_sdlRdr, &panelRc);

        // title + close
        g_text.drawText(sg.panelX + S(20), sg.panelY + S(16), "Settings", 16, 255, 255, 255);
        svgicon::draw(g_sdlRdr, "close", sg.closeCx, sg.closeCy, S(18), 161, 161, 166, 200);

        // toggle rows
        int toggleVals[SET_ROW_COUNT] = { g_cfg.hwDecode, g_cfg.volNorm,
            g_cfg.subAutoLoad, g_cfg.thumbCache, g_cfg.resume,
            g_cfg.nightMode, g_cfg.audioExclusive, g_cfg.motionInterp,
            g_cfg.hiQScale };
        const char* rowLabels[SET_ROW_COUNT] = {
            "Hardware Decode",
            "Volume Normalization",
            "Subtitle Auto-Load",
            "Thumbnail Disk Cache",
            "Resume Playback",
            "Night Mode (Compressor)",
            "Exclusive Audio (WASAPI)",
            "Motion Interpolation",
            "High Quality Scaling (GPU+)",
        };
        for (int i = 0; i < SET_ROW_COUNT; ++i) {
            int ry = sg.rowY[i];
            bool on = (toggleVals[i] != 0);
            g_text.drawText(sg.panelX + S(20), ry + S(3), rowLabels[i], 13, on ? 230 : 170, on ? 230 : 170, on ? 230 : 170);

            SDL_Rect swRc = {sg.swX, ry, sg.swW, sg.swH};
            SDL_SetRenderDrawColor(g_sdlRdr, on ? 37 : 80, on ? 99 : 80, on ? 235 : 80, 255);
            SDL_RenderFillRect(g_sdlRdr, &swRc);
            int thumbX = on ? sg.swX + sg.swW - sg.swH : sg.swX;
            SDL_Rect tRc = {thumbX + S(2), ry + S(2), sg.swH - S(4), sg.swH - S(4)};
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
            SDL_RenderFillRect(g_sdlRdr, &tRc);
        }

        // playback mode row (Single / Loop / Shuffle)
        g_text.drawText(sg.panelX + S(20), sg.modeRowY + S(3), "Playback Mode", 13, 200, 200, 200);
        const char* modes[] = {"Single", "Loop", "Shuffle"};
        for (int i = 0; i < 3; ++i) {
            int lx = sg.swX - S(180) + i * (sg.chipW + S(6));
            bool sel = (g_cfg.playMode == i);
            SDL_Rect lr = {lx, sg.chipY, sg.chipW, sg.chipH};
            SDL_SetRenderDrawColor(g_sdlRdr, sel ? 37 : 55, sel ? 99 : 55, sel ? 235 : 58, 255);
            SDL_RenderFillRect(g_sdlRdr, &lr);
            if (!sel) {
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
                SDL_RenderDrawRect(g_sdlRdr, &lr);
            }
            g_text.drawText(lx + S(8), sg.chipY + S(4), modes[i], 11, sel ? 255 : 150, sel ? 255 : 150, sel ? 255 : 150);
        }
    }

    // --- toast notification（M32g 胶囊样式: 居中圆角深底 + 白字） ---
    if (g_ui.toastActive) {
        Uint32 elapsed = SDL_GetTicks() - g_ui.toastStart;
        if (elapsed > ui::TOAST_MS) {
            g_ui.toastActive = false;
        } else {
            float alpha = 1.0f;
            if (elapsed > ui::TOAST_MS - 300)
                alpha = 1.0f - (float)(elapsed - (ui::TOAST_MS - 300)) / 300.0f;
            Uint8 a = (Uint8)(alpha * 255);
            int tw = g_text.measureText(g_ui.toastMsg, 13);
            int bh = S(36), capR = bh / 2, padX = S(20);
            int bw = tw + padX * 2;
            int bx = w / 2 - bw / 2, by = S(60);
            SDL_SetRenderDrawColor(g_sdlRdr, 15, 15, 17, a);
            SDL_Rect mid = {bx + capR, by, bw - capR * 2, bh};
            SDL_RenderFillRect(g_sdlRdr, &mid);
            fillCircle(g_sdlRdr, bx + capR, by + bh / 2, bh / 2, 15, 15, 17, a);
            fillCircle(g_sdlRdr, bx + bw - capR, by + bh / 2, bh / 2, 15, 15, 17, a);
            // border (white.10)
            Uint8 ba = (Uint8)(26 * alpha);
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, ba);
            SDL_RenderDrawRect(g_sdlRdr, &mid);
            g_text.drawText(w / 2 - tw / 2, by + S(8), g_ui.toastMsg, 13, 255, 255, 255);
        }
    }

    // --- OSD 信息叠加（按 I 切换，8 秒自动消失） ---
    if (g_ui.osdActive) {
        if (SDL_GetTicks() - g_ui.osdStart > 8000) {
            g_ui.osdActive = false;
        } else {
            std::string vfmt   = mpvStr("video-format");
            std::string vfps   = mpvStr("container-fps");
            if (vfps.empty()) vfps = mpvStr("estimated-vf-fps");
            std::string vbr    = formatBitrate(mpvStr("video-bitrate"));
            std::string afmt   = mpvStr("audio-codec-name");
            std::string asr    = mpvStr("audio-params/samplerate");
            std::string ach    = mpvStr("audio-params/channel-count");
            int vw = g_mpv->videoWidth(), vh = g_mpv->videoHeight();

            char line1[128] = {}, line2[64] = {}, line3[96] = {};
            if (!vfmt.empty())
                std::snprintf(line1, sizeof(line1), "%s  %dx%d%s%s",
                    vfmt.c_str(), vw, vh,
                    vfps.empty() ? "" : " @ ", vfps.c_str());
            if (!vbr.empty()) std::snprintf(line2, sizeof(line2), "%s", vbr.c_str());
            if (!afmt.empty()) {
                int sr = std::atoi(asr.c_str());
                std::snprintf(line3, sizeof(line3), "%s %s Hz %sch",
                    afmt.c_str(),
                    sr > 0 ? asr.c_str() : "?",
                    ach.empty() ? "?" : ach.c_str());
            }

            // 面板尺寸随内容
            int lines = 0;
            if (line1[0]) ++lines;
            if (line2[0]) ++lines;
            if (line3[0]) ++lines;
            if (lines > 0) {
                int padX = S(14), padY = S(10), lineH = S(22);
                int boxW = S(340), boxH = padY * 2 + lines * lineH;
                int boxX = S(16), boxY = S(ui::TOPBAR_H) + S(12);
                SDL_Rect bg = {boxX, boxY, boxW, boxH};
                SDL_SetRenderDrawColor(g_sdlRdr, 11, 11, 11, 200);
                SDL_RenderFillRect(g_sdlRdr, &bg);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 30);
                SDL_RenderDrawRect(g_sdlRdr, &bg);

                int ty = boxY + padY;
                if (line1[0]) { g_text.drawText(boxX + padX, ty, line1, 12, 255, 255, 255); ty += lineH; }
                if (line2[0]) { g_text.drawText(boxX + padX, ty, line2, 12, 161, 161, 166); ty += lineH; }
                if (line3[0]) { g_text.drawText(boxX + padX, ty, line3, 12, 161, 161, 166); }
            }
        }
    }

    SDL_RenderPresent(g_sdlRdr);
}

// ---- DPI awareness ----
// 125%+ 缩放下非 aware 进程坐标被虚拟化: 鼠标命中/截图测试全部错位,
// 且渲染被拉伸模糊。PER_MONITOR_AWARE_V2 让所有坐标统一为物理像素。
static void enableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        using FnCtx = BOOL(WINAPI*)(HANDLE);
        auto fCtx = (FnCtx)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fCtx && fCtx((HANDLE)-4)) return;   // PER_MONITOR_AWARE_V2
        using Fn = BOOL(WINAPI*)(void);
        auto fAware = (Fn)(void*)GetProcAddress(u32, "SetProcessDPIAware");
        if (fAware && fAware()) return;
    }
}

// ---- main ----
int main(int argc, char** argv) {
    enableDpiAwareness();
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
wc.style         = CS_DBLCLKS;   // 接收 WM_LBUTTONDBLCLK
    wc.lpfnWndProc   = parentProc;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName  = L"VPlayerParent";
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    // 创建窗口前先取主屏 DPI，保证 S(960)xS(540) 按物理像素展开
    {
        HDC dc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, dc);
        LOG_INFO("MAIN", "initial dpi scale=%.2f", g_dpi);
    }
    // 记忆位置优先；无效则默认尺寸 + 系统级联位置
    int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT;
    int winW = S(960), winH = S(540);
    if (g_cfg.posX != AppConfig::INVALID_POS && g_cfg.posW > 0) {
        winX = g_cfg.posX; winY = g_cfg.posY;
        winW = g_cfg.posW; winH = g_cfg.posH;
        LOG_INFO("MAIN", "restore window pos (%d,%d) %dx%d", winX, winY, winW, winH);
    }
    g_parentHwnd = CreateWindowExW(WS_EX_ACCEPTFILES,
        wc.lpszClassName, L"VPlayer", WS_OVERLAPPEDWINDOW,
        winX, winY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_parentHwnd) { LOG_ERROR("MAIN", "CreateWindow failed"); return 1; }
    updateDpiForWindow(g_parentHwnd);   // 以窗口所在显示器为准精调
    LOG_INFO("MAIN", "window dpi scale=%.2f", g_dpi);

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
    // 安装输入中继（见 mpvRelayProc 注释）
    g_mpvOldProc = (WNDPROC)SetWindowLongPtrW(g_mpvHwnd, GWLP_WNDPROC,
                                              (LONG_PTR)mpvRelayProc);

    MpvBackend mpv;
    g_mpv = &mpv;
    mpv.setVolume(g_cfg.volume);
    if (g_cfg.speed >= 0.25f && g_cfg.speed <= 4.0f && std::abs(g_cfg.speed - 1.0f) > 0.01f)
        mpv.setSpeed(g_cfg.speed);
    mpv.onFileLoaded = [&]() {
        // 续播：FILE_LOADED 后跳到上次位置
        if (g_pendingResumePos > 1.0 && g_mpv) {
            LOG_INFO("MAIN", "resume at %.1fs", g_pendingResumePos);
            mpv.seek(g_pendingResumePos);
            char msg[48];
            std::snprintf(msg, sizeof(msg), "Resumed at %02d:%02d",
                (int)(g_pendingResumePos / 60), (int)g_pendingResumePos % 60);
            showToast(msg);
        }
        g_pendingResumePos = -1.0;
    };
    mpv.onPlaybackEnded = [&]() {
        LOG_INFO("MAIN", "playback ended");
        if (!g_mpv) return;
        std::string cur = g_mpv->path();
        if (!cur.empty()) g_cfg.history[cur] = 0;   // 看完清零
        int idx = playlistIndexOf(cur);
        int n = (int)g_playlist.size();
        if (idx < 0 || n == 0) return;

        if (g_cfg.playMode == 0) {                   // Single：停住
            showToast("End of track");
        } else if (g_cfg.playMode == 2) {            // Shuffle
            if (n > 1) {
                int next = idx;
                while (next == idx) next = std::rand() % n;
                playIndex(next);
            }
        } else {                                     // Loop：顺序循环
            playIndex((idx + 1) % n);
        }
    };

    if (!mpv.init(g_mpvHwnd)) { LOG_ERROR("MAIN", "mpv init failed"); return 1; }

    // 按配置应用运行时选项
    mpvSetOpt("hwdec", g_cfg.hwDecode ? "auto-safe" : "no");
    mpvSetOpt("sub-auto", g_cfg.subAutoLoad ? "fuzzy" : "no");
    mpvSetOpt("audio-exclusive", g_cfg.audioExclusive ? "yes" : "no");
    rebuildAudioFilters();
    if (g_cfg.motionInterp) applyMotionInterp(true);
    if (g_cfg.hiQScale) {
        mpvSetOpt("scale", "ewa_lanczossharp");
        mpvSetOpt("cscale", "ewa_lanczossharp");
    }

    // ---- SDL2 overlay（owned 顶层窗口，不进任务栏） ----
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
    if (initialFile.empty() && g_cfg.resume && !g_cfg.lastFile.empty()) {
        initialFile = g_cfg.lastFile;
        // 恢复上次播放：队列重建自其所在目录
        buildPlaylistAround(initialFile);
        playPath(initialFile);
    } else if (!initialFile.empty()) {
        buildPlaylistAround(initialFile);
        playPath(initialFile);
    }

    LOG_INFO("MAIN", "entering main loop (playlist=%d)", (int)g_playlist.size());

    // 缩略图 worker（含磁盘缓存目录）
    CreateDirectoryA((exeDir() + "cache").c_str(), nullptr);
    CreateDirectoryA(thumbCacheDir().c_str(), nullptr);
    thumbCacheCleanup(7);
    g_thumbQuit.store(false);
    g_thumbThread = std::thread(thumbWorkerMain);

    // ---- main loop（按需渲染：脏标记 + 定时唤醒 + 空闲阻塞） ----
    bool running = true;
    Uint32 lastPosSave = 0;
    int lastPosSec = -1;
    auto lastState = mpv.state();
    Uint32 waitCap = 200;
    while (running) {
        MSG msg;
        BOOL hasMsg = FALSE;
        while ((hasMsg = PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) != 0) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);   // parentProc 入口已置 dirty
        }
        if (!running) break;

        Uint32 now = SDL_GetTicks();

        // 播放状态轮询：进度秒变 / 播放状态切换 -> dirty
        if (g_mpv && g_mpv->hasMedia()) {
            double pos = g_mpv->clock();
            if ((int)pos != lastPosSec) { lastPosSec = (int)pos; g_dirty.store(true); }
            auto st = g_mpv->state();
            if (st != lastState) { lastState = st; g_dirty.store(true); }
        }

        // 周期保存播放进度（3 秒）
        if (now - lastPosSave >= 3000) {
            lastPosSave = now;
            if (g_mpv && g_mpv->hasMedia() && g_mpv->state() == MpvBackend::State::Playing) {
                double pos = g_mpv->clock();
                double dur = g_mpv->duration();
                std::string cur = g_mpv->path();
                if (!cur.empty() && dur > 0)
                    g_cfg.history[cur] = (pos < dur - 2.0) ? pos : 0.0;  // 结尾视为看完
            }
        }

        // 定时状态迁移（迁移动作本身置 dirty）
        if (g_ui.visible && now > g_ui.hideAt) {
            g_ui.visible = false;
            g_dirty.store(true);
        }

        // 控件淡入淡出逼近（M30 缓动 + 效果图 cb-opacity 最低 0.25）
        {
            float target = g_ui.visible ? 1.0f : 0.25f;
            float cur = g_ui.ctrlAlpha;
            if ((target > cur && cur < target) || (target < cur && cur > target)) {
                float dir = (target > cur) ? 1.0f : -1.0f;
                cur += dir * 0.08f;
                if ((dir > 0 && cur >= target) || (dir < 0 && cur <= target))
                    cur = target;
                g_ui.ctrlAlpha = cur;
                g_dirty.store(true);
                waitCap = 16;   // 动画期间高频刷新
            }
        }
        if (g_ui.volumeSliderOpen && !g_ui.volumeDragging &&
            now > g_ui.volHoverAt + 1200) {
            g_ui.volumeSliderOpen = false;
            LOG_DBG("MAIN", "volume slider auto-collapse (idle)");
            g_dirty.store(true);
        }
        if (g_ui.toastActive && now - g_ui.toastStart > ui::TOAST_MS + 50) {
            g_ui.toastActive = false;
            g_dirty.store(true);
        }
        if (g_ui.osdActive && now - g_ui.osdStart > 8000) {
            g_ui.osdActive = false;
            g_dirty.store(true);
        }

        // 仅脏时渲染
        if (g_dirty.exchange(false)) {
            renderOverlay();
        }

        // 计算最近唤醒点（cap 200ms 保证播放中进度秒变响应；空闲更久）
        Uint32 wait = 200;
        if (waitCap < wait) wait = waitCap;
        waitCap = 200;
        auto upd = [&](Uint32 deadline) {
            if (deadline > now && deadline - now < wait) wait = deadline - now;
        };
        upd(g_ui.hideAt);
        if (g_ui.volumeSliderOpen) upd(g_ui.volHoverAt + 1200);
        if (g_ui.toastActive)      upd(g_ui.toastStart + ui::TOAST_MS + 60);
        if (g_ui.osdActive)        upd(g_ui.osdStart + 8050);
        // 进度保存(3s 周期)由 200ms 兜底轮询覆盖, 无需专门加速

        MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    // 退出前保存最终进度 + 窗口位置（WM_CLOSE 已存，此处兜底）
    if (g_mpv && g_mpv->hasMedia()) {
        double pos = g_mpv->clock();
        double dur = g_mpv->duration();
        std::string cur = g_mpv->path();
        if (!cur.empty() && dur > 0)
            g_cfg.history[cur] = (pos < dur - 2.0) ? pos : 0.0;
    }
    saveWindowPos(g_parentHwnd);

    g_thumbQuit.store(true);
    if (g_thumbThread.joinable()) g_thumbThread.join();

    mpv.close();
    saveConfig(configPath(), g_cfg);
    destroyOverlay();
    DestroyWindow(g_parentHwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    SDL_Quit();
    LOG_INFO("MAIN", "vplayer exiting");
    return 0;
}
