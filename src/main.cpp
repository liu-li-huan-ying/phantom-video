#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_syswm.h>

static const char* PHANTOM_VERSION = "0.1.0";

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <mutex>
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

// M36: 文件夹选择 (SHBrowseForFolderW + NEWDIALOGSTYLE 可调大小/新建按钮)
// 注: w64devkit 的 shobjidl.h 只前置声明 IFileDialog(无完整 vtable),
//     IFileDialog 方案不可用, 故用经典 API
static std::string openFolderDialog(HWND hwnd) {
    std::string result;
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool coOwned = SUCCEEDED(coInit);
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"Select folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH * 2] = {};
        if (SHGetPathFromIDListW(pidl, path)) result = WideToUtf8(path);
        CoTaskMemFree(pidl);
    }
    if (coOwned) CoUninitialize();
    return result;
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

    // topbar icon hover (-1=none, 0=close, 1=maximize, ...)
    int    topbarHover   = -1;

    // M36: 欢迎页命中区 (每帧渲染时重建)
    SDL_Rect heroFileBtn   = {};
    SDL_Rect heroFolderBtn = {};
    std::vector<std::pair<std::string, SDL_Rect>> continueHits;   // 继续观看卡片
    std::vector<std::pair<int, SDL_Rect>>         gridHits;       // 队列网格卡片 (playlist idx)
    float introAlpha = 1.0f;   // 入场淡入 0→1

    // speed popup
    bool   speedMenuOpen = false;

    // quality popup
    bool   qualityMenuOpen = false;
    int    qualityPreset = 1;   // 0=省电 1=标准 2=至臻

    // EQ popup
    bool   eqMenuOpen = false;
    int    eqDraggingBand = -1; // 正在拖动的频段, -1=无

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

// ---- i18n: 双语字符串表 (0=中文 1=English) ----
static const char* T(const char* zh, const char* en) {
    return g_cfg.lang == 0 ? zh : en;
}
namespace i18n {
    inline const char* subtitles()  { return T("字幕", "Subtitles"); }
    inline const char* speed()      { return T("倍速", "Speed"); }
    inline const char* quality()    { return T("画质", "Quality"); }
    inline const char* settings()   { return T("设置", "Settings"); }
    inline const char* settingsTitle()  { return T("设置", "Settings"); }
    inline const char* language()       { return T("语言", "Language"); }
    inline const char* chinese()        { return T("中文", "Chinese"); }
    inline const char* english()        { return T("English", "English"); }
    inline const char* hwDecode()       { return T("硬件解码", "Hardware Decode"); }
    inline const char* volNorm()        { return T("音量标准化", "Volume Normalization"); }
    inline const char* subAutoLoad()    { return T("字幕自动加载", "Subtitle Auto-Load"); }
    inline const char* thumbCache()     { return T("缩略图缓存", "Thumbnail Cache"); }
    inline const char* resume()         { return T("续播记忆", "Resume Playback"); }
    inline const char* nightMode()      { return T("夜间模式", "Night Mode"); }
    inline const char* exclusiveAudio() { return T("独占音频", "Exclusive Audio"); }
    inline const char* motionInterp()   { return T("运动插值", "Motion Interpolation"); }
    inline const char* hiQScaling()     { return T("高质量缩放", "HQ Scaling"); }
    inline const char* playbackMode()   { return T("播放模式", "Playback Mode"); }
    inline const char* modeSingle()     { return T("单曲", "Single"); }
    inline const char* modeLoop()       { return T("循环", "Loop"); }
    inline const char* modeShuffle()    { return T("随机", "Shuffle"); }
    inline const char* playlist()       { return T("播放列表", "Playlist"); }
    inline const char* playing()        { return T("正在播放", "Playing"); }
    inline const char* played()         { return T("已播放", "Played"); }
    inline const char* unplayed()       { return T("未播放", "Unplayed"); }
    inline const char* emptyPlaylist()  { return T("无文件", "No files"); }
    inline const char* dropHint()       { return T("拖入视频文件", "Drop video here"); }
    inline const char* ctrlOHint()      { return T("或按 Ctrl+O", "or press Ctrl+O"); }
    inline const char* equalizer()      { return T("均衡器", "Equalizer"); }
    inline const char* reset()          { return T("重置", "Reset"); }
    inline const char* muted()          { return T("已静音", "Muted"); }
    inline const char* unmuted()        { return T("已取消静音", "Unmuted"); }
    inline const char* subtitlesOn()    { return T("字幕已开启", "Subtitles ON"); }
    inline const char* subtitlesOff()   { return T("字幕已关闭", "Subtitles OFF"); }
    inline const char* loopASet()       { return T("已设置 A 点", "Loop A set"); }
    inline const char* loopActive()     { return T("AB 循环中", "AB loop active"); }
    inline const char* loopCleared()    { return T("循环已清除", "Loop cleared"); }
    inline const char* singleTrack()    { return T("单音轨", "Single audio track"); }
    inline const char* playlistReordered() { return T("列表已重排", "Playlist reordered"); }
    inline const char* failedOpen()     { return T("打开失败", "Failed to open file"); }
    inline const char* eqReset()        { return T("EQ 已重置", "EQ reset"); }
    inline const char* noPrev()         { return T("无上一曲", "No previous track"); }
    inline const char* noNext()         { return T("无下一曲", "No next track"); }
    inline const char* screenshotSaved() { return T("截图已保存", "Screenshot saved"); }
    inline const char* screenshotFailed() { return T("截图失败", "Screenshot failed"); }
    inline const char* pipOn()          { return T("画中画已开启", "PIP ON"); }
    inline const char* pipOff()         { return T("画中画已关闭", "PIP OFF"); }
    inline const char* buffering()      { return T("缓冲中...", "Buffering..."); }
    inline const char* endOfTrack()     { return T("播放结束", "End of track"); }
    inline const char* resumedAt()      { return T("已续播", "Resumed at"); }
    inline const char* modeSingleT()   { return T("模式: 单曲", "Mode: Single"); }
    inline const char* modeLoopT()     { return T("模式: 循环", "Mode: Loop"); }
    inline const char* modeShuffleT()  { return T("模式: 随机", "Mode: Shuffle"); }
    // M36: 欢迎页
    inline const char* appName()       { return T("幻影视频", "Phantom Video"); }
    inline const char* tagline()       { return T("轻 · 快 · 纯粹的本地视频体验", "Light, fast, pure local video"); }
    inline const char* openFile()      { return T("打开文件", "Open File"); }
    inline const char* openFolder()    { return T("打开文件夹", "Open Folder"); }
    inline const char* dropAnywhere()  { return T("或直接拖拽视频到窗口", "or drop a video anywhere"); }
    inline const char* continueWatching() { return T("继续观看", "Continue Watching"); }
    inline const char* folderEmpty()   { return T("该文件夹没有视频文件", "No videos in this folder"); }
    inline const char* debandOff()     { return T("关闭", "Off"); }
    inline const char* debandLight()   { return T("轻", "Light"); }
    inline const char* debandMedium()  { return T("中", "Medium"); }
    inline const char* debandStrong()  { return T("强", "Strong"); }
    inline const char* subBottom()     { return T("底部", "Bottom"); }
    inline const char* subCenter()     { return T("居中", "Center"); }
    inline const char* subTop()        { return T("顶部", "Top"); }
}

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

// ---- quality presets ----
struct QualityPreset {
    const char* name;
    const char* scale;      // 上采样
    const char* dscale;     // 下采样
    const char* cscale;     // 色度上采样
    int         deband;     // 去色带 0/1
    float       antiring;   // 振铃抑制
};
static const QualityPreset QUALITY_PRESETS[] = {
    { "省电",  "bilinear", "bilinear", "bilinear", 0, 0.0f },
    { "标准",  "spline36", "mitchell", "spline36", 1, 0.7f },
    { "至臻",  "ewa_lanczossharp", "ewa_lanczossharp", "ewa_lanczossharp", 1, 0.7f },
};
static const int QUALITY_PRESET_COUNT = 3;

static void applyQualityPreset(int idx) {
    if (!g_mpv || !g_mpv->mpv() || idx < 0 || idx >= QUALITY_PRESET_COUNT) return;
    const QualityPreset& p = QUALITY_PRESETS[idx];
    auto set = [](const char* k, const char* v) {
        mpv_set_property_string(g_mpv->mpv(), k, v);
    };
    auto setf = [](const char* k, float v) {
        mpv_set_property(g_mpv->mpv(), k, MPV_FORMAT_DOUBLE, &v);
    };
    set("scale", p.scale);
    set("dscale", p.dscale);
    set("cscale", p.cscale);
    int deband = p.deband;
    mpv_set_property(g_mpv->mpv(), "deband", MPV_FORMAT_FLAG, &deband);
    setf("scale-antiring", p.antiring);
    g_ui.qualityPreset = idx;
    LOG_INFO("MAIN", "quality preset -> %s (scale=%s deband=%d)", p.name, p.scale, p.deband);
}

// ---- 统一 UI 缩放 ----
// 参考点: 1280x720 窗口, 96 DPI。所有 UI 像素和文字大小从这里派生。
static float g_dpi = 1.0f;
static float g_uiBase = 1.0f;   // 窗口缩放因子 = min(winW/1280, winH/720)

// 像素度量(DPI + 窗口比例), 最小 1px
static int U(int v) {
    float s = g_dpi * g_uiBase;
    return std::max(1, (int)(v * s + 0.5f));
}

// 文字点号(DPI 已由 GDI 处理, 此处只做窗口比例)
static int T(int pt) {
    return std::max(8, (int)(pt * g_uiBase + 0.5f));
}

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

static int sbTopY()    { return g_ui.winH - U(80); }
static int sbTrackY()  { return sbTopY() + U(10); }
static int sbLeftX()   { return U(16); }
static int sbRightX()  { return g_ui.winW - U(16); }
static int sbWidth()   { return sbRightX() - sbLeftX(); }
static int curCtrlH()  { return U(80); }
static int curTopH()   { return U(52); }

// ---- 窗口位置保存（须在窗口销毁前调用） ----
static void saveWindowPos(HWND hwnd) {
    if (!IsWindow(hwnd)) return;
    if (g_ui.fullscreen || g_ui.miniMode || IsIconic(hwnd) || IsZoomed(hwnd)) return;
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return;
    // 防御: 异常尺寸不入库
    if (wr.right - wr.left < U(300) || wr.bottom - wr.top < U(200)) return;
    int w = wr.right - wr.left;
    // 列表展开时窗口含 +U(430) 扩展区, 必须扣除, 否则下次启动窗口虚胖
    if (g_ui.playlistOpen && !g_ui.fullscreen) w -= U(430);
    g_cfg.posX = wr.left; g_cfg.posY = wr.top;
    g_cfg.posW = w; g_cfg.posH = wr.bottom - wr.top;
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

static void showToast(const char* msg);
static const char* qualityLabel();
static void renderOverlay();

static void layoutRow1(int w, int h, bool volOpen, Row1Layout& L) {
    const int pad = U(16);
    const int iconSz = U(34);
    const int playSz = U(42);
    int cy = h - U(80) + U(50);
    L.cy = cy;
    int x = pad;
    auto iconBtn = [&](SDL_Rect& rc) {
        rc = {x, cy - iconSz / 2, iconSz, iconSz};
        x += iconSz + U(2);
    };
    iconBtn(L.prev);
    L.play = {x, cy - playSz / 2, playSz, playSz};
    x += playSz + U(4);
    iconBtn(L.next);
    // time
    L.timeX = x + U(10);
    char tbuf[80];
    formatTime(tbuf, sizeof(tbuf), 0);
    std::string sample = std::string(tbuf) + " / " + tbuf;
    x += U(10) + g_text.measureText(sample, T(12)) + U(12);

    // 右侧组: 从右往左
    int xr = w - pad;
    auto placeRight = [&](SDL_Rect& rc, int bw) {
        xr -= bw;
        rc = {xr, cy - iconSz / 2, bw, iconSz};
        xr -= U(4);  // 按钮间距
    };
    placeRight(L.fullBtn, iconSz);
    placeRight(L.setBtn, g_text.measureText(i18n::settings(), T(12)) + U(26));
    L.volSliderW = volOpen ? U(80) : 0;
    placeRight(L.qualityBtn, g_text.measureText(i18n::quality(), T(12)) + g_text.measureText(qualityLabel(), T(11)) + U(22));
    {
        char spd[16];
        float s = g_mpv ? g_mpv->speed() : 1.0f;
        if (s == (int)s) std::snprintf(spd, sizeof(spd), "%.0fx", s);
        else             std::snprintf(spd, sizeof(spd), "%.2fx", s);
        placeRight(L.speedBtn, g_text.measureText(spd, T(12)) + g_text.measureText(i18n::speed(), T(12)) + U(24));
    }
        placeRight(L.subBtn, g_text.measureText(i18n::subtitles(), T(12)) + U(26));
    // 音量 wrap: slider(open) + icon
    L.volIconCx = xr - U(17);
    if (volOpen) { L.volSliderX = xr - U(34) - U(80) + U(5); xr -= U(84); }
}

static void showToast(const char* msg) {
    std::snprintf(g_ui.toastMsg, sizeof(g_ui.toastMsg), "%s", msg);
    g_ui.toastActive = true;
    g_ui.toastStart = SDL_GetTicks();
}

static const char* qualityLabel() {
    if (!g_mpv) return "---";
    int h = g_mpv->videoHeight();
    if (h <= 0) return "---";
    if (h <= 240)  return "240p";
    if (h <= 360)  return "360p";
    if (h <= 480)  return "480p";
    if (h <= 576)  return "576p";
    if (h <= 720)  return "720p";
    if (h <= 1080) return "1080p";
    if (h <= 1440) return "2K";
    if (h <= 2160) return "4K";
    return "8K";
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
    int langRowY;               // 语言行 Y
    int langSegX, langSegW, langSegH; // 语言分段控件
};
static const int SET_ROW_COUNT = 9;

static SettingsGeom settingsGeom(int w, int h) {
    SettingsGeom g;
    g.panelW = U(400); g.panelH = U(520);
    g.panelX = (w - g.panelW) / 2;
    g.panelY = (h - g.panelH) / 2;
    // 小窗口: 面板尽量整体抬到控制栏上方, 底部行不被遮挡
    {
        int limit = h - curCtrlH() - U(12);
        if (g.panelY + g.panelH > limit)
            g.panelY = std::max(U(6), limit - g.panelH);
    }
    g.closeCx = g.panelX + g.panelW - U(22);
    g.closeCy = g.panelY + U(22);
    g.closeR = U(12);
    g.swX = g.panelX + g.panelW - U(60);
    g.swW = U(40); g.swH = U(20);
    for (int i = 0; i < SET_ROW_COUNT; ++i)
        g.rowY[i] = g.panelY + U(55) + i * U(40);
    // 模式行放在最后一行开关下方, 与开关区留足间隔避免命中重叠
    g.modeRowY = g.rowY[SET_ROW_COUNT - 1] + U(36);
    g.chipY = g.modeRowY;
    g.chipH = U(24); g.chipW = U(56);
    // 语言行: 在播放模式下方; 分段加宽保证 "English" 不溢出
    g.langRowY = g.modeRowY + U(40);
    g.langSegW = U(150);
    g.langSegX = g.swX - g.langSegW;
    g.langSegH = U(24);
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

// 播放列表开/关 -> 主窗口宽度增减 U(430)（右侧独立区域，不遮挡视频）
// 注意: 无边框窗口的 GetWindowRect 比 client 多出隐藏边框(~18px),
// 必须以 client 增量换算回 window 增量, 否则列表区宽度漂移
static void applyPlaylistWindow(HWND hwnd) {
    RECT rc, wr;
    GetClientRect(hwnd, &rc);
    GetWindowRect(hwnd, &wr);
    int frameW = (wr.right - wr.left) - (rc.right - rc.left);
    int frameH = (wr.bottom - wr.top) - (rc.bottom - rc.top);
    int newWinW = (rc.right - rc.left) + (g_ui.playlistOpen ? U(430) : -U(430)) + frameW;
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
        int w = U(480), h = U(270);
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(hwnd, HWND_TOPMOST,
            wa.right - w - U(20), wa.bottom - h - U(20), w, h,
            SWP_NOZORDER | SWP_NOACTIVATE);
        g_ui.miniMode = true;
        LOG_INFO("MAIN", "pip mini ON (%dx%d)", w, h);
        showToast(i18n::pipOn());
    } else {
        SetWindowPos(hwnd, HWND_NOTOPMOST,
            g_ui.savedRect.left, g_ui.savedRect.top,
            g_ui.savedRect.right  - g_ui.savedRect.left,
            g_ui.savedRect.bottom - g_ui.savedRect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        g_ui.miniMode = false;
        LOG_INFO("MAIN", "pip mini OFF");
        showToast(i18n::pipOff());
    }
    raiseOverlayAbove();
}

// ---- 播放队列（稳定顺序，文件夹扫描生成，不随播放重排） ----
static std::vector<std::string> g_playlist;

static void clampPlaylistScroll() {
    int contentH = (int)g_playlist.size() * U(72);
    int viewH = g_ui.winH - curTopH() - U(55);
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

// 扫描目录内视频文件 (自然排序, UTF-8 路径) — buildPlaylistAround/FromFolder 共用
static std::vector<std::string> scanVideoDirUtf8(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    std::vector<fs::path> found;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (found.size() >= PLAYLIST_MAX) break;
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().string();   // 扩展名 ASCII, ANSI 读取安全
        for (auto* ve : kVideoExts) {
            if (_stricmp(ext.c_str(), ve) == 0) { found.push_back(e.path()); break; }
        }
    }
    std::sort(found.begin(), found.end(), [](const fs::path& x, const fs::path& y) {
        return naturalLess(x.filename().wstring(), y.filename().wstring());
    });
    for (auto& f : found) out.push_back(WideToUtf8(f.wstring()));   // 宽->UTF-8
    return out;
}

// 以 file 所在目录扫描视频文件构建播放队列（自然顺序）
static void buildPlaylistAround(const std::string& file) {
    namespace fs = std::filesystem;
    g_playlist.clear();
    fs::path p(Utf8ToWide(file));                       // 宽字符构造, 杜绝 ANSI 误读
    fs::path dir = p.parent_path();
    g_playlist = scanVideoDirUtf8(dir);
    if (g_playlist.empty()) { g_playlist.push_back(file); return; }
    {
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

// M36: 直接以文件夹构建播放队列 — 欢迎页「打开文件夹」按钮
static bool buildPlaylistFromFolder(const std::string& dirUtf8) {
    g_playlist = scanVideoDirUtf8(Utf8ToWide(dirUtf8));
    return !g_playlist.empty();
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
// EOF 自动连播: 事件线程只投递, UI 主循环消费(避免跨线程 mpv/UI 调用死锁)
static std::mutex g_autoNextMtx;
static std::string g_autoNextPath;
static bool g_autoNextPending = false;
static double g_resumeSeekPos = -1.0;      // FILE_LOADED 投递的续播位置
static bool g_resumeSeekPending = false;
// M36: 统一历史写入 (pos + dur + 时间戳)
static void recordHistory(const std::string& path, double pos, double dur) {
    if (path.empty()) return;
    HistoryEntry& e = g_cfg.history[path];
    e.pos = pos;
    if (dur > 0) e.dur = dur;
    e.lastPlayed = (long long)std::time(nullptr);
}
static void playPath(const std::string& path) {
    if (!g_mpv || path.empty()) return;
    g_pendingResumePos = -1.0;
    auto it = g_cfg.history.find(path);
    if (g_cfg.resume && it != g_cfg.history.end() && it->second.pos > 1.0)
        g_pendingResumePos = it->second.pos;
    if (!g_mpv->loadFile(path)) {
        showToast(i18n::failedOpen());
        return;
    }
    g_cfg.lastFile = path;

    // 播放列表面板打开时，滚动到当前项附近
    if (g_ui.playlistOpen) {
        int idx = playlistIndexOf(path);
        if (idx >= 0) {
            int itemH = U(72);
            int viewH = g_ui.winH - curTopH() - U(55);
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
    int row1Off = U(50);
    return my >= barTop + row1Off - U(17) && my <= barTop + row1Off + U(17) &&
           mx >= g_ui.winW - U(170) && mx <= g_ui.winW - U(38);
}

// ---- topbar icon hit test ----
static int hitTestTopbarIcon(int mx, int my, int winW) {
    if (my < 0 || my > curTopH()) return -1;
    int iconY = curTopH() / 2;
    int iconHalf = U(12);
    int rx = winW - U(20);
    int icoSp = U(34);
    struct IDef { const char* id; int idIdx; };
    static const IDef icons[] = {
        {"close", 0}, {"maximize", 1}, {"minimize", 2},
        {"list", 3}, {"pip", 4}, {"camera", 5}
    };
    for (int i = 0; i < 6; ++i) {
        if (mx >= rx - iconHalf && mx <= rx + iconHalf &&
            my >= iconY - iconHalf && my <= iconY + iconHalf)
            return icons[i].idIdx;
        rx -= icoSp;
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
        int panelExtra = (g_ui.playlistOpen && !g_ui.fullscreen) ? U(430) : 0;
        g_ui.winW = rc.right - panelExtra;
        g_ui.winH = rc.bottom;
        float oldBase = g_uiBase;
        g_uiBase = std::min(g_ui.winW / 1280.0f, g_ui.winH / 720.0f);
        g_uiBase = std::max(0.45f, std::min(g_uiBase, 2.0f));
        LOG_DBG("MAIN", "WM_SIZE %ux%u wp=%u panelExtra=%d winW=%d base=%.2f->%.2f",
                rc.right, rc.bottom, wp, panelExtra, g_ui.winW, oldBase, g_uiBase);
        if (g_mpvHwnd) MoveWindow(g_mpvHwnd, 0, 0, g_ui.winW, rc.bottom, TRUE);
        if (g_sdlWin) {
            POINT pt = {0,0}; ClientToScreen(hwnd, &pt);
            SDL_SetWindowPosition(g_sdlWin, pt.x, pt.y);
            SDL_SetWindowSize(g_sdlWin, rc.right, rc.bottom);
        }
        // 即时重绘 — 不等主循环唤醒
        renderOverlay();
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

    // ---- 焦点管理: 父窗口失去焦点时隐藏 overlay, 避免浮在其他窗口上面 ----
    case WM_ACTIVATEAPP: {
        bool active = (wp != 0);
        LOG_DBG("MAIN", "WM_ACTIVATEAPP active=%d mini=%d", active, g_ui.miniMode ? 1 : 0);
        if (g_sdlWin && !g_ui.miniMode) {
            if (active) {
                LOG_DBG("MAIN", "overlay show (focus gained)");
                SDL_ShowWindow(g_sdlWin);
                raiseOverlayAbove();
            } else {
                LOG_DBG("MAIN", "overlay hide (focus lost)");
                SDL_HideWindow(g_sdlWin);
            }
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
                showToast(g_mpv->muted() ? i18n::muted() : i18n::unmuted());
                break;
            }
            case 'N': g_mpv->seekRelative( 10.0); break;
            case 'P': g_mpv->seekRelative(-10.0); break;
            case '[':
            case 0xDB:   // VK_OEM_4: Windows 对 [ 键发的虚拟码(≠ASCII 0x5B)
            {
                g_mpv->setSpeed(g_mpv->speed() - 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("倍速", "Speed"), g_mpv->speed());
                showToast(msg);
                break;
            }
            case ']':
            case 0xDD:   // VK_OEM_6
            {
                g_mpv->setSpeed(g_mpv->speed() + 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("倍速", "Speed"), g_mpv->speed());
                showToast(msg);
                break;
            }
            case 'C': {
                bool vis = !g_mpv->subVisible();
                g_mpv->setSubVisibility(vis);
                showToast(vis ? i18n::subtitlesOn() : i18n::subtitlesOff());
                break;
            }
            case 'X': {
                g_mpv->addSubDelay(-0.5);
                char msg[40];
                std::snprintf(msg, sizeof(msg), "%s: %.1fs", T("字幕延迟", "Sub delay"), -g_mpv->subDelay());
                showToast(msg);
                break;
            }
            case 'Z': {
                g_mpv->addSubDelay(0.5);
                char msg[40];
                std::snprintf(msg, sizeof(msg), "%s: %.1fs", T("字幕延迟", "Sub delay"), -g_mpv->subDelay());
                showToast(msg);
                break;
            }
            case 'I':
                g_ui.osdActive = !g_ui.osdActive;
                g_ui.osdStart = SDL_GetTicks();
                LOG_DBG("MAIN", "osd -> %d", g_ui.osdActive ? 1 : 0);
                break;
            case 'A': {  // AB 循环: 第一次设 A, 第二次设 B, 第三次清除
                if (!g_mpv->looping()) {
                    g_mpv->setLoopA();
                    showToast(i18n::loopASet());
                } else if (g_mpv->loopA() >= 0 && g_mpv->loopB() < 0) {
                    g_mpv->setLoopB();
                    showToast(i18n::loopActive());
                } else {
                    g_mpv->clearLoop();
                    showToast(i18n::loopCleared());
                }
                break;
            }
            case 'G': {  // 章节跳转: 跳到下一章
                auto chs = g_mpv->chapters();
                if (!chs.empty()) {
                    int cur = g_mpv->currentChapter();
                    int next = (cur + 1) % (int)chs.size();
                    g_mpv->seekToChapter(next);
                    char msg[64];
                    std::snprintf(msg, sizeof(msg), "%s %d/%d: %s",
                                 T("章节", "Chapter"),
                                 next + 1, (int)chs.size(),
                                 chs[next].title.empty() ? T("无标题", "Untitled") : chs[next].title.c_str());
                    showToast(msg);
                }
                break;
            }
            case 'V': {  // 音轨切换: 循环下一音轨
                auto tracks = g_mpv->audioTracks();
                if (tracks.size() > 1) {
                    int cur = g_mpv->currentAudioTrack();
                    int nextIdx = 0;
                    for (int i = 0; i < (int)tracks.size(); ++i) {
                        if (tracks[i].id == cur && i + 1 < (int)tracks.size()) {
                            nextIdx = i + 1; break;
                        }
                    }
                    g_mpv->setAudioTrack(tracks[nextIdx].id);
                    char msg[64];
                    std::snprintf(msg, sizeof(msg), "%s: %s",
                                 T("音轨", "Audio"),
                                 tracks[nextIdx].desc.empty() ? T("默认", "Default") : tracks[nextIdx].desc.c_str());
                    showToast(msg);
                } else {
                    showToast(i18n::singleTrack());
                }
                break;
            }
            case 'B': {  // 字幕位置: 循环底部/居中/顶部
                static int subPosIdx = 0;
                int positions[] = {100, 50, 10};
                const char* names[] = {i18n::subBottom(), i18n::subCenter(), i18n::subTop()};
                subPosIdx = (subPosIdx + 1) % 3;
                g_mpv->setSubPos(positions[subPosIdx]);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %s", T("字幕", "Sub"), names[subPosIdx]);
                showToast(msg);
                break;
            }
            case 'D': {  // 去色带强度: 关→轻→中→强→关...
                int cur = g_mpv->debandLevel();
                int next = (cur + 1) % 4;
                g_mpv->setDebandLevel(next);
                const char* names[] = {i18n::debandOff(), i18n::debandLight(), i18n::debandMedium(), i18n::debandStrong()};
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %s", T("去色带", "Deband"), names[next]);
                showToast(msg);
                break;
            }
            case 'E': {  // 音频均衡器: 打开/关闭弹窗
                g_ui.eqMenuOpen = !g_ui.eqMenuOpen;
                g_ui.eqDraggingBand = -1;
                break;
            }
            case VK_ESCAPE:
                if (g_ui.speedMenuOpen) g_ui.speedMenuOpen = false;
                else if (g_ui.eqMenuOpen) g_ui.eqMenuOpen = false;
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
        bool onSB = (g_ui.mouseY >= barTop - U(8) && g_ui.mouseY <= barTop + U(12) &&
                     g_ui.mouseX >= sbLeftX()   && g_ui.mouseX <= sbRightX());
        g_ui.seekbarHover = onSB;

        bool onTopbar = (g_ui.mouseY >= 0 && g_ui.mouseY <= curTopH());
        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + (onTopbar ? 4000 : ui::CTRLBAR_HIDE_MS);

        // topbar icon hover
        g_ui.topbarHover = onTopbar ? hitTestTopbarIcon(g_ui.mouseX, g_ui.mouseY, g_ui.totalW) : -1;

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
                        g_ui.mouseX >= g_ui.sbTrackX - U(5) &&
                        g_ui.mouseX <= g_ui.sbTrackX + g_ui.sbTrackW + U(5) &&
                        g_ui.mouseY >= g_ui.sbTrackY &&
                        g_ui.mouseY <= g_ui.sbTrackY + g_ui.sbTrackH;
            g_ui.sbHover = over || g_ui.sbDragging;
            if (g_ui.sbDragging && g_ui.sbTrackX >= 0) {
                int contentH = (int)g_playlist.size() * U(72);
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
            std::abs(g_ui.mouseY - g_ui.plDownY) > U(8)) {
            g_ui.plDragging = true;
            LOG_DBG("MAIN", "playlist drag start from=%d", g_ui.plDragFrom);
        }
        if (g_ui.plDragging) {
            g_ui.plDragY = g_ui.mouseY;
            // 自动滚动：拖到面板上下边缘时滚动列表
            int panelTop = curTopH(), panelBottom = g_ui.winH - U(10);
            if (g_ui.mouseY < panelTop + U(30) && g_ui.playlistScroll > 0)
                g_ui.playlistScroll -= U(12);
            int contentH = (int)g_playlist.size() * U(72);
            int viewH = g_ui.winH - curTopH() - U(55);
            if (g_ui.mouseY > panelBottom - U(30) && g_ui.playlistScroll < contentH - viewH)
                g_ui.playlistScroll += U(12);
        }

        // volume slider drag
        if (g_ui.volumeDragging && g_mpv) {
            Row1Layout L;
            layoutRow1(g_ui.winW, g_ui.winH, true, L);
            float ratio = (float)(g_ui.mouseX - L.volSliderX) / U(70);
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_mpv->setVolume(ratio);
        }

        // EQ slider drag
        if (g_ui.eqDraggingBand >= 0 && g_mpv) {
            int menuW = U(200);
            int itemH = U(36);
            int menuH = U(32) + 6 * itemH + U(40);
            int menuX = g_ui.winW / 2 - menuW / 2;
            int menuY = g_ui.winH / 2 - menuH / 2;
            int trackX = menuX + U(60);
            int trackW = U(100);
            float norm = (float)(g_ui.mouseX - trackX) / trackW;
            if (norm < 0.0f) norm = 0.0f; if (norm > 1.0f) norm = 1.0f;
            float gain = norm * 24.0f - 12.0f;
            g_mpv->setEQBand(g_ui.eqDraggingBand, gain);
            g_dirty.store(true);
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
            const int m = U(6);
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
        if (pt.y >= 0 && pt.y <= curTopH()) {
            if (hitTestTopbarIcon(pt.x, pt.y, g_ui.winW) < 0)
                return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        int barTop = sbTopY();
        LOG_TRACE("MAIN", "parent LBUTTONDOWN (%d,%d) barTop=%d settings=%d playlist=%d",
                  mx, my, barTop, g_ui.settingsOpen ? 1 : 0, g_ui.playlistOpen ? 1 : 0);

        // --- 播放列表关闭钮: 最高优先级 ---
        // (y 在 topbar 高度内会被 topbar 分支拦截: 窗口模式变拖拽, 全屏时与
        //  应用关闭图标重叠导致误关整个程序)
        if (g_ui.playlistOpen && g_ui.plCloseRect.w > 0 &&
            mx >= g_ui.plCloseRect.x && mx <= g_ui.plCloseRect.x + g_ui.plCloseRect.w &&
            my >= g_ui.plCloseRect.y && my <= g_ui.plCloseRect.y + g_ui.plCloseRect.h) {
            g_ui.playlistOpen = false;
            LOG_INFO("MAIN", "playlist close btn");
            if (!g_ui.fullscreen) applyPlaylistWindow(hwnd);
            else g_dirty.store(true);
            return 0;
        }

        // --- topbar icon clicks (列表展开时列表区域不属于 topbar) ---
        bool inPlaylistArea = (g_ui.playlistOpen && !g_ui.fullscreen && mx >= g_ui.winW);
        if (my >= 0 && my <= curTopH() && !inPlaylistArea) {
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
                    showToast(r < 0 ? i18n::screenshotFailed() : i18n::screenshotSaved());
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

        // --- M36: 欢迎页交互（无媒体时）: Hero 按钮 / 继续观看 / 队列网格 ---
        if (!g_mpv || !g_mpv->hasMedia()) {
            auto inRc = [&](const SDL_Rect& rc) {
                return mx >= rc.x && mx <= rc.x + rc.w && my >= rc.y && my <= rc.y + rc.h;
            };
            if (g_ui.heroFileBtn.w > 0 && inRc(g_ui.heroFileBtn)) {
                std::string f = openFileDialog(hwnd);
                LOG_INFO("MAIN", "welcome open-file -> %s", f.c_str());
                if (!f.empty()) { buildPlaylistAround(f); playPath(f); }
                return 0;
            }
            if (g_ui.heroFolderBtn.w > 0 && inRc(g_ui.heroFolderBtn)) {
                std::string d = openFolderDialog(hwnd);
                LOG_INFO("MAIN", "welcome open-folder -> %s", d.c_str());
                if (!d.empty()) {
                    if (buildPlaylistFromFolder(d))
                        playPath(g_playlist[0]);
                    else
                        showToast(i18n::folderEmpty());
                }
                return 0;
            }
            for (auto& kv : g_ui.continueHits) {
                if (inRc(kv.second)) {
                    LOG_INFO("MAIN", "welcome continue-watch click");
                    buildPlaylistAround(kv.first);
                    playPath(kv.first);
                    return 0;
                }
            }
            for (auto& kv : g_ui.gridHits) {
                if (kv.first >= 0 && kv.first < (int)g_playlist.size() && inRc(kv.second)) {
                    std::string p = g_playlist[kv.first];   // 已在队列, 不重建
                    playPath(p);
                    return 0;
                }
            }
        }

        // --- settings modal: 最高优先级（面板底部可能覆盖控制栏/进度条命中区,
        //     若不先处理, 语言行/模式行的点击会被进度条吃掉） ---
        if (g_ui.settingsOpen) {
            SettingsGeom sg = settingsGeom(g_ui.winW, g_ui.winH);
            bool inside = (mx >= sg.panelX && mx <= sg.panelX + sg.panelW &&
                           my >= sg.panelY && my <= sg.panelY + sg.panelH);
            if (!inside) {
                g_ui.settingsOpen = false;      // 点外 = 关闭(点击不再下传)
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
                bool handled = false;
                for (int i = 0; i < SET_ROW_COUNT && !handled; ++i) {
                    if (my >= sg.rowY[i] - U(5) && my <= sg.rowY[i] + sg.swH + U(5) &&
                        mx >= sg.panelX + U(12)) {
                        *vals[i] = *vals[i] ? 0 : 1;
                        applySetting(keys[i], *vals[i]);
                        const char* tNames[] = { i18n::hwDecode(), i18n::volNorm(), i18n::subAutoLoad(),
                            i18n::thumbCache(), i18n::resume(), i18n::nightMode(),
                            i18n::exclusiveAudio(), i18n::motionInterp(), i18n::hiQScaling() };
                        showToast(tNames[i]);
                        LOG_INFO("MAIN", "setting %s -> %d", keys[i], *vals[i]);
                        handled = true;
                    }
                }
                if (!handled && my >= sg.chipY && my <= sg.chipY + sg.chipH) {
                    for (int i = 0; i < 3; ++i) {
                        int lx = sg.swX - U(180) + i * (sg.chipW + U(6));
                        if (mx >= lx && mx <= lx + sg.chipW) {
                            g_cfg.playMode = i;
                            const char* mNames[] = { i18n::modeSingleT(), i18n::modeLoopT(), i18n::modeShuffleT() };
                            showToast(mNames[i]);
                            LOG_INFO("MAIN", "playmode -> %d", i);
                            handled = true;
                            break;
                        }
                    }
                }
                // 语言切换
                if (!handled && my >= sg.langRowY && my <= sg.langRowY + sg.langSegH) {
                    for (int i = 0; i < 2; ++i) {
                        int lx = sg.langSegX + i * (sg.langSegW / 2);
                        if (mx >= lx && mx <= lx + sg.langSegW / 2) {
                            if (g_cfg.lang != i) {
                                g_cfg.lang = i;
                                LOG_INFO("MAIN", "lang -> %d", i);
                            }
                            handled = true;
                            break;
                        }
                    }
                }
                saveConfig(configPath(), g_cfg);
            }
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            return 0;   // 设置打开期间点击不再下传到进度条/控制栏/视频
        }

        // --- 弹出菜单模态层(倍速/画质/EQ): 打开期间任意点击只作用于弹层 ---
        // 点击菜单项=生效; 点击菜单外=仅关闭。绝不触发视频暂停/进度条/按钮,
        // 消除"弹层点击穿透作用到后面"的问题。
        if (g_ui.speedMenuOpen || g_ui.qualityMenuOpen || g_ui.eqMenuOpen) {
            if (g_ui.speedMenuOpen) {
                Row1Layout L;
                layoutRow1(g_ui.winW, g_ui.winH, false, L);
                int itemH = U(32), menuW = U(132);
                int menuH = SPEED_PRESET_COUNT * itemH + U(12);
                int menuX = L.speedBtn.x;
                int menuY = L.speedBtn.y - menuH - U(6);
                if (menuY < 0) menuY = L.speedBtn.y + L.speedBtn.h + U(6);
                if (menuX + menuW > g_ui.winW - U(8)) menuX = g_ui.winW - menuW - U(8);
                if (mx >= menuX && mx <= menuX + menuW &&
                    my >= menuY && my <= menuY + menuH - U(12)) {
                    int idx = (my - menuY) / itemH;
                    if (idx >= 0 && idx < SPEED_PRESET_COUNT) {
                        g_mpv->setSpeed(SPEED_PRESETS[idx]);
                        char msg[32];
                        std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("倍速", "Speed"), SPEED_PRESETS[idx]);
                        showToast(msg);
                    }
                }
                g_ui.speedMenuOpen = false;
            }
            if (g_ui.qualityMenuOpen) {
                Row1Layout QL;
                layoutRow1(g_ui.winW, g_ui.winH, false, QL);
                int itemH = U(32), menuW = U(140);
                int menuH = QUALITY_PRESET_COUNT * itemH + U(44);
                int menuX = QL.qualityBtn.x;
                int menuY = QL.qualityBtn.y - menuH - U(6);
                if (menuY < 0) menuY = QL.qualityBtn.y + QL.qualityBtn.h + U(6);
                if (menuX + menuW > g_ui.winW - U(8)) menuX = g_ui.winW - menuW - U(8);
                int itemsY = menuY + U(38);
                if (mx >= menuX && mx <= menuX + menuW &&
                    my >= itemsY && my <= itemsY + QUALITY_PRESET_COUNT * itemH) {
                    int idx = (my - itemsY) / itemH;
                    if (idx >= 0 && idx < QUALITY_PRESET_COUNT) {
                        applyQualityPreset(idx);
                        const char* qNames[] = { T("省电", "Power Saving"), T("标准", "Standard"), T("至臻", "Ultimate") };
                        showToast(qNames[idx]);
                    }
                }
                g_ui.qualityMenuOpen = false;
            }
            if (g_ui.eqMenuOpen) {
                int menuW = U(200), itemH = U(36);
                int menuH = U(32) + 6 * itemH + U(40);
                int menuX = g_ui.winW / 2 - menuW / 2;
                int menuY = g_ui.winH / 2 - menuH / 2;
                if (mx >= menuX && mx <= menuX + menuW && my >= menuY && my <= menuY + menuH) {
                    int trackX = menuX + U(60), trackW = U(100);
                    int baseY = menuY + U(32);
                    bool hitSlider = false;
                    for (int i = 0; i < 6; ++i) {
                        int iy = baseY + i * itemH;
                        if (mx >= trackX - U(8) && mx <= trackX + trackW + U(8) &&
                            my >= iy && my <= iy + itemH) {
                            float norm = (float)(mx - trackX) / trackW;
                            if (norm < 0.0f) norm = 0.0f; if (norm > 1.0f) norm = 1.0f;
                            g_mpv->setEQBand(i, norm * 24.0f - 12.0f);
                            g_ui.eqDraggingBand = i;
                            SetCapture(hwnd);
                            hitSlider = true;
                            break;
                        }
                    }
                    if (!hitSlider) {
                        int resetY = baseY + 6 * itemH + U(4);
                        if (mx >= menuX + menuW / 2 - U(30) && mx <= menuX + menuW / 2 + U(30) &&
                            my >= resetY && my <= resetY + U(26)) {
                            for (int i = 0; i < 6; ++i) g_mpv->setEQBand(i, 0.0f);
                            showToast(i18n::eqReset());
                        }
                        // 菜单内其他位置: 保持打开
                        g_ui.visible = true;
                        g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
                        g_dirty.store(true);
                        return 0;
                    }
                } else {
                    g_ui.eqMenuOpen = false;   // 菜单外: 关闭
                }
            }
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            g_dirty.store(true);
            return 0;   // 弹层打开期间点击一律消费
        }

        // --- seekbar（垂直容差收紧: 下探过深会吃掉用户点按钮/视频的点击）---
        if (g_mpv && my >= barTop - U(8) && my <= barTop + U(12) &&
            mx >= sbLeftX() && mx <= sbRightX() && g_mpv->duration() > 0) {
            g_ui.seekingDrag = true;
            double ratio = (double)(mx - sbLeftX()) / sbWidth();
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_ui.seekTarget = g_mpv->duration() * ratio;
            SetCapture(hwnd);
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            LOG_DBG("SEEK", "seekbar press target=%.2f ratio=%.3f (no pause fallthrough)",
                    g_ui.seekTarget, ratio);
            return 0;  // seekbar 点击不穿透到视频区
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
            if (volOpen && mx >= L.volSliderX - U(6) &&
                mx <= L.volSliderX + U(76) &&
                my >= L.cy - U(12) && my <= L.cy + U(12)) {
                g_ui.volumeDragging = true;
                float ratio = (float)(mx - L.volSliderX) / U(70);
                if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
                g_mpv->setVolume(ratio);
                SetCapture(hwnd);
            }
            else if (inRc(L.prev)) {
                int idx = playlistIndexOf(g_mpv->path());
                if (idx > 0) { playIndex(idx - 1); showToast(T("上一曲", "Previous")); }
                else showToast(i18n::noPrev());
            }
            else if (inRc(L.play)) {
                g_mpv->togglePause();
            }
            else if (inRc(L.next)) {
                int idx = playlistIndexOf(g_mpv->path());
                int n = (int)g_playlist.size();
                if (idx >= 0 && idx + 1 < n) { playIndex(idx + 1); showToast(T("下一曲", "Next")); }
                else showToast(i18n::noNext());
            }
            else if (inRc(L.subBtn)) {
                bool vis = !g_mpv->subVisible();
                g_mpv->setSubVisibility(vis);
                std::string trk = g_mpv->currentSubTrack();
                char msg[96];
                std::snprintf(msg, sizeof(msg), vis ? "%s [%s]" : "%s",
                              vis ? i18n::subtitlesOn() : i18n::subtitlesOff(),
                              trk.c_str());
                showToast(msg);
            }
            else if (inRc(L.speedBtn)) {
                g_ui.speedMenuOpen = !g_ui.speedMenuOpen;
            }
            else if (inRc(L.qualityBtn)) {
                g_ui.qualityMenuOpen = !g_ui.qualityMenuOpen;
            }
            else if (mx >= L.volIconCx - U(17) && mx <= L.volIconCx + U(17) &&
                     my >= L.cy - U(17) && my <= L.cy + U(17)) {
                g_mpv->toggleMute();
                showToast(g_mpv->muted() ? i18n::muted() : i18n::unmuted());
                LOG_INFO("MAIN", "mute toggled -> %d", g_mpv->muted() ? 1 : 0);
            }
            else if (inRc(L.setBtn)) {
                g_ui.settingsOpen = !g_ui.settingsOpen;
                LOG_INFO("MAIN", "setBtn click (%d,%d) rect[%d,%d %dx%d] -> open=%d",
                         mx, my, L.setBtn.x, L.setBtn.y, L.setBtn.w, L.setBtn.h,
                         g_ui.settingsOpen ? 1 : 0);
            }
            else if (inRc(L.fullBtn)) {
                toggleFullscreen(hwnd);
            }
            else {
                goto videoAreaClick;
            }
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            return 0;
        }
    videoAreaClick:;
        // (倍速/画质/EQ 菜单命中已由模态层统一处理, 此处不再可达)
        // --- 音量图标点击：切换静音（滑条由 hover 展开） ---
        if (g_mpv && mx >= g_ui.winW - U(68) && mx <= g_ui.winW - U(40) &&
                 my >= barTop + U(36) && my <= barTop + U(64)) {
            g_mpv->toggleMute();
            showToast(g_mpv->muted() ? i18n::muted() : i18n::unmuted());
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
                mx >= g_ui.sbTrackX - U(5) && mx <= g_ui.sbTrackX + g_ui.sbTrackW + U(5) &&
                my >= g_ui.sbTrackY && my <= g_ui.sbTrackY + g_ui.sbTrackH) {
                SetCapture(hwnd);
                if (my < g_ui.sbBarY || my > g_ui.sbBarY + g_ui.sbBarH) {
                    // 轨道跳页: bar 中心对齐点击处
                    int contentH = (int)g_playlist.size() * U(72);
                    int viewH = g_ui.sbTrackH;
                    g_ui.playlistScroll =
                        (my - g_ui.sbGrabOff - g_ui.sbTrackY - g_ui.sbBarH / 2) *
                        (contentH - viewH) / (viewH - g_ui.sbBarH);
                    clampPlaylistScroll();
                }
                g_ui.sbDragging = true;
                g_ui.sbGrabOff = my - g_ui.sbBarY;
            } else {
                int itemH = U(72);
                int rel = my - U(45) + g_ui.playlistScroll;   // panelY=0 与渲染一致
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
        // 进度条区域双击 -> 跳转到点击位置
        if (g_mpv && g_mpv->duration() > 0 &&
            my >= barTop - U(8) && my <= barTop + U(12) &&
            mx >= sbLeftX() && mx <= sbRightX()) {
            double ratio = (double)(mx - sbLeftX()) / sbWidth();
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_mpv->seek(g_mpv->duration() * ratio);
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
        }
        // 视频区双击 -> 全屏切换（取消待定暂停）
        else if (g_mpv && my > curTopH() && my < barTop - U(6)) {
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
            if (g_mpv) {
                LOG_DBG("SEEK", "seekbar release -> seek(%.2f)", g_ui.seekTarget);
                g_mpv->seek(g_ui.seekTarget);
            }
        }
        if (g_ui.volumeDragging) {
            g_ui.volumeDragging = false;
        }
        if (g_ui.eqDraggingBand >= 0) {
            g_ui.eqDraggingBand = -1;
        }
        if (g_ui.sbDragging) {
            g_ui.sbDragging = false;
        }
        // 列表拖拽落位 / 单击播放
        if (g_ui.plDragFrom >= 0) {
            if (g_ui.plDragging) {
                int itemH = U(72);
                int topY = U(45);   // panelY=0 与渲染一致
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
                    showToast(i18n::playlistReordered());
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
            int panelH = g_ui.winH - curTopH();
            int contentH = (int)g_playlist.size() * U(72);
            int viewH = panelH - U(55);
            int step = U(72) * 2;
            g_ui.playlistScroll -= (d > 0 ? step : -step);
            if (g_ui.playlistScroll < 0) g_ui.playlistScroll = 0;
            if (contentH > viewH && g_ui.playlistScroll > contentH - viewH)
                g_ui.playlistScroll = contentH - viewH;
            else if (contentH <= viewH) g_ui.playlistScroll = 0;
        }
        else if (g_mpv) {
            g_mpv->setVolume(g_mpv->volume() + (d > 0 ? 0.05f : -0.05f));
            char msg[24];
            std::snprintf(msg, sizeof(msg), "%s %d%%", T("音量", "Volume"), (int)(g_mpv->volume() * 100 + 0.5f));
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
// 逐像素 alpha 合成(UpdateLayeredWindow): 渲染结果 ReadPixels 后预乘 alpha,
// 经 ULW_ALPHA 上屏。支持真半透明(玻璃渐变/压暗遮罩/模态背景), 无 colorkey
// 二值透明的抖动伪装。点击穿透仍由 WS_EX_TRANSPARENT 保证。
static const Uint8 TRANSPARENT_R = 0;
static const Uint8 TRANSPARENT_G = 0;
static const Uint8 TRANSPARENT_B = 0;

static bool createOverlay(HWND parent, int w, int h) {
    // 顶层无边框窗口（本系统不支持 WS_EX_LAYERED 子窗口，实测 err=87）
    // 通过 OWNER 关联 + TOOLWINDOW 融入主窗口：不进任务栏/Alt+Tab，随主窗口关闭
    g_sdlWin = SDL_CreateWindow("Phantom Video",
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
    // 不再使用 LWA_COLORKEY: 改由 UpdateLayeredWindow 提供逐像素 alpha

    // 设为 parent 的 Owned 窗口：置顶于父、父最小化时联动、无独立任务栏项
    SetWindowLongPtrW(ov, GWLP_HWNDPARENT, (LONG_PTR)parent);
    SetWindowPos(ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 必须用软件渲染器: D3D11 交换链后备缓冲不保留 alpha(读回恒为 255),
    // 会让 ULW 整层变不透明黑板。软件渲染器后备缓冲是真 alpha Surface。
    // UI 按需重绘, 软件渲染性能足够。
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    g_sdlRdr = SDL_CreateRenderer(g_sdlWin, -1, 0);
    if (!g_sdlRdr) {
        LOG_ERROR("MAIN", "SDL_CreateRenderer: %s", SDL_GetError());
        return false;
    }
    SDL_RendererInfo rinfo{};
    if (SDL_GetRendererInfo(g_sdlRdr, &rinfo) == 0)
        LOG_INFO("MAIN", "overlay renderer: %s", rinfo.name);
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

// M36: 圆角矩形填充 (settings 面板组合法通用化) — B/C/D 共用
static void roundedRectFill(SDL_Renderer* r, int x, int y, int w, int h, int rad,
                            Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (w <= 0 || h <= 0) return;
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_Rect body = {x + rad, y, w - rad * 2, h};
    SDL_RenderFillRect(r, &body);
    SDL_Rect top = {x, y + rad, w, h - rad * 2};
    SDL_RenderFillRect(r, &top);
    fillCircle(r, x + rad, y + rad, rad, cr, cg, cb, ca);
    fillCircle(r, x + w - rad - 1, y + rad, rad, cr, cg, cb, ca);
    fillCircle(r, x + rad, y + h - rad - 1, rad, cr, cg, cb, ca);
    fillCircle(r, x + w - rad - 1, y + h - rad - 1, rad, cr, cg, cb, ca);
}

// M36: 圆角矩形描边 (当前项高亮环)
static void roundedRectStroke(SDL_Renderer* r, int x, int y, int w, int h, int rad,
                              Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (w <= 0 || h <= 0) return;
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_RenderDrawLine(r, x + rad, y, x + w - rad - 1, y);
    SDL_RenderDrawLine(r, x + rad, y + h - 1, x + w - rad - 1, y + h - 1);
    SDL_RenderDrawLine(r, x, y + rad, x, y + h - rad - 1);
    SDL_RenderDrawLine(r, x + w - 1, y + rad, x + w - 1, y + h - rad - 1);
    // 四角圆弧 (近似: 内外两层像素)
    for (int i = 0; i <= rad; ++i) {
        int d = (int)(rad - std::sqrt((float)rad * rad - (float)(rad - i) * (rad - i)) + 0.5f);
        SDL_RenderDrawPoint(r, x + i, y + d);
        SDL_RenderDrawPoint(r, x + i, y + h - 1 - d);
        SDL_RenderDrawPoint(r, x + w - 1 - i, y + d);
        SDL_RenderDrawPoint(r, x + w - 1 - i, y + h - 1 - d);
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
static SDL_Texture* g_gradTex[3]   = { nullptr, nullptr, nullptr };
static GradKey       g_gradKey[3]  = {};

static void destroyGradCache() {
    for (auto& t : g_gradTex)
        if (t) { SDL_DestroyTexture(t); t = nullptr; }
}

// 均匀抖动压暗: Bayer 抖动混合"透明键黑"与不透明深色, 模拟半透明遮罩
// (colorkey 架构下无法真半透明, 抖动是唯一正确方案)
static void drawDitherDim(SDL_Renderer* r, int x, int y, int w, int h,
                          Uint8 cr, Uint8 cg, Uint8 cb, Uint8 alpha) {
    static const int bayer[4][4] = {
        {  0, 136,  34, 170},
        {204,  68, 238, 102},
        { 51, 187,  17, 153},
        {255, 119, 221,  85}
    };
    if (w <= 0 || h <= 0) return;
    GradKey key{ w, h, cr, cg, cb, alpha, alpha };

    // slot 2 专用缓存
    static SDL_Texture* dimTex = nullptr;
    static GradKey      dimKey = {};
    if (!dimTex || !(dimKey == key)) {
        if (dimTex) { SDL_DestroyTexture(dimTex); dimTex = nullptr; }
        SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!tex) return;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        Uint32* pixels = nullptr; int pitch = 0;
        if (SDL_LockTexture(tex, nullptr, (void**)&pixels, &pitch) == 0) {
            Uint32 rgb = ((Uint32)cr << 16) | ((Uint32)cg << 8) | cb;
            for (int dy = 0; dy < h; ++dy) {
                int by = dy % 4;
                Uint32* row = (Uint32*)((Uint8*)pixels + dy * pitch);
                for (int dx = 0; dx < w; ++dx) {
                    row[dx] = (alpha > bayer[by][dx % 4])
                            ? (0xFF000000u | rgb)
                            : 0x00000000u;
                }
            }
            SDL_UnlockTexture(tex);
        }
        dimTex = tex;
        dimKey = key;
    }
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(r, dimTex, nullptr, &dst);
}

static void drawGradientBar(SDL_Renderer* r, int slot, int x, int y, int w, int h,
                             Uint8 cr, Uint8 cg, Uint8 cb, Uint8 aTop, Uint8 aBot) {
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
                int a = aTop + ((int)(aBot - aTop)) * dy / h;   // 线性 alpha, 平滑无抖动
                Uint32* row = (Uint32*)((Uint8*)pixels + dy * pitch);
                for (int dx = 0; dx < w; ++dx) {
                    row[dx] = ((Uint32)a << 24) | rgb;
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

// 逐像素 alpha 上屏: UI 画入离屏 ARGB 纹理(真 alpha) → ReadPixels →
// 预乘 alpha → UpdateLayeredWindow。
// 注意: 窗口后备缓冲(RGBX)不保留 alpha, 必须经离屏纹理中转。
struct UlwCtx {
    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
};
static UlwCtx g_ulw;
static SDL_Texture* g_ovTex = nullptr;   // UI 离屏画布(真 alpha)
static int g_ovTexW = 0, g_ovTexH = 0;

// M36: 本帧圆角遮罩列表 (渲染时填充, overlayPresent 消费后清空)
struct RoundMask { int x, y, w, h, r; };
static std::vector<RoundMask> g_roundMasks;

static bool ovTexEnsure(int w, int h) {
    if (g_ovTex && g_ovTexW == w && g_ovTexH == h) return true;
    if (g_ovTex) { SDL_DestroyTexture(g_ovTex); g_ovTex = nullptr; }
    g_ovTex = SDL_CreateTexture(g_sdlRdr, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_TARGET, w, h);
    if (!g_ovTex) {
        LOG_ERROR("MAIN", "ovTex create: %s", SDL_GetError());
        return false;
    }
    g_ovTexW = w; g_ovTexH = h;
    return true;
}

static void ulwDestroy() {
    if (g_ulw.dib) { DeleteObject(g_ulw.dib); g_ulw.dib = nullptr; }
    if (g_ulw.memDC) { DeleteDC(g_ulw.memDC); g_ulw.memDC = nullptr; }
    g_ulw.bits = nullptr; g_ulw.w = g_ulw.h = 0;
}

static bool ulwResize(int w, int h) {
    ulwDestroy();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_ulw.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &g_ulw.bits, nullptr, 0);
    if (!g_ulw.dib) { LOG_ERROR("MAIN", "CreateDIBSection failed"); return false; }
    g_ulw.memDC = CreateCompatibleDC(nullptr);
    if (!g_ulw.memDC) { LOG_ERROR("MAIN", "CreateCompatibleDC failed"); return false; }
    SelectObject(g_ulw.memDC, g_ulw.dib);
    g_ulw.w = w; g_ulw.h = h;
    return true;
}

static void overlayPresent() {
    if (!g_sdlRdr || !g_sdlWin || !g_overlayHwnd) return;
    int w = g_ui.totalW > 0 ? g_ui.totalW : g_ui.winW;
    int h = g_ui.winH > 0 ? g_ui.winH : 540;
    if (w <= 0 || h <= 0 || !g_ovTex) return;

    if (g_ulw.w != w || g_ulw.h != h) {
        if (!ulwResize(w, h)) return;
    }

    // 1. 从离屏 ARGB 纹理回读 (真 alpha)
    if (SDL_SetRenderTarget(g_sdlRdr, g_ovTex) != 0) return;
    static std::vector<Uint32> px;
    px.resize((size_t)w * h);
    SDL_Rect rr = {0, 0, w, h};
    if (SDL_RenderReadPixels(g_sdlRdr, &rr, SDL_PIXELFORMAT_ARGB8888,
                             px.data(), w * 4) != 0) {
        LOG_ERROR("MAIN", "RenderReadPixels: %s", SDL_GetError());
        SDL_SetRenderTarget(g_sdlRdr, nullptr);
        return;
    }
    SDL_SetRenderTarget(g_sdlRdr, nullptr);

    // 1.5 M36: 圆角缩略图遮罩 — 角外像素置全透明(软渲染无法裁剪纹理)
    for (const auto& m : g_roundMasks) {
        int r = std::min(m.r, std::min(m.w, m.h) / 2);
        if (r <= 0) continue;
        auto zeroIfOut = [&](int x, int y, int cx, int cy) {
            if (x < 0 || y < 0 || x >= w || y >= h) return;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r)
                px[(size_t)y * w + x] = 0;
        };
        for (int yy = 0; yy < r; ++yy) {
            int rowT = m.y + yy, rowB = m.y + m.h - 1 - yy;
            if (rowT < 0 || rowT >= h || rowB < 0 || rowB >= h) continue;
            int x0 = std::max(m.x, 0), x1 = std::min(m.x + m.w - 1, w - 1);
            for (int xx = x0; xx <= std::min(x0 + r, x1); ++xx) {
                zeroIfOut(xx, rowT, m.x + r, m.y + r);
                zeroIfOut(xx, rowB, m.x + r, m.y + m.h - 1 - r);
            }
            for (int xx = std::max(m.x + m.w - 1 - r, x0); xx <= x1; ++xx) {
                zeroIfOut(xx, rowT, m.x + m.w - 1 - r, m.y + r);
                zeroIfOut(xx, rowB, m.x + m.w - 1 - r, m.y + m.h - 1 - r);
            }
        }
    }
    g_roundMasks.clear();

    // 2. 预乘 alpha (ULW 要求 premultiplied BGRA)
    const Uint32* src = px.data();
    Uint32* dst = (Uint32*)g_ulw.bits;
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        Uint32 p = src[i];
        Uint32 a = p >> 24;
        if (a == 0)        dst[i] = 0;
        else if (a == 255) dst[i] = p | 0xFF000000u;
        else {
            Uint32 r = ((p >> 16) & 255) * a / 255;
            Uint32 gg = ((p >> 8) & 255) * a / 255;
            Uint32 b = (p & 255) * a / 255;
            dst[i] = (a << 24) | (r << 16) | (gg << 8) | b;
        }
    }

    // 诊断: 首帧采样视频区中心 alpha(应为 0=透明; 255=alpha 通路失效)
    {
        static bool sampled = false;
        if (!sampled) {
            sampled = true;
            Uint32 s1 = src[(size_t)(h / 2) * w + w / 2];
            Uint32 s2 = src[(size_t)10 * w + 10];
            LOG_INFO("MAIN", "alpha sample center=%08X corner=%08X (expect A=00)",
                     s1, s2);
        }
    }

    // 3. ULW 上屏 (位置跟随 parent 客户区原点)
    POINT pt = {0, 0};
    ClientToScreen(g_parentHwnd, &pt);
    POINT srcPt = {0, 0};
    SIZE sz = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC scr = GetDC(nullptr);
    BOOL ok = UpdateLayeredWindow(g_overlayHwnd, scr, &pt, &sz,
                                  g_ulw.memDC, &srcPt, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    if (!ok) {
        static bool warned = false;
        if (!warned) { LOG_ERROR("MAIN", "UpdateLayeredWindow failed err=%lu", GetLastError()); warned = true; }
    }
}

// M36: 像素宽度约束的单行省略 (UTF-8 安全, 逐码点回退)
static std::string ellipsize(const std::string& s, int pt, int maxW) {
    if (s.empty() || g_text.measureText(s, T(pt)) <= maxW) return s;
    std::string out = s;
    while (out.size() > 1) {
        size_t n = out.size();
        while (n > 0 && (out[n - 1] & 0xC0) == 0x80) --n;   // 跳过续字节
        if (n > 0) --n;                                      // 去掉一个前导字节
        out.resize(n);
        if (g_text.measureText(out + "...", T(pt)) <= maxW) break;
    }
    return out + "...";
}

// M36: 缩略图 cover 绘制 + 注册圆角遮罩; 未就绪时画占位面板
static void drawThumbCover(const std::string& path, SDL_Rect rc, int rad) {
    auto it = g_thumbTex.find(path);
    if (it == g_thumbTex.end() || !it->second) {
        SDL_SetRenderDrawColor(g_sdlRdr, ui::SURFACE1_R, ui::SURFACE1_G,
                               ui::SURFACE1_B, 255);
        SDL_RenderFillRect(g_sdlRdr, &rc);
        svgicon::draw(g_sdlRdr, "play", rc.x + rc.w / 2, rc.y + rc.h / 2, U(20),
                      255, 255, 255, 255);
    } else {
        int tw = 0, th = 0;
        SDL_QueryTexture(it->second, nullptr, nullptr, &tw, &th);
        SDL_Rect src = {0, 0, tw, th};
        double dstA = (double)rc.w / std::max(1, rc.h);
        double srcA = (double)tw / std::max(1, th);
        if (srcA > dstA + 0.01) {          // 源更宽 → 裁左右
            int cw = (int)(th * dstA);
            src.x = (tw - cw) / 2; src.w = cw;
        } else if (srcA < dstA - 0.01) {   // 源更窄 → 裁上下
            int ch = (int)(tw / dstA);
            src.y = (th - ch) / 2; src.h = ch;
        }
        SDL_RenderCopy(g_sdlRdr, it->second, &src, &rc);
    }
    if (rad > 0) g_roundMasks.push_back({rc.x, rc.y, rc.w, rc.h, rad});
}

static void renderOverlay() {
    if (!g_sdlRdr || !g_sdlWin) return;

    uploadThumbs(g_sdlRdr);   // 惰性上传就绪的缩略图纹理

    // 所有 UI 画入离屏 ARGB 纹理(真 alpha); 后备缓冲不使用
    // 直接用 g_ui.winW/winH (WM_SIZE 已更新), 不走 SDL_GetWindowSize 避免延迟
    int ow = g_ui.totalW > 0 ? g_ui.totalW : g_ui.winW;
    int oh = g_ui.winH > 0 ? g_ui.winH : 540;
    if (ow <= 0 || oh <= 0) return;
    if (!ovTexEnsure(ow, oh)) return;
    SDL_SetRenderTarget(g_sdlRdr, g_ovTex);

    SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 0);   // 全透明底(per-pixel alpha)
    SDL_RenderClear(g_sdlRdr);

    int w = g_ui.winW, h = g_ui.winH, totalW = g_ui.totalW;

    if (!g_mpv || !g_mpv->hasMedia()) {
        // --- M36 welcome page: Apple 居中 Hero + YouTube 缩略图卡片 ---
        int w = g_ui.winW, h = g_ui.winH, totalW = g_ui.totalW;

        // 入场淡入 (离开欢迎页时归零, 由下方播放分支负责)
        g_ui.introAlpha = std::min(1.0f, g_ui.introAlpha + 0.055f);
        Uint8 fa8 = (Uint8)(255 * g_ui.introAlpha);
        auto A8 = [&](Uint8 base) { return (Uint8)(base * g_ui.introAlpha); };

        g_ui.continueHits.clear();
        g_ui.gridHits.clear();
        std::vector<std::string> wantThumbs;

        // 暗色底: 无媒体时 mpv 子窗口是白的, 欢迎页必须自己铺满遮住
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, ui::SURFACE0_R, ui::SURFACE0_G,
                               ui::SURFACE0_B, A8(255));
        SDL_Rect fullBg = {0, 0, totalW, h};
        SDL_RenderFillRect(g_sdlRdr, &fullBg);

        // ---- topbar (与播放态一致的全套图标) ----
        drawGradientBar(g_sdlRdr, 0, 0, 0, totalW, curTopH(), 11, 11, 11,
                        (Uint8)(ui::TOPBAR_A0 * g_ui.introAlpha), 0);
        {
            std::string title = i18n::appName();
            g_text.drawText(U(20), U(14), title, T(14), 255, 255, 255, A8(255));
        }
        {
            int iconY = curTopH() / 2;
            int iconSz = U(34);
            int rx = totalW - U(20);
            const char* ids[6] = {"close", "maximize", "minimize",
                                  "list", "pip", "camera"};
            for (int i = 0; i < 6; ++i) {
                if (g_ui.topbarHover == i) {
                    SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
                    if (i == 0) SDL_SetRenderDrawColor(g_sdlRdr, 232, 17, 35, A8(240));
                    else        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, A8(50));
                    int hr = iconSz / 2 + U(2);
                    SDL_Rect hrc = {rx - hr, iconY - hr, hr * 2, hr * 2};
                    SDL_RenderFillRect(g_sdlRdr, &hrc);
                }
                Uint8 ic = (g_ui.topbarHover == i) ? 255 : 235;
                svgicon::draw(g_sdlRdr, ids[i], rx, iconY, U(28), 255, 255, 255, A8(255));
                rx -= iconSz;
            }
        }

        // ---- Hero: 渐变圆角应用图标 (矮窗口自动紧凑) ----
        const int margin = U(48);
        bool compact = (h < U(660));
        int iconSz = compact ? U(60) : U(88);
        int ix = (w - iconSz) / 2;
        int iy = curTopH() + (compact ? U(20) : U(36));
        roundedRectFill(g_sdlRdr, ix, iy, iconSz, iconSz, U(20),
                        ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, A8(255));
        // 内光晕: 左上光源, 双层同心圆 (全部在图标内部)
        fillCircle(g_sdlRdr, ix + U(28), iy + U(28), U(18),
                   ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, A8(38));
        fillCircle(g_sdlRdr, ix + U(28), iy + U(28), U(10),
                   ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, A8(52));
        svgicon::draw(g_sdlRdr, "play", ix + iconSz / 2, iy + iconSz / 2, U(30),
                      255, 255, 255, A8(255));

        // ---- 产品名 + 标语 (间距按字号实际高度, 防重叠) ----
        int namePt = T(ui::T_DISPLAY);
        int nameHpx = (int)(namePt * g_dpi * 1.4f);   // GDI 行高近似
        int nameY = iy + iconSz + (compact ? U(10) : U(18));
        {
            std::string nm = i18n::appName();
            int nw = g_text.measureText(nm, namePt);
            g_text.drawText((w - nw) / 2, nameY, nm, namePt,
                            255, 255, 255, A8(255));
        }
        int tagY = nameY + nameHpx + U(4);
        {
            std::string tg = i18n::tagline();
            int tgw = g_text.measureText(tg, T(ui::T_BODY));
            g_text.drawText((w - tgw) / 2, tagY, tg, T(ui::T_BODY),
                            170, 170, 178, A8(255));
        }

        // ---- 双药丸按钮 (MD3: 填充主操作 + 描边次操作) ----
        int btnH = compact ? U(40) : U(46);
        int btnY = tagY + U(18) + (compact ? U(6) : U(18));
        {
            std::string l1 = i18n::openFile(), l2 = i18n::openFolder();
            int w1 = g_text.measureText(l1, T(ui::T_BODY)) + U(48);
            int w2 = g_text.measureText(l2, T(ui::T_BODY)) + U(48);
            int gap = U(14);
            int bx1 = (w - (w1 + gap + w2)) / 2;
            int bx2 = bx1 + w1 + gap;
            g_ui.heroFileBtn   = {bx1, btnY, w1, btnH};
            g_ui.heroFolderBtn = {bx2, btnY, w2, btnH};

            bool hov1 = (g_ui.mouseX >= bx1 && g_ui.mouseX <= bx1 + w1 &&
                         g_ui.mouseY >= btnY && g_ui.mouseY <= btnY + btnH);
            bool hov2 = (g_ui.mouseX >= bx2 && g_ui.mouseX <= bx2 + w2 &&
                         g_ui.mouseY >= btnY && g_ui.mouseY <= btnY + btnH);

            // 填充式主按钮: 红, 悬停提亮为渐变亮端
            roundedRectFill(g_sdlRdr, bx1, btnY, w1, btnH, btnH / 2,
                            hov1 ? ui::ACCENT2_R : ui::ACCENT_R_,
                            hov1 ? ui::ACCENT2_G : ui::ACCENT_G_,
                            hov1 ? ui::ACCENT2_B : ui::ACCENT_B_, A8(255));
            int t1w = g_text.measureText(l1, T(ui::T_BODY));
            g_text.drawText(bx1 + (w1 - t1w) / 2, btnY + U(12), l1,
                            T(ui::T_BODY), 255, 255, 255, A8(255));

            // 描边式次按钮: 白描边, 悬停浮起背景
            if (hov2) {
                SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, A8(22));
                SDL_Rect fb = {bx2, btnY, w2, btnH};
                SDL_RenderFillRect(g_sdlRdr, &fb);
            }
            roundedRectStroke(g_sdlRdr, bx2, btnY, w2, btnH, btnH / 2,
                              255, 255, 255, A8(hov2 ? 90 : 60));
            int t2w = g_text.measureText(l2, T(ui::T_BODY));
            g_text.drawText(bx2 + (w2 - t2w) / 2, btnY + U(12), l2,
                            T(ui::T_BODY), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5, A8(255));
        }
        // 拖拽提示 (紧贴按钮下方)
        int hintY = btnY + btnH + U(8);
        {
            std::string dh = i18n::dropAnywhere();
            int dw = g_text.measureText(dh, T(ui::T_CAPTION));
            g_text.drawText((w - dw) / 2, hintY, dh,
                            T(ui::T_CAPTION), 140, 140, 148, A8(255));
        }

        // ---- 继续观看行 (YouTube 缩略图卡片) ----
        int contentY = hintY + U(22) + (compact ? U(24) : U(40));
        struct CWItem { std::string path; double pos, dur; long long ts; };
        std::vector<CWItem> cw;
        for (const auto& kv : g_cfg.history) {
            const HistoryEntry& e = kv.second;
            if (e.pos > 1.0 && (e.dur <= 0 || e.pos < e.dur * 0.95))
                cw.push_back({kv.first, e.pos, e.dur, e.lastPlayed});
        }
        if (!cw.empty()) {
            std::sort(cw.begin(), cw.end(),
                      [](const CWItem& a, const CWItem& b) { return a.ts > b.ts; });
            int cardW = compact ? U(150) : U(180);
            int gap = U(14);
            int maxCards = std::max(1, (totalW - margin * 2 + gap) / (cardW + gap));
            int nShow = std::min((int)cw.size(), maxCards);
            int rowW = nShow * cardW + (nShow - 1) * gap;
            int gx = ((totalW > w ? totalW : w) - rowW) / 2;   // 列表区打开时居中于全客户区

            {
                std::string hd = i18n::continueWatching();
                g_text.drawText(std::max(margin, gx), contentY - U(30), hd,
                                T(ui::T_HEADLINE), 255, 255, 255, A8(255));
            }
            for (int i = 0; i < nShow; ++i) {
                const CWItem& it = cw[i];
                int cx = gx + i * (cardW + gap);
                int thumbH = cardW * 9 / 16;
                SDL_Rect trc = {cx, contentY, cardW, thumbH};
                drawThumbCover(it.path, trc, U(8));

                // 进度条 (红)
                if (it.dur > 0) {
                    float frac = (float)std::min(1.0, it.pos / it.dur);
                    int pbH = U(3);
                    SDL_SetRenderDrawColor(g_sdlRdr, 40, 40, 44, A8(220));
                    SDL_Rect pbg = {cx, contentY + thumbH - pbH, cardW, pbH};
                    SDL_RenderFillRect(g_sdlRdr, &pbg);
                    int pw = (int)(cardW * frac);
                    if (pw > 0) {
                        SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_,
                                               ui::ACCENT_B_, A8(255));
                        SDL_Rect pfg = {cx, contentY + thumbH - pbH, pw, pbH};
                        SDL_RenderFillRect(g_sdlRdr, &pfg);
                    }
                }
                // 标题 (单行像素级省略)
                std::string fn = fileNameOf(it.path);
                fn = ellipsize(fn, ui::T_BODY, cardW);
                g_text.drawText(cx, contentY + thumbH + U(8), fn,
                                T(ui::T_BODY), 235, 235, 240, A8(255));
                // 副标题: 看到 xx% · 时间点
                char tb1[16];
                formatTime(tb1, sizeof(tb1), it.pos);
                std::string sub2 = std::string(T("看到 ", "Watched ")) +
                                   (it.dur > 0 ? std::to_string((int)(it.pos / it.dur * 100 + 0.5)) + "% · " : "") +
                                   tb1;
                g_text.drawText(cx, contentY + thumbH + U(30), sub2,
                                T(ui::T_CAPTION), 150, 150, 158, A8(255));

                g_ui.continueHits.push_back({it.path, {cx, contentY - U(4), cardW, thumbH + U(52)}});
                wantThumbs.push_back(it.path);
            }
            contentY += cardW * 9 / 16 + (compact ? U(58) : U(76));
        }

        // ---- 文件夹队列网格 (缩略图卡) ----
        if (!g_playlist.empty()) {
            {
                std::string hd = i18n::playlist();
                g_text.drawText(std::max(margin, (w - 0) / 2), contentY - U(26), hd,
                                T(ui::T_HEADLINE), 255, 255, 255, A8(255));
            }
            int cardW = U(180);
            int gap = U(14);
            int cols = std::max(1, std::min((int)g_playlist.size(),
                               (w - margin * 2 + gap) / (cardW + gap)));
            int gridW = cols * cardW + (cols - 1) * gap;
            int gx = (w - gridW) / 2;
            int thumbH = cardW * 9 / 16;
            int rowsMax = std::max(1, (h - contentY - U(70)) / (thumbH + U(58)));
            int shown = std::min((int)g_playlist.size(), cols * rowsMax);
            std::string curPath = g_mpv ? g_mpv->path() : "";
            for (int i = 0; i < shown; ++i) {
                int col = i % cols, row = i / cols;
                int cx = gx + col * (cardW + gap);
                int cy = contentY + row * (thumbH + U(58));
                bool isCur = (g_playlist[i] == curPath);
                bool hov = (g_ui.mouseX >= cx && g_ui.mouseX <= cx + cardW &&
                            g_ui.mouseY >= cy && g_ui.mouseY <= cy + thumbH + U(50));

                drawThumbCover(g_playlist[i], {cx, cy, cardW, thumbH}, U(8));
                // 当前项红环 / 悬停白环
                if (isCur)
                    roundedRectStroke(g_sdlRdr, cx - U(2), cy - U(2), cardW + U(4),
                                      thumbH + U(4), U(9),
                                      ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, A8(255));
                else if (hov)
                    roundedRectStroke(g_sdlRdr, cx - U(2), cy - U(2), cardW + U(4),
                                      thumbH + U(4), U(9), 255, 255, 255, A8(80));

                std::string fn = ellipsize(fileNameOf(g_playlist[i]), ui::T_CAPTION, cardW);
                g_text.drawText(cx, cy + thumbH + U(8), fn,
                                T(ui::T_CAPTION), isCur ? 255 : 225, isCur ? 255 : 225,
                                isCur ? 255 : 230, A8(255));
                // 时长/进度副文字
                auto hit = g_cfg.history.find(g_playlist[i]);
                double hp = (hit != g_cfg.history.end()) ? hit->second.pos : 0;
                char sub[40] = "";
                if (hp > 0) {
                    char tb[16];
                    formatTime(tb, sizeof(tb), hp);
                    std::snprintf(sub, sizeof(sub), "@ %s", tb);
                }
                if (sub[0])
                    g_text.drawText(cx, cy + thumbH + U(26), sub,
                                    T(ui::T_CAPTION), 140, 140, 148, A8(255));

                g_ui.gridHits.push_back({i, {cx, cy, cardW, thumbH + U(50)}});
                wantThumbs.push_back(g_playlist[i]);
            }
        }

        // 提交欢迎页可见缩略图请求 (与播放列表面板合并语义)
        if (!wantThumbs.empty()) {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            for (auto& p : wantThumbs)
                if (!g_thumbRgb.count(p) && !g_thumbTex.count(p) &&
                    std::find(g_thumbWant.begin(), g_thumbWant.end(), p) == g_thumbWant.end())
                    g_thumbWant.push_back(p);
        }

        // ---- 底部: 键盘提示(居中) + 版本(右下) ----
        {
            std::string hint = T("空格 播放/暂停 · ←→ 快进退 · F 全屏 · M 静音",
                                 "Space Play/Pause · Arrows Seek · F Fullscreen · M Mute");
            int hw = g_text.measureText(hint, T(ui::T_CAPTION));
            g_text.drawText((totalW - hw) / 2, h - U(30), hint,
                            T(ui::T_CAPTION), ui::HINT_TEXT, ui::HINT_TEXT, ui::HINT_TEXT + 6, A8(200));
            std::string ver = std::string("v") + PHANTOM_VERSION;
            int vw = g_text.measureText(ver, T(ui::T_CAPTION));
            g_text.drawText(totalW - vw - U(16), h - U(30), ver,
                            T(ui::T_CAPTION), 110, 110, 116, A8(160));
        }

        overlayPresent();
        return;
    }

    g_ui.introAlpha = 0.0f;   // 离开欢迎页, 下次进入重新淡入
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
    int topOff = -(int)((1.0f - fa) * curTopH() + 0.5f);

    // --- topbar (gradient: glass 半透明效果, 视频隐约可见) ---
    {
        drawGradientBar(g_sdlRdr, 0, 0, topOff, w, U(52), 11, 11, 11,
                        (Uint8)(ui::TOPBAR_A0 * fa), 0);

        // title (left)
        std::string title = g_mpv->title();
        if (title.empty()) title = "幻影视频";
        if (title.size() > 55) title = title.substr(0, 52) + "...";
        g_text.drawText(U(20), U(14) + topOff, title, T(14), 255, 255, 255);

        // icons (right) — 纯白图标 + 悬停高亮背景
        int topH = U(52);
        int iconY = topH / 2 + topOff;
        auto A = [&](Uint8 base) { return (Uint8)(base * fa); };
        int iconSz = U(42);
        int iconDrawSz = U(28);

        int hoverR = iconSz / 2 + U(2);
        int rx = w - U(20);
        struct TopDef { const char* id; };
        const TopDef topIcons[] = {
            {"close"}, {"maximize"}, {"minimize"},
            {"list"}, {"pip"}, {"camera"}
        };
        for (int i = 0; i < 6; ++i) {
            // 悬停背景
            if (g_ui.topbarHover == i) {
                SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
                if (i == 0) {
                    // close: 红色悬停
                    SDL_SetRenderDrawColor(g_sdlRdr, 232, 17, 35, A(240));
                } else {
                    SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, A(50));
                }
                SDL_Rect hrc = {rx - hoverR, iconY - hoverR, hoverR * 2, hoverR * 2};
                SDL_RenderFillRect(g_sdlRdr, &hrc);
            }
            // 纯白图标, 放大加粗
            svgicon::draw(g_sdlRdr, topIcons[i].id, rx, iconY, iconDrawSz,
                          255, 255, 255, A(255));
            rx -= iconSz;
        }
    }

    // 控件淡出: 控制栏随 alpha 滑出屏底
    int ctrlH = U(80);
    int barTop = sbTopY() + (int)((1.0f - fa) * ctrlH + 0.5f);

    // --- 暂停压暗遮罩 + 中央播放图标 (per-pixel alpha 真半透明, 全画面均匀) ---
    if (fa > 0.01f && g_mpv->state() == MpvBackend::State::Paused) {
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, (Uint8)(110 * fa));
        SDL_Rect dim = {0, 0, w, h};
        SDL_RenderFillRect(g_sdlRdr, &dim);
        int ccx = w / 2, ccy = h / 2;
        svgicon::draw(g_sdlRdr, "play", ccx, ccy, U(52),
                      255, 255, 255, (Uint8)(220 * fa));
    }

    // --- gradient background (效果图: 单层渐变 底部→顶部全透) ---
    drawGradientBar(g_sdlRdr, 1, 0, barTop, w, ctrlH, 0, 0, 0, ui::CTRLBAR_A0, ui::CTRLBAR_A1);

    // --- seekbar (at very top of bar) ---
    if (dur > 0) {
        int tx = sbLeftX(), tw = sbWidth();
        int ty = barTop + U(9);
        int th = g_ui.seekbarHover ? U(10) : U(6);

        // track background
        SDL_Rect bgRc = {tx, ty, tw, th};
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, ui::SEEK_TRACK_A);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);

        // buffer fill (behind progress)
        double buf = g_mpv->bufferFill();
        if (buf > 0.0 && buf < 1.0) {
            int bufW = (int)(tw * buf);
            SDL_Rect bufRc = {tx, ty, bufW, th};
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, ui::SEEK_BUF_A);
            SDL_RenderFillRect(g_sdlRdr, &bufRc);
        }

        // progress
        int progW = (int)(tw * pos / dur);
        if (progW > 0) {
            SDL_Rect prRc = {tx, ty, progW, th};
            SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
            SDL_RenderFillRect(g_sdlRdr, &prRc);
        }

        // thumb (on hover or drag) + 时间预览气泡
        if (g_ui.seekbarHover || g_ui.seekingDrag) {
            int cx = tx + progW;
            int cy = ty + th / 2;
            int r = std::max(U(6), (int)(g_ui.winW * 0.006f));  // thumb 半径, 按窗口缩放
            // 圆形 thumb
            fillCircle(g_sdlRdr, cx, cy, r, 255, 255, 255, 255);

            // 预览时间戳气泡
            double hoverPos = dur * ((double)(g_ui.mouseX - tx) / tw);
            if (hoverPos < 0) hoverPos = 0;
            if (hoverPos > dur) hoverPos = dur;
            char pv[16];
            formatTime(pv, sizeof(pv), hoverPos);
            int bw = g_text.measureText(pv, T(11)) + U(16);
            int bh = U(22);
            int bx = g_ui.mouseX - bw / 2;
            if (bx < tx) bx = tx;
            if (bx + bw > tx + tw) bx = tx + tw - bw;
            int by = ty - bh - U(10);
            // 气泡背景(圆角近似)
            SDL_SetRenderDrawColor(g_sdlRdr, 20, 20, 22, 235);
            SDL_Rect bubble = {bx + U(4), by, bw - U(8), bh};
            SDL_RenderFillRect(g_sdlRdr, &bubble);
            SDL_Rect bubbleL = {bx, by + U(4), U(4), bh - U(8)};
            SDL_RenderFillRect(g_sdlRdr, &bubbleL);
            SDL_Rect bubbleR = {bx + bw - U(4), by + U(4), U(4), bh - U(8)};
            SDL_RenderFillRect(g_sdlRdr, &bubbleR);
            fillCircle(g_sdlRdr, bx + U(4), by + U(4), U(4), 20, 20, 22, 235);
            fillCircle(g_sdlRdr, bx + bw - U(4), by + U(4), U(4), 20, 20, 22, 235);
            fillCircle(g_sdlRdr, bx + U(4), by + bh - U(4), U(4), 20, 20, 22, 235);
            fillCircle(g_sdlRdr, bx + bw - U(4), by + bh - U(4), U(4), 20, 20, 22, 235);
            // 气泡文字
            g_text.drawText(bx + U(8), by + U(4), pv, T(11), 255, 255, 255);
        }
    }

    // --- controlbar row1 (效果图复刻): prev/PLAY白底/next/time ... 字幕/倍速/画质/音量/设置/全屏 ---
    {
        Row1Layout L;
        bool volOpen = (g_ui.volumeSliderOpen || g_ui.volumeDragging);
        layoutRow1(w, h, volOpen, L);
        auto A = [&](Uint8 base) { return (Uint8)(base * fa); };
        const int iconC = ui::ICON_BRIGHT, text2 = ui::ICON_DIM;

        // prev
        int ctrlIconSz = U(42);
        svgicon::draw(g_sdlRdr, "prev", L.prev.x + ctrlIconSz / 2, L.prev.y + ctrlIconSz / 2, U(28),
                      255, 255, 255, A(255));
        // PLAY 白图标
        {
            const char* pi = (g_mpv->state() == MpvBackend::State::Paused) ? "play" : "pause";
            svgicon::draw(g_sdlRdr, pi, L.play.x + L.play.w / 2, L.play.y + L.play.h / 2,
                          U(28), 255, 255, 255, A(255));
        }
        // next
        svgicon::draw(g_sdlRdr, "next", L.next.x + ctrlIconSz / 2, L.next.y + ctrlIconSz / 2, U(28),
                      255, 255, 255, A(255));
        // time（tabular 观感: 等宽由字体保证）
        {
            char cur[32], tot[32], ts[80];
            formatTime(cur, sizeof(cur), pos);
            formatTime(tot, sizeof(tot), dur);
            std::snprintf(ts, sizeof(ts), "%s / %s", cur, tot);
            g_text.drawText(L.timeX, L.cy - U(9), ts, T(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        }

        // 右侧 textbtn 组 (文字 + 图标)
        auto drawTextBtn = [&](const SDL_Rect& rc, const char* label,
                               const char* iconId, Uint8 ir, Uint8 ig, Uint8 ib) {
            int tw = g_text.measureText(label, T(12));
            int tx = rc.x + U(8);
            g_text.drawText(tx, rc.y + U(10), label, T(12), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5);
            svgicon::draw(g_sdlRdr, iconId, tx + tw + U(9), rc.y + U(17), U(22),
                          ir, ig, ib, A(255));
        };
        // 字幕
        {
            Uint8 ic = g_mpv->subVisible() ? 255 : 110;
            drawTextBtn(L.subBtn, i18n::subtitles(), "cc", ic, ic, ic);
        }
        // 倍速
        {
            char spd[16];
            float s = g_mpv->speed();
            if (s == (int)s) std::snprintf(spd, sizeof(spd), "%.0fx", s);
            else             std::snprintf(spd, sizeof(spd), "%.2fx", s);
            int lw = g_text.measureText(i18n::speed(), T(12));
            g_text.drawText(L.speedBtn.x + U(8), L.speedBtn.y + U(10), i18n::speed(), T(12), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5);
            g_text.drawText(L.speedBtn.x + U(8) + lw + U(4), L.speedBtn.y + U(10), spd, T(12),
                            ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B);
        }
        // 画质 + 分辨率标签
        {
            const char* ql = qualityLabel();
            int qw = g_text.measureText(i18n::quality(), T(12));
            g_text.drawText(L.qualityBtn.x + U(8), L.qualityBtn.y + U(10), i18n::quality(), T(12), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5);
            g_text.drawText(L.qualityBtn.x + U(8) + qw + U(4), L.qualityBtn.y + U(11), ql, T(11), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        }
        // 音量图标
        {
            const char* vid = g_mpv->muted() ? "mute" : "volume";
            svgicon::draw(g_sdlRdr, vid, L.volIconCx, L.cy, U(28),
                          255, 255, 255, A(255));
        }
        // 设置(文字+gear)
        drawTextBtn(L.setBtn, i18n::settings(), "gear", 255, 255, 255);
        // 全屏
        const char* fid = g_ui.fullscreen ? "exitfull" : "full";
        svgicon::draw(g_sdlRdr, fid, L.fullBtn.x + ctrlIconSz / 2, L.fullBtn.y + ctrlIconSz / 2, U(28),
                      255, 255, 255, A(255));

        // 音量滑条(展开态, 在 Row1Layout 内用 L.volIconCx 定位)
        if (volOpen && L.volSliderW > 0) {
            int sldW = U(70);
            int sx = L.volSliderX;
            int sy = L.cy - U(2);
            int slH = U(4);
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 51);
            SDL_Rect trk = {sx, sy, sldW, slH};
            SDL_RenderFillRect(g_sdlRdr, &trk);
            float v = g_mpv->volume();
            int fw = (int)(sldW * v);
            if (fw > 0) {
                SDL_Rect fl = {sx, sy, fw, slH};
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
                SDL_RenderFillRect(g_sdlRdr, &fl);
            }
        }
    }

    // --- buffering indicator ---
    if (g_mpv->bufferFill() < 0.5) {
        g_text.drawText(w / 2 - U(30), barTop + U(75), "Buffering...", T(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
    }

    // --- speed popup menu（效果图规格: 圆角r8/向上展开/k标注） ---
    if (g_ui.speedMenuOpen) {
        Row1Layout L;
        layoutRow1(w, h, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32);
        int menuW = U(132);
        int menuH = SPEED_PRESET_COUNT * itemH + U(12);
        int menuX = L.speedBtn.x;                        // 与按钮左对齐
        int menuY = L.speedBtn.y - menuH - U(6);        // 向上展开
        if (menuY < 0) menuY = L.speedBtn.y + L.speedBtn.h + U(6);  // 空间不足时回退向下
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);

        // 圆角矩形: 先画矩形主体, 再用圆填充四角
        int cr = U(8);  // corner radius
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
            int iy = menuY + U(6) + i * itemH;
            bool sel = (std::abs(curSpeed - SPEED_PRESETS[i]) < 0.01f);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            char label[16];
            float sp = SPEED_PRESETS[i];
            if (sp == (int)sp) std::snprintf(label, sizeof(label), "%.2fx", sp);
            else               std::snprintf(label, sizeof(label), "%.2fx", sp);
            g_text.drawText(menuX + U(10), iy + U(6), label, T(13), tr, tg, tb);
            // k 标注: 慢/正常/快
            const char* k = (sp < 0.99f) ? T("慢", "Slow") : (sp < 1.01f) ? T("正常", "Normal") :
                            (sp < 2.01f) ? nullptr : T("快", "Fast");
            if (k) {
                int kw = g_text.measureText(k, T(11));
                g_text.drawText(menuX + menuW - kw - U(10), iy + U(7), k, T(11), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
            }
        }
    }

    // --- quality popup menu (画质: 视频信息 + 三档预设) ---
    if (g_ui.qualityMenuOpen) {
        Row1Layout L;
        layoutRow1(w, h, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32);
        int menuW = U(140);
        int infoH = U(38);
        int menuH = infoH + QUALITY_PRESET_COUNT * itemH + U(12);
        int menuX = L.qualityBtn.x;
        int menuY = L.qualityBtn.y - menuH - U(6);
        if (menuY < 0) menuY = L.qualityBtn.y + L.qualityBtn.h + U(6);
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);

        // 圆角矩形背景
        int cr = U(8);
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);

        // 视频信息区
        int iw = g_mpv->videoWidth(), ih = g_mpv->videoHeight();
        char info[64];
        std::snprintf(info, sizeof(info), "%dx%d", iw, ih);
        g_text.drawText(menuX + U(10), menuY + U(8), info, T(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        // 分隔线
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 20);
        SDL_RenderDrawLine(g_sdlRdr, menuX + U(8), menuY + infoH - U(4),
                           menuX + menuW - U(8), menuY + infoH - U(4));

        // 预设选项
        const char* qNames[] = { T("省电", "Power Saving"), T("标准", "Standard"), T("至臻", "Ultimate") };
        for (int i = 0; i < QUALITY_PRESET_COUNT; ++i) {
            int iy = menuY + infoH + i * itemH;
            bool sel = (g_ui.qualityPreset == i);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            g_text.drawText(menuX + U(10), iy + U(8), qNames[i], T(13), tr, tg, tb);
            // 当前选中标记
            if (sel) {
                g_text.drawText(menuX + menuW - U(24), iy + U(8), "✓", T(13), ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B);
            }
        }
    }
    // --- EQ popup menu (6频段均衡器) ---
    if (g_ui.eqMenuOpen) {
        Row1Layout L;
        layoutRow1(w, h, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        static const char* bandNames[] = {"60Hz","170Hz","310Hz","600Hz","3kHz","12kHz"};
        int sliderW = U(100);
        int itemH = U(36);
        int menuW = U(200);
        int menuH = U(32) + 6 * itemH + U(40);  // title + 6 bands + reset button
        int menuX = w / 2 - menuW / 2;           // 居中显示
        int menuY = h / 2 - menuH / 2;

        // 背景
        int cr = U(8);
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        // 标题
        g_text.drawText(menuX + U(10), menuY + U(10), i18n::equalizer(), T(13), 255, 255, 255);
        // 开关状态
        const char* st = g_mpv->eqEnabled() ? "ON" : "OFF";
        Uint8 sr = g_mpv->eqEnabled() ? 59 : 161, sg = g_mpv->eqEnabled() ? 130 : 161, sb = g_mpv->eqEnabled() ? 246 : 166;
        g_text.drawText(menuX + menuW - U(40), menuY + U(10), st, T(12), sr, sg, sb);

        // 6 频段滑块
        int baseY = menuY + U(32);
        int trackX = menuX + U(60);
        int trackW = sliderW;
        for (int i = 0; i < 6; ++i) {
            int iy = baseY + i * itemH;
            g_text.drawText(menuX + U(10), iy + U(8), bandNames[i], T(11), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
            // 轨道
            SDL_SetRenderDrawColor(g_sdlRdr, 58, 58, 62, 255);
            SDL_Rect trk = {trackX, iy + U(14), trackW, U(4)};
            SDL_RenderFillRect(g_sdlRdr, &trk);
            // 滑块位置: gain -12..+12 → 0..1
            float gain = g_mpv->eqGain(i);
            float norm = (gain + 12.0f) / 24.0f;
            if (norm < 0.0f) norm = 0.0f; if (norm > 1.0f) norm = 1.0f;
            int thumbX = trackX + (int)(norm * trackW);
            // 滑块 thumb
            fillCircle(g_sdlRdr, thumbX, iy + U(16), U(6), ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, 255);
            // 数值
            char val[16];
            std::snprintf(val, sizeof(val), "%+.0f", gain);
            g_text.drawText(trackX + trackW + U(8), iy + U(8), val, T(11), ui::ICON_BRIGHT, ui::ICON_BRIGHT, 231);
            // 存储滑块区域用于点击
            static SDL_Rect s_bandRects[6];
            s_bandRects[i] = {trackX - U(8), iy, trackW + U(16), itemH};
            // (hit-test 在后面处理)
        }
        // Reset 按钮
        int resetY = baseY + 6 * itemH + U(4);
        SDL_Rect resetRc = {menuX + menuW / 2 - U(30), resetY, U(60), U(26)};
        SDL_SetRenderDrawColor(g_sdlRdr, 58, 58, 62, 255);
        SDL_RenderFillRect(g_sdlRdr, &resetRc);
        g_text.drawText(resetRc.x + U(14), resetRc.y + U(5), i18n::reset(), T(11), ui::ICON_BRIGHT, ui::ICON_BRIGHT, 231);
    }
    if (g_ui.playlistOpen) {
        int panelW, panelX;
        if (!g_ui.fullscreen) {
            panelW = totalW - w;                 // 窗口扩展出的独立区域
            panelX = w;
        } else {                                  // 全屏无法扩窗: 覆盖式
            panelW = U(430);
            panelX = w - panelW;
        }
        if (panelW < U(200)) { panelW = U(200); panelX = w - panelW; }   // 兜底
        int panelH = h;
        int panelY = 0;

        // panel background（独立区域不透明）
        SDL_Rect pRc = {panelX, panelY, panelW, panelH};
        SDL_SetRenderDrawColor(g_sdlRdr, 16, 16, 17, 255);
        SDL_RenderFillRect(g_sdlRdr, &pRc);
        // left border
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
        SDL_RenderDrawLine(g_sdlRdr, panelX, panelY, panelX, panelY + panelH);

        // title + 关闭钮（效果图 .pl-head）
        g_text.drawText(panelX + U(14), panelY + U(16), i18n::playlist(), T(13), 255, 255, 255);
        int closeX = panelX + panelW - U(40);
        int closeY = panelY + U(10);
        SDL_Rect closeRc = {closeX, closeY, U(28), U(28)};
        svgicon::draw(g_sdlRdr, "close", closeX + U(14), closeY + U(14), U(22),
                      255, 255, 255, 255);
        g_ui.plCloseRect = closeRc;

        // items from playlist queue（卡片化: thumb100×56+dur角标+title+state）
        int itemY = panelY + U(45);
        int itemH = U(72);                       // 卡片高度(56 thumb+padding)
        int scroll = g_ui.playlistScroll;
        std::vector<std::string> visiblePaths;
        // 裁剪到列表区: 滚动时内容不会盖住固定标题栏
        SDL_Rect listClip = {panelX, itemY, panelW, panelY + panelH - itemY};
        SDL_RenderSetClipRect(g_sdlRdr, &listClip);
        for (size_t pi = 0; pi < g_playlist.size(); ++pi) {
            int iy = itemY + (int)pi * itemH - scroll;
            if (iy + itemH < itemY - U(60)) continue;
            if (iy >= panelY + panelH - U(10)) break;
            const std::string& p = g_playlist[pi];
            visiblePaths.push_back(p);
            bool isCurrent = (g_mpv && g_mpv->path() == p);

            double hpos = 0;
            auto hit = g_cfg.history.find(p);
            if (hit != g_cfg.history.end()) hpos = hit->second.pos;

            // hover 背景（鼠标在本项内）
            bool hovered = (g_ui.mouseX >= panelX + U(8) &&
                            g_ui.mouseX <= panelX + panelW - U(8) &&
                            g_ui.mouseY >= iy && g_ui.mouseY <= iy + itemH - U(6));
            if (isCurrent || hovered) {
                SDL_Rect hlRc = {panelX + U(7), iy, panelW - U(15), itemH - U(4)};
                if (isCurrent) SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 46);
                else           SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 15);
                SDL_RenderFillRect(g_sdlRdr, &hlRc);
            }

            // 缩略图 100×56 r7（渐变占位底 #26262c→#15151a 近似）
            SDL_Rect thRc = {panelX + U(12), iy + U(8), U(100), U(56)};
            SDL_SetRenderDrawColor(g_sdlRdr, 33, 33, 38, 255);
            SDL_RenderFillRect(g_sdlRdr, &thRc);
            auto texIt = g_thumbTex.find(p);
            if (texIt != g_thumbTex.end()) {
                SDL_RenderCopy(g_sdlRdr, texIt->second, nullptr, &thRc);
            } else {
                svgicon::draw(g_sdlRdr, "play", thRc.x + U(50), thRc.y + U(28), U(24),
                              255, 255, 255, 255);
            }
            // dur 角标(right4 bottom4 黑.72)
            {
                char durBuf[16] = "";
                if (hpos > 1.0) {
                    std::snprintf(durBuf, sizeof(durBuf), "%02d:%02d",
                                  (int)(hpos / 60), (int)hpos % 60);
                    int dw = g_text.measureText(durBuf, T(9)) + U(8);
                    int dx = thRc.x + thRc.w - dw - U(4);
                    int dy = thRc.y + thRc.h - U(18);
                    SDL_Rect db = {dx, dy, dw, U(15)};
                    SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 184);
                    SDL_RenderFillRect(g_sdlRdr, &db);
                    g_text.drawText(dx + U(4), dy + U(2), durBuf, T(9), 255, 255, 255);
                }
            }

            // meta: title 一行 + state 行
            std::string fn = fileNameOf(p);
            int maxTw = panelW - U(140);
            if (maxTw < U(80)) maxTw = U(80);
            {
                // 按像素宽截断
                if (g_text.measureText(fn, T(12)) > maxTw) {
                    while (fn.size() > 4 && g_text.measureText(fn + "...", T(12)) > maxTw)
                        fn.pop_back();
                    fn += "...";
                }
                Uint8 tr = isCurrent ? 191 : 240, tg = isCurrent ? 214 : 240,
                      tb = isCurrent ? 255 : 240;   // playing #bfd6ff
                g_text.drawText(thRc.x + thRc.w + U(10), iy + U(10), fn, T(12), tr, tg, tb);
            }
            // state: 正在播放(accent2)/已播放(#6b7280)/未播放(#3f3f46)
            {
                const char* st; Uint8 sr, sg_, sb_;
                if (isCurrent) { st = i18n::playing(); sr = 59; sg_ = 130; sb_ = 246; }
                else if (hpos > 1.0) { st = i18n::played(); sr = 107; sg_ = 114; sb_ = 128; }
                else { st = i18n::unplayed(); sr = 63; sg_ = 63; sb_ = 70; }
                g_text.drawText(thRc.x + thRc.w + U(10), iy + U(32), st, T(11), sr, sg_, sb_);
            }
        }
        SDL_RenderSetClipRect(g_sdlRdr, nullptr);   // 解除裁剪(拖拽指示线/滚动条可越界)

        // 拖拽排序视觉反馈：插入指示线 + 被拖项高亮
        if (g_ui.plDragging && g_ui.plDragFrom >= 0) {
            int itemH = U(72);
            int topY = panelY + U(45);
            float rel = (float)(g_ui.plDragY - topY) + g_ui.playlistScroll;
            int drop = (int)(rel / itemH + 0.5f);
            int n = (int)g_playlist.size();
            if (drop < 0) drop = 0;
            if (drop > n) drop = n;
            int lineY = topY + drop * itemH - g_ui.playlistScroll - itemH / 2 + itemH / 2;
            lineY = topY + drop * itemH - g_ui.playlistScroll;
            if (lineY >= panelY && lineY <= panelY + panelH) {
                SDL_Rect line = {panelX + U(4), lineY - U(2), panelW - U(10), U(3)};
                SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                SDL_RenderFillRect(g_sdlRdr, &line);
            }
            int fromY = topY + g_ui.plDragFrom * itemH - g_ui.playlistScroll;
            if (fromY >= panelY && fromY <= panelY + panelH) {
                SDL_Rect hl = {panelX + U(4), fromY - U(2), panelW - U(8), itemH};
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 30);
                SDL_RenderFillRect(g_sdlRdr, &hl);
            }
        }

        // 提交可见集给缩略图 worker（仅缺图的; 合并语义, 不清其他视图的请求）
        {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            for (auto& p : visiblePaths) {
                if (!g_thumbRgb.count(p) && !g_thumbTex.count(p) &&
                    std::find(g_thumbWant.begin(), g_thumbWant.end(), p) == g_thumbWant.end())
                    g_thumbWant.push_back(p);
            }
        }

        // scrollbar（M33d: 悬停加亮/拖拽/轨道跳页）
        {
            int contentH = (int)g_playlist.size() * itemH;
            int viewH = panelH - U(55);
            if (contentH > viewH && contentH > 0) {
                int trackW = U(6);
                int trackX = panelX + panelW - trackW - U(4);
                int trackY = panelY + U(45);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 18);
                SDL_Rect trk = {trackX, trackY, trackW, viewH};
                SDL_RenderFillRect(g_sdlRdr, &trk);
                int barH = std::max(U(30), viewH * viewH / contentH);
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
            g_text.drawText(panelX + U(16), itemY + U(10), i18n::emptyPlaylist(), T(12), 100, 100, 100);
        }
    }

    // --- settings modal panel ---
    if (g_ui.settingsOpen) {
        SettingsGeom sg = settingsGeom(w, h);

        // 模态背景: 真半透明压暗(per-pixel alpha), 视频隐约可见
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 140);
        SDL_Rect fullRc = {0, 0, w, h};
        SDL_RenderFillRect(g_sdlRdr, &fullRc);

        // 面板阴影（柔和扩散）
        for (int i = 4; i >= 1; --i) {
            Uint8 sha = (Uint8)(12 * i);
            SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, sha);
            SDL_Rect sr = {sg.panelX - i*2, sg.panelY - i*2, sg.panelW + i*4, sg.panelH + i*4};
            SDL_RenderDrawRect(g_sdlRdr, &sr);
        }

        // panel (圆角矩形)
        int cr = U(12);
        SDL_SetRenderDrawColor(g_sdlRdr, 28, 28, 30, 255);
        SDL_Rect pBody = {sg.panelX + cr, sg.panelY, sg.panelW - cr*2, sg.panelH};
        SDL_RenderFillRect(g_sdlRdr, &pBody);
        SDL_Rect pH = {sg.panelX, sg.panelY + cr, sg.panelW, sg.panelH - cr*2};
        SDL_RenderFillRect(g_sdlRdr, &pH);
        fillCircle(g_sdlRdr, sg.panelX + cr, sg.panelY + cr, cr, 28, 28, 30, 255);
        fillCircle(g_sdlRdr, sg.panelX + sg.panelW - cr, sg.panelY + cr, cr, 28, 28, 30, 255);
        fillCircle(g_sdlRdr, sg.panelX + cr, sg.panelY + sg.panelH - cr, cr, 28, 28, 30, 255);
        fillCircle(g_sdlRdr, sg.panelX + sg.panelW - cr, sg.panelY + sg.panelH - cr, cr, 28, 28, 30, 255);
        // 边框
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 20);
        SDL_Rect borderH = {sg.panelX + cr, sg.panelY, sg.panelW - cr*2, sg.panelH};
        SDL_RenderDrawRect(g_sdlRdr, &borderH);
        SDL_Rect borderV = {sg.panelX, sg.panelY + cr, sg.panelW, sg.panelH - cr*2};
        SDL_RenderDrawRect(g_sdlRdr, &borderV);

        // title + close
        g_text.drawText(sg.panelX + U(20), sg.panelY + U(16), i18n::settingsTitle(), T(16), 255, 255, 255);
        svgicon::draw(g_sdlRdr, "close", sg.closeCx, sg.closeCy, U(26), 255, 255, 255, 200);

        // toggle rows
        int toggleVals[SET_ROW_COUNT] = { g_cfg.hwDecode, g_cfg.volNorm,
            g_cfg.subAutoLoad, g_cfg.thumbCache, g_cfg.resume,
            g_cfg.nightMode, g_cfg.audioExclusive, g_cfg.motionInterp,
            g_cfg.hiQScale };
        const char* rowLabels[SET_ROW_COUNT] = {
            i18n::hwDecode(), i18n::volNorm(), i18n::subAutoLoad(),
            i18n::thumbCache(), i18n::resume(), i18n::nightMode(),
            i18n::exclusiveAudio(), i18n::motionInterp(), i18n::hiQScaling(),
        };
        for (int i = 0; i < SET_ROW_COUNT; ++i) {
            int ry = sg.rowY[i];
            bool on = (toggleVals[i] != 0);
            g_text.drawText(sg.panelX + U(20), ry + U(3), rowLabels[i], T(13), on ? 230 : 170, on ? 230 : 170, on ? 230 : 170);

            // Switch
            SDL_Rect swRc = {sg.swX, ry, sg.swW, sg.swH};
            int swR = sg.swH / 2;
            if (on) {
                fillCircle(g_sdlRdr, sg.swX + swR, ry + swR, swR, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                fillCircle(g_sdlRdr, sg.swX + sg.swW - swR, ry + swR, swR, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                SDL_Rect mid = {sg.swX + swR, ry, sg.swW - sg.swH, sg.swH};
                SDL_RenderFillRect(g_sdlRdr, &mid);
            } else {
                fillCircle(g_sdlRdr, sg.swX + swR, ry + swR, swR, 80, 80, 80, 255);
                fillCircle(g_sdlRdr, sg.swX + sg.swW - swR, ry + swR, swR, 80, 80, 80, 255);
                SDL_Rect mid = {sg.swX + swR, ry, sg.swW - sg.swH, sg.swH};
                SDL_RenderFillRect(g_sdlRdr, &mid);
            }
            int thumbX = on ? sg.swX + sg.swW - sg.swH + U(2) : sg.swX + U(2);
            fillCircle(g_sdlRdr, thumbX + (sg.swH - U(4))/2, ry + U(10), (sg.swH - U(4))/2, 255, 255, 255, 255);
        }

        // playback mode row (选中=蓝色胶囊, 未选中=纯文字无边框)
        g_text.drawText(sg.panelX + U(20), sg.modeRowY + U(3), i18n::playbackMode(), T(13), 200, 200, 200);
        const char* modes[] = { i18n::modeSingle(), i18n::modeLoop(), i18n::modeShuffle() };
        for (int i = 0; i < 3; ++i) {
            int lx = sg.swX - U(180) + i * (sg.chipW + U(6));
            bool sel = (g_cfg.playMode == i);
            if (sel) {
                int cr2 = sg.chipH / 2;
                SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                SDL_Rect lr = {lx + cr2, sg.chipY, sg.chipW - cr2*2, sg.chipH};
                SDL_RenderFillRect(g_sdlRdr, &lr);
                fillCircle(g_sdlRdr, lx + cr2, sg.chipY + cr2, cr2, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                fillCircle(g_sdlRdr, lx + sg.chipW - cr2, sg.chipY + cr2, cr2, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
            }
            int tw = g_text.measureText(modes[i], T(11));
            g_text.drawText(lx + (sg.chipW - tw) / 2, sg.chipY + U(4), modes[i], T(11),
                            sel ? 255 : 150, sel ? 255 : 150, sel ? 255 : 150);
        }

        // 语言切换行 (同风格: 选中=蓝色胶囊, 未选中=纯文字)
        g_text.drawText(sg.panelX + U(20), sg.langRowY + U(3), i18n::language(), T(13), 200, 200, 200);
        const char* langLabels[] = { i18n::chinese(), i18n::english() };
        for (int i = 0; i < 2; ++i) {
            int lx = sg.langSegX + i * (sg.langSegW / 2);
            bool sel = (g_cfg.lang == i);
            int halfW = sg.langSegW / 2;
            if (sel) {
                int cr2 = sg.langSegH / 2;
                SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                SDL_Rect lr = {lx + cr2, sg.langRowY, halfW - cr2*2, sg.langSegH};
                SDL_RenderFillRect(g_sdlRdr, &lr);
                fillCircle(g_sdlRdr, lx + cr2, sg.langRowY + cr2, cr2, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                fillCircle(g_sdlRdr, lx + halfW - cr2, sg.langRowY + cr2, cr2, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
            }
            int tw2 = g_text.measureText(langLabels[i], T(11));
            g_text.drawText(lx + (halfW - tw2) / 2, sg.langRowY + U(4), langLabels[i], T(11),
                            sel ? 255 : 150, sel ? 255 : 150, sel ? 255 : 150);
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
            int tw = g_text.measureText(g_ui.toastMsg, T(13));
            int bh = U(36), capR = bh / 2, padX = U(20);
            int bw = tw + padX * 2;
            int bx = w / 2 - bw / 2, by = U(60);
            SDL_SetRenderDrawColor(g_sdlRdr, 15, 15, 17, a);
            SDL_Rect mid = {bx + capR, by, bw - capR * 2, bh};
            SDL_RenderFillRect(g_sdlRdr, &mid);
            fillCircle(g_sdlRdr, bx + capR, by + bh / 2, bh / 2, 15, 15, 17, a);
            fillCircle(g_sdlRdr, bx + bw - capR, by + bh / 2, bh / 2, 15, 15, 17, a);
            // border (white.10)
            Uint8 ba = (Uint8)(26 * alpha);
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, ba);
            SDL_RenderDrawRect(g_sdlRdr, &mid);
            g_text.drawText(w / 2 - tw / 2, by + U(8), g_ui.toastMsg, T(13), 255, 255, 255);
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
                int padX = U(14), padY = U(10), lineH = U(22);
                int boxW = U(340), boxH = padY * 2 + lines * lineH;
                int boxX = U(16), boxY = curTopH() + U(12);
                SDL_Rect bg = {boxX, boxY, boxW, boxH};
                SDL_SetRenderDrawColor(g_sdlRdr, 11, 11, 11, 200);
                SDL_RenderFillRect(g_sdlRdr, &bg);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 30);
                SDL_RenderDrawRect(g_sdlRdr, &bg);

                int ty = boxY + padY;
                if (line1[0]) { g_text.drawText(boxX + padX, ty, line1, T(12), 255, 255, 255); ty += lineH; }
if (line2[0]) { g_text.drawText(boxX + padX, ty, line2, T(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B); ty += lineH; }
        if (line3[0]) { g_text.drawText(boxX + padX, ty, line3, T(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B); }
            }
        }
    }

    overlayPresent();
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

    Logger::instance().init("phantom", 7);
    bool diag = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--debug") { diag = true; break; }
    Logger::instance().setLevel(diag ? LogLevel::Trace : LogLevel::Warn);
    LOG_INFO("MAIN", "phantom video (mpv + SDL2 overlay) starting");

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
    wc.lpszClassName  = L"PhantomParent";
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    // 创建窗口前先取主屏 DPI，保证 U(960)xU(540) 按物理像素展开
    {
        HDC dc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, dc);
        LOG_INFO("MAIN", "initial dpi scale=%.2f", g_dpi);
    }
    // 记忆位置优先；无效则默认尺寸 + 系统级联位置
    int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT;
    int winW = U(960), winH = U(540);
    if (g_cfg.posX != AppConfig::INVALID_POS && g_cfg.posW > 0) {
        winX = g_cfg.posX; winY = g_cfg.posY;
        winW = g_cfg.posW; winH = g_cfg.posH;
        LOG_INFO("MAIN", "restore window pos (%d,%d) %dx%d", winX, winY, winW, winH);
    }
    g_parentHwnd = CreateWindowExW(WS_EX_ACCEPTFILES,
        wc.lpszClassName, L"幻影视频",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
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
    g_ui.winW = rc.right; g_ui.winH = rc.bottom;
    g_uiBase = std::min(g_ui.winW / 1280.0f, g_ui.winH / 720.0f);
    g_uiBase = std::max(0.45f, std::min(g_uiBase, 2.0f));
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
        // 续播：事件线程只投递待 seek 位置, UI 主循环执行
        // (事件线程直接调 mpv.seek/showToast 会与 UI 线程竞争)
        double pos = g_pendingResumePos;
        g_pendingResumePos = -1.0;
        if (pos > 1.0) {
            std::lock_guard<std::mutex> lk(g_autoNextMtx);
            g_resumeSeekPos = pos;
            g_resumeSeekPending = true;
        }
    };
    mpv.onPlaybackEnded = [&]() {
        LOG_INFO("MAIN", "playback ended");
        if (!g_mpv) return;
        std::string cur = g_mpv->path();
        if (!cur.empty()) recordHistory(cur, 0, g_mpv->duration());   // 看完清零
        int idx = playlistIndexOf(cur);
        int n = (int)g_playlist.size();
        if (idx < 0 || n == 0) return;

        // 注意: 本回调在 mpv 事件线程执行。此处绝不能直接调 playPath/
        // showToast（mpv 命令 + UI 状态会与 UI 线程形成锁循环死锁,
        // 且 g_ui/g_playlist 无锁并发写是数据竞争）。
        // 只计算下一曲路径, 投递给 UI 主循环执行。
        std::string next;
        if (g_cfg.playMode == 2) {                   // Shuffle
            if (n > 1) {
                int pick = idx;
                while (pick == idx) pick = std::rand() % n;
                next = g_playlist[pick];
            }
        } else if (g_cfg.playMode == 1) {            // Loop：顺序循环
            next = g_playlist[(idx + 1) % n];
        }                                            // Single：停住(无 next)
        {
            std::lock_guard<std::mutex> lk(g_autoNextMtx);
            g_autoNextPath = next;
            g_autoNextPending = true;
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

        // EOF 自动连播: 消费事件线程投递的下一曲(UI 线程安全点执行)
        {
            std::string next;
            bool fire = false;
            {
                std::lock_guard<std::mutex> lk(g_autoNextMtx);
                fire = g_autoNextPending;
                if (fire) { next = std::move(g_autoNextPath); g_autoNextPending = false; }
            }
            if (fire) {
                if (next.empty()) showToast(i18n::endOfTrack());   // Single 模式停住
                else playPath(next);
            }
        }

        // 续播 seek: 消费 FILE_LOADED 投递的位置
        {
            double pos = -1.0;
            {
                std::lock_guard<std::mutex> lk(g_autoNextMtx);
                if (g_resumeSeekPending) { pos = g_resumeSeekPos; g_resumeSeekPending = false; }
            }
            if (pos > 1.0 && g_mpv && g_mpv->hasMedia()) {
                LOG_INFO("MAIN", "resume at %.1fs", pos);
                g_mpv->seek(pos);
                char msg[48];
                std::snprintf(msg, sizeof(msg), "%s %02d:%02d",
                    T("续播于", "Resumed at"), (int)(pos / 60), (int)pos % 60);
                showToast(msg);
            }
        }

        // 周期保存播放进度（3 秒）
        if (now - lastPosSave >= 3000) {
            lastPosSave = now;
            if (g_mpv && g_mpv->hasMedia() && g_mpv->state() == MpvBackend::State::Playing) {
                double pos = g_mpv->clock();
                double dur = g_mpv->duration();
                std::string cur = g_mpv->path();
                if (!cur.empty() && dur > 0)
                    recordHistory(cur, (pos < dur - 2.0) ? pos : 0.0, dur);  // 结尾视为看完
            }
        }

        // 定时状态迁移（迁移动作本身置 dirty）
        if (g_ui.visible && now > g_ui.hideAt) {
            g_ui.visible = false;
            g_dirty.store(true);
        }

        // 控件淡入淡出（ease-out 缓动 + 效果图 cb-opacity 最低 0.25）
        {
            float target = g_ui.visible ? 1.0f : 0.25f;
            float cur = g_ui.ctrlAlpha;
            if (std::abs(target - cur) > 0.001f) {
                // ease-out: 接近目标时减速
                float diff = target - cur;
                float step = diff * 0.12f;  // 每帧移动剩余距离的12%
                if (std::abs(step) < 0.005f) step = (diff > 0 ? 0.005f : -0.005f);
                cur += step;
                g_ui.ctrlAlpha = cur;
                g_dirty.store(true);
                waitCap = 16;   // 动画期间高频刷新
            } else {
                g_ui.ctrlAlpha = target;
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
        // AB 循环检测: 播放到 B 点时跳回 A 点
        if (g_mpv && g_mpv->looping() && !g_ui.seekingDrag) {
            double cur = g_mpv->clock();
            if (cur >= g_mpv->loopB()) {
                g_mpv->seek(g_mpv->loopA());
                g_dirty.store(true);
            }
        }
        // 进度保存(3s 周期)由 200ms 兜底轮询覆盖, 无需专门加速

        MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    // 退出前保存最终进度 + 窗口位置（WM_CLOSE 已存，此处兜底）
    if (g_mpv && g_mpv->hasMedia()) {
        double pos = g_mpv->clock();
        double dur = g_mpv->duration();
        std::string cur = g_mpv->path();
        if (!cur.empty() && dur > 0)
            recordHistory(cur, (pos < dur - 2.0) ? pos : 0.0, dur);
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
    LOG_INFO("MAIN", "phantom video exiting");
    return 0;
}
