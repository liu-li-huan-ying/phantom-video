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

    // playlist panel
    bool   playlistOpen = false;
    int    playlistTargetW = 0;  // target window width when playlist open
    int    playlistAnimW = 0;   // current animation width
    int    playlistScroll = 0;  // 滚动偏移(px)

    // 单击暂停延迟判定（双击全屏互斥）
    bool   pendingPause = false;

    // 音量滑条 hover 自动展开/收起
    Uint32 volHoverAt = 0;

    // 播放列表拖拽排序
    int    plDragFrom = -1;     // 按下的项 index
    int    plDownY = 0;        // 按下时 y
    bool   plDragging = false;
    int    plDragY = 0;         // 拖拽中鼠标 y

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
    if (g_ui.fullscreen || g_ui.miniMode || IsIconic(hwnd)) return;
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return;
    g_cfg.posX = wr.left; g_cfg.posY = wr.top;
    g_cfg.posW = wr.right - wr.left; g_cfg.posH = wr.bottom - wr.top;
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

// ---- 设置面板：几何与行定义（渲染/命中共用） ----
struct SettingsGeom {
    int panelX, panelY, panelW, panelH;
    int closeCx, closeCy, closeR;
    int swX, swW, swH;          // 开关
    int rowY[5];                // 5 个开关行
    int modeRowY;               // 播放模式行
    int chipY, chipH, chipW;    // 模式 chips
};
static const int SET_ROW_COUNT = 5;

static SettingsGeom settingsGeom(int w, int h) {
    SettingsGeom g;
    g.panelW = S(380); g.panelH = S(400);
    g.panelX = (w - g.panelW) / 2;
    g.panelY = (h - g.panelH) / 2;
    g.closeCx = g.panelX + g.panelW - S(22);
    g.closeCy = g.panelY + S(22);
    g.closeR = S(12);
    g.swX = g.panelX + g.panelW - S(60);
    g.swW = S(40); g.swH = S(20);
    for (int i = 0; i < SET_ROW_COUNT; ++i)
        g.rowY[i] = g.panelY + S(55) + i * S(44);
    g.modeRowY = g.rowY[4] + S(44);
    g.chipY = g.modeRowY;
    g.chipH = S(24); g.chipW = S(56);
    return g;
}

// 应用设置变更到 mpv（开关翻转时调用）
static void applySetting(const char* key, int value) {
    if (std::strcmp(key, "hw") == 0)        mpvSetOpt("hwdec", value ? "auto-safe" : "no");
    else if (std::strcmp(key, "vol") == 0)  mpvSetOpt("audio-filters", value ? "loudnorm" : "");
    else if (std::strcmp(key, "sub") == 0)  mpvSetOpt("sub-auto", value ? "fuzzy" : "no");
    // thumbCache/resume 纯本地，无需通知 mpv
}

// ---- overlay z 序 ----
static void raiseOverlayAbove() {
    if (!g_overlayHwnd) return;
    // parent 变 TOPMOST/还原后，把 overlay 重新提到同层最上
    SetWindowPos(g_overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// ---- 全屏切换（F 键 / maximize 按钮 / pip 共用） ----
static void toggleFullscreen(HWND hwnd) {
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
        LOG_INFO("MAIN", "fullscreen ON");
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, sty | WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, nullptr, 100, 100, S(960), S(540),
            SWP_FRAMECHANGED | SWP_NOZORDER);
        g_ui.fullscreen = false;
        LOG_INFO("MAIN", "fullscreen OFF");
    }
    raiseOverlayAbove();
}

static void toggleMini(HWND hwnd) {
    if (!g_ui.miniMode) {
        GetWindowRect(hwnd, &g_ui.savedRect);
        g_ui.savedStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE,
            (g_ui.savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        int w = S(480), h = S(270);
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(hwnd, HWND_TOPMOST,
            wa.right - w - S(20), wa.bottom - h - S(20), w, h,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);
        g_ui.miniMode = true;
        LOG_INFO("MAIN", "pip mini ON (%dx%d)", w, h);
        showToast("Picture-in-picture: ON");
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_ui.savedStyle);
        SetWindowPos(hwnd, HWND_NOTOPMOST,
            g_ui.savedRect.left, g_ui.savedRect.top,
            g_ui.savedRect.right  - g_ui.savedRect.left,
            g_ui.savedRect.bottom - g_ui.savedRect.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);
        g_ui.miniMode = false;
        LOG_INFO("MAIN", "pip mini OFF");
        showToast("Picture-in-picture: OFF");
    }
    raiseOverlayAbove();
}

// ---- 播放队列（稳定顺序，文件夹扫描生成，不随播放重排） ----
static std::vector<std::string> g_playlist;
static const char* kVideoExts[] = {
    ".mp4",".mkv",".avi",".mov",".flv",".wmv",".webm",".ts",".m2ts",
    ".rmvb",".rm",".3gp",".mpg",".mpeg"
};
static const size_t PLAYLIST_MAX = 2000;

// 以 file 所在目录扫描视频文件构建播放队列（按文件名排序）
static void buildPlaylistAround(const std::string& file) {
    namespace fs = std::filesystem;
    g_playlist.clear();
    fs::path p(file);
    fs::path dir = p.parent_path();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) { g_playlist.push_back(file); return; }
    std::vector<fs::path> found;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (found.size() >= PLAYLIST_MAX) break;
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().string();
        for (auto* ve : kVideoExts) {
            if (_stricmp(ext.c_str(), ve) == 0) { found.push_back(e.path()); break; }
        }
    }
    if (found.empty()) { g_playlist.push_back(file); return; }
    std::sort(found.begin(), found.end());
    for (auto& f : found) g_playlist.push_back(f.string());
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
            int itemH = S(52);
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
    switch (msg) {

    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0;
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

        // 列表拖拽排序：位移超阈值进入拖拽态
        if (g_ui.plDragFrom >= 0 && !g_ui.plDragging &&
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
            int contentH = (int)g_playlist.size() * S(52);
            int viewH = g_ui.winH - S(ui::TOPBAR_H) - S(55);
            if (g_ui.mouseY > panelBottom - S(30) && g_ui.playlistScroll < contentH - viewH)
                g_ui.playlistScroll += S(12);
        }

        // volume slider drag
        if (g_ui.volumeDragging && g_mpv) {
            int sliderW = S(ui::VOLSIDER_W);
            int sliderX = g_ui.winW - S(54) - sliderW - S(10);
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
            case 3: // playlist
                g_ui.playlistOpen = !g_ui.playlistOpen;
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
        // --- speed popup ---
        else if (g_ui.speedMenuOpen) {
            int menuW = S(80), itemH = S(30);
            int menuX = g_ui.winW - S(150);
            int menuY = barTop - SPEED_PRESET_COUNT * itemH - S(5);
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
        // --- 字幕 cc 图标点击（切换可见性）---
        else if (g_mpv && mx >= g_ui.winW - S(136) && mx <= g_ui.winW - S(108) &&
                 my >= barTop + S(36) && my <= barTop + S(64)) {
            bool vis = !g_mpv->subVisible();
            g_mpv->setSubVisibility(vis);
            std::string trk = g_mpv->currentSubTrack();
            char msg[96];
            if (vis)
                std::snprintf(msg, sizeof(msg), "Subtitles ON %s",
                              trk.empty() ? "" : ("[" + trk + "]").c_str());
            else
                std::snprintf(msg, sizeof(msg), "Subtitles OFF");
            showToast(msg);
            LOG_DBG("MAIN", "sub visibility -> %d", vis ? 1 : 0);
        }
        // --- speed label click（toggle 弹出菜单）---
        else if (g_mpv && mx >= g_ui.winW - S(170) && mx <= g_ui.winW - S(138) &&
                 my >= barTop + S(36) && my <= barTop + S(64)) {
            g_ui.speedMenuOpen = !g_ui.speedMenuOpen;
        }
        // --- settings gear click ---
        else if (g_mpv && mx >= g_ui.winW - S(102) && mx <= g_ui.winW - S(74) &&
                 my >= barTop + S(36) && my <= barTop + S(64)) {
            g_ui.settingsOpen = !g_ui.settingsOpen;
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
                    &g_cfg.resume, &g_cfg.subAutoLoad, &g_cfg.thumbCache };
                const char* keys[SET_ROW_COUNT] = { "hw", "vol", "resume", "sub", "thumb" };
                const char* names[SET_ROW_COUNT] = { "Hardware Decode", "Volume Norm",
                    "Resume", "Sub Auto-Load", "Thumb Cache" };
                bool handled = false;
                for (int i = 0; i < SET_ROW_COUNT && !handled; ++i) {
                    if (my >= sg.rowY[i] - S(6) && my <= sg.rowY[i] + sg.swH + S(6) &&
                        mx >= sg.panelX + S(12)) {
                        *vals[i] = *vals[i] ? 0 : 1;
                        applySetting(keys[i], *vals[i]);
                        showToast(names[std::strcmp(keys[i],"hw")==0 ? 0 :
                                        std::strcmp(keys[i],"vol")==0 ? 1 :
                                        std::strcmp(keys[i],"resume")==0 ? 2 :
                                        std::strcmp(keys[i],"sub")==0 ? 3 : 4]);
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
        // --- volume slider drag ---
        else if (g_ui.volumeSliderOpen && g_mpv) {
            int sliderW = S(ui::VOLSIDER_W);
            int sliderX = g_ui.winW - S(54) - sliderW - S(10);
            int sliderY = barTop + S(50) - S(2);
            if (mx >= sliderX && mx <= sliderX + sliderW && my >= sliderY - S(8) && my <= sliderY + S(12)) {
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
        // --- prev / next 按钮（传输区两侧） ---
        else if (g_mpv && my >= barTop + S(36) && my <= barTop + S(64)) {
            int cx0 = g_ui.winW / 2;
            if (mx >= cx0 - S(64) && mx <= cx0 - S(36)) {          // prev
                int idx = playlistIndexOf(g_mpv->path());
                if (idx > 0) { playIndex(idx - 1); showToast("Previous"); }
            }
            else if (mx >= cx0 + S(36) && mx <= cx0 + S(64)) {     // next
                int idx = playlistIndexOf(g_mpv->path());
                if (idx >= 0 && idx + 1 < (int)g_playlist.size()) {
                    playIndex(idx + 1); showToast("Next");
                } else {
                    showToast("No next track");
                }
            }
            else if (mx >= cx0 - S(21) && mx <= cx0 + S(21)) {     // play/pause
                g_mpv->togglePause();
            }
            // 其余空隙不处理
        }
        // --- 播放列表面板区域：按下记候选（拖拽排序 / 松手播放） ---
        else if (g_ui.playlistOpen && mx >= g_ui.winW - S(320)) {
            int panelY = S(ui::TOPBAR_H);
            int itemH = S(52);
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
        // 列表拖拽落位 / 单击播放
        if (g_ui.plDragFrom >= 0) {
            if (g_ui.plDragging) {
                int itemH = S(52);
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
        if (g_ui.playlistOpen && pt.x >= g_ui.winW - S(320)) {
            int panelH = g_ui.winH - S(ui::TOPBAR_H);
            int contentH = (int)g_playlist.size() * S(52);
            int viewH = panelH - S(55);
            int step = S(52) * 2;
            g_ui.playlistScroll -= (d > 0 ? step : -step);
            if (g_ui.playlistScroll < 0) g_ui.playlistScroll = 0;
            if (contentH > viewH && g_ui.playlistScroll > contentH - viewH)
                g_ui.playlistScroll = contentH - viewH;
            else if (contentH <= viewH) g_ui.playlistScroll = 0;
        }
        else if (g_mpv) {
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
static const Uint8 TRANSPARENT_R = 255;
static const Uint8 TRANSPARENT_G = 0;
static const Uint8 TRANSPARENT_B = 255;

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

static void destroyOverlay() {
    g_text.shutdown();
    svgicon::shutdown();
    if (g_sdlRdr) { SDL_DestroyRenderer(g_sdlRdr); g_sdlRdr = nullptr; }
    if (g_sdlWin) { SDL_DestroyWindow(g_sdlWin);   g_sdlWin = nullptr; }  // 连同 HWND 一起销毁
    g_overlayHwnd = nullptr;
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

    uploadThumbs(g_sdlRdr);   // 惰性上传就绪的缩略图纹理

    SDL_SetRenderDrawColor(g_sdlRdr, TRANSPARENT_R, TRANSPARENT_G, TRANSPARENT_B, 255);
    SDL_RenderClear(g_sdlRdr);

    int w = g_ui.winW, h = g_ui.winH;

    if (!g_mpv || !g_mpv->hasMedia()) {
        // --- welcome page ---
        int w = g_ui.winW, h = g_ui.winH;

        // topbar still visible
        drawGradientBar(g_sdlRdr, 0, 0, w, S(ui::TOPBAR_H), 11, 11, 11, 220, 0);
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
                std::string fn = std::filesystem::path(g_playlist[pi]).filename().string();
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
    double pos = g_ui.seekingDrag ? g_ui.seekTarget : g_mpv->clock();

    // --- topbar (gradient opaque->transparent from top) ---
    {
        drawGradientBar(g_sdlRdr, 0, 0, w, S(ui::TOPBAR_H), 11, 11, 11, 220, 0);

        // title (left)
        std::string title = g_mpv->title();
        if (title.empty()) title = "VPlayer";
        if (title.size() > 55) title = title.substr(0, 52) + "...";
        g_text.drawText(S(20), S(14), title, 14, 255, 255, 255);

        // icons (right) - same order as design mockup
        int iconY = S(ui::TOPBAR_H) / 2;
        int rx = w - S(20);
        svgicon::draw(g_sdlRdr, "close",    rx, iconY, S(20), 255, 255, 255, 200); rx -= S(34);
        svgicon::draw(g_sdlRdr, "maximize", rx, iconY, S(20), 161, 161, 166, 200); rx -= S(34);
        svgicon::draw(g_sdlRdr, "minimize", rx, iconY, S(20), 161, 161, 166, 200); rx -= S(34);
        svgicon::draw(g_sdlRdr, "list",     rx, iconY, S(20), 161, 161, 166, 200); rx -= S(34);
        svgicon::draw(g_sdlRdr, "pip",      rx, iconY, S(20),
            g_ui.miniMode ? 37 : 161, g_ui.miniMode ? 99 : 161,
            g_ui.miniMode ? 235 : 166, 200); rx -= S(34);
        svgicon::draw(g_sdlRdr, "camera",   rx, iconY, S(20), 161, 161, 166, 200);
    }

    int barTop = sbTopY();

    // --- gradient background (top transparent -> bottom opaque) ---
    drawGradientBar(g_sdlRdr, 0, barTop, w, S(60), 11, 11, 11, 0, 220);
    // solid bottom portion
    SDL_Rect solidRc = {0, barTop + S(60), w, S(CONTROL_BAR_H) - S(60)};
    SDL_SetRenderDrawColor(g_sdlRdr, 11, 11, 11, 240);
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

        // thumb (on hover or drag)
        if (g_ui.seekbarHover || g_ui.seekingDrag) {
            int cx = tx + progW;
            int cy = ty + th / 2;
            int r = S(ui::SEEKTHUMB_D) / 2;
            SDL_Rect tRc = {cx - r, cy - r, S(ui::SEEKTHUMB_D), S(ui::SEEKTHUMB_D)};
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
            SDL_RenderFillRect(g_sdlRdr, &tRc);
        }
    }

    // --- transport row: centered prev/play/next ---
    {
        int cy = barTop + S(50);
        svgicon::draw(g_sdlRdr, "prev", w / 2 - S(50), cy, S(20), 161, 161, 166, 200);
        const char* icon = (g_mpv->state() == MpvBackend::State::Paused) ? "play" : "pause";
        svgicon::draw(g_sdlRdr, icon, w / 2, cy, S(ui::PLAYBTN_SIZE), 255, 255, 255, 255);
        svgicon::draw(g_sdlRdr, "next", w / 2 + S(50), cy, S(20), 161, 161, 166, 200);
    }

    // --- left side: time + title ---
    {
        char cur[32], tot[32], ts[80];
        formatTime(cur, sizeof(cur), pos);
        formatTime(tot, sizeof(tot), dur);
        std::snprintf(ts, sizeof(ts), "%s / %s", cur, tot);
        g_text.drawText(S(20), barTop + S(38), ts, 14, 161, 161, 166);
    }
    {
        std::string title = g_mpv->title();
        if (title.empty()) title = std::filesystem::path(g_mpv->path()).filename().string();
        if (title.size() > 50) title = title.substr(0, 47) + "...";
        g_text.drawText(S(20), barTop + S(60), title, 13, 255, 255, 255);
    }

    // --- right side: HW badge + speed + cc + gear + volume + fullscreen ---
    // 布局基准(自右缘): 全屏@w-S(20) 音量@w-S(54) 齿轮@w-S(88) 字幕cc@w-S(122)
    // 速度文本左锚@w-S(160) HW徽标@w-S(204)；命中区与此一一对应
    {
        int cyI = barTop + S(50);
        const char* fid = g_ui.fullscreen ? "exitfull" : "full";
        svgicon::draw(g_sdlRdr, fid, w - S(20), cyI, S(20), 161, 161, 166, 200);
        const char* vid = g_mpv->muted() ? "mute" : "volume";
        svgicon::draw(g_sdlRdr, vid, w - S(54), cyI, S(20), 161, 161, 166, 200);
        svgicon::draw(g_sdlRdr, "gear", w - S(88), cyI, S(20), 161, 161, 166, 200);
        {
            // 字幕图标：可见=亮，隐藏=暗
            Uint8 ca = g_mpv->subVisible() ? 230 : 90;
            svgicon::draw(g_sdlRdr, "cc", w - S(122), cyI, S(20), 255, 255, 255, ca);
        }
        {
            char spd[16];
            float s = g_mpv->speed();
            if (s == (int)s) std::snprintf(spd, sizeof(spd), "%.0fx", s);
            else             std::snprintf(spd, sizeof(spd), "%.1fx", s);
            g_text.drawText(w - S(166), barTop + S(42), spd, 12, 161, 161, 166);
        }
        if (g_mpv->hwDecodeActive()) {
            g_text.drawText(w - S(204), barTop + S(42), "[HW]", 11, 37, 99, 235);
        }
    }

    // --- buffering indicator ---
    if (g_mpv->bufferFill() < 0.5) {
        g_text.drawText(w / 2 - S(30), barTop + S(75), "Buffering...", 12, 161, 161, 166);
    }

    // --- speed popup menu ---
    if (g_ui.speedMenuOpen) {
        int menuW = S(80), itemH = S(30);
        int menuH = SPEED_PRESET_COUNT * itemH;
        int menuX = w - S(150);
        int menuY = barTop - menuH - S(5);

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
            g_text.drawText(menuX + S(20), iy + S(7), label, 13, 255, 255, 255);
        }
    }

    // --- volume slider (appears left of volume icon) ---
    if (g_ui.volumeSliderOpen || g_ui.volumeDragging) {
        int sliderW = S(ui::VOLSIDER_W);
        int sliderH = S(4);
        int volIconX = w - S(54);
        int sliderX = volIconX - sliderW - S(10);
        int sliderY = barTop + S(50) - sliderH / 2;

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
        int thumbR = S(5);
        SDL_Rect tRc = {thumbX - thumbR, sliderY + sliderH/2 - thumbR, thumbR*2, thumbR*2};
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
        SDL_RenderFillRect(g_sdlRdr, &tRc);

        // volume percentage
        char vStr[16];
        std::snprintf(vStr, sizeof(vStr), "%d%%", (int)(vol * 100));
        g_text.drawText(sliderX + sliderW/2 - S(12), sliderY - S(18), vStr, 11, 161, 161, 166);
    }

    // --- playlist panel (right side) ---
    if (g_ui.playlistOpen) {
        int panelW = S(320);
        int panelX = w - panelW;
        int panelH = h - S(ui::TOPBAR_H);
        int panelY = S(ui::TOPBAR_H);

        // panel background
        SDL_Rect pRc = {panelX, panelY, panelW, panelH};
        SDL_SetRenderDrawColor(g_sdlRdr, 18, 18, 18, 245);
        SDL_RenderFillRect(g_sdlRdr, &pRc);
        // left border
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 20);
        SDL_RenderDrawLine(g_sdlRdr, panelX, panelY, panelX, panelY + panelH);

        // title
        g_text.drawText(panelX + S(16), panelY + S(14), "Playlist", 15, 255, 255, 255);

        // items from playlist queue（含滚动 + 缩略图）
        int itemY = panelY + S(45);
        int itemH = S(52);
        int scroll = g_ui.playlistScroll;
        std::vector<std::string> visiblePaths;
        for (size_t pi = 0; pi < g_playlist.size(); ++pi) {
            int iy = itemY + (int)pi * itemH - scroll;
            if (iy + itemH < itemY - S(40)) continue;          // 在视口上方
            if (iy >= panelY + panelH - S(10)) break;          // 到达底部
            const std::string& p = g_playlist[pi];
            visiblePaths.push_back(p);
            bool isCurrent = (g_mpv && g_mpv->path() == p);

            // 缩略图占位/图像
            SDL_Rect thRc = {panelX + S(8), iy + S(5), S(72), S(40)};
            auto texIt = g_thumbTex.find(p);
            if (texIt != g_thumbTex.end()) {
                SDL_RenderCopy(g_sdlRdr, texIt->second, nullptr, &thRc);
            } else {
                SDL_SetRenderDrawColor(g_sdlRdr, 30, 30, 32, 255);
                SDL_RenderFillRect(g_sdlRdr, &thRc);
            }
            if (isCurrent) {
                SDL_SetRenderDrawColor(g_sdlRdr, 37, 99, 235, 255);
                SDL_RenderDrawRect(g_sdlRdr, &thRc);
            }

            // 当前项高亮条
            if (isCurrent) {
                SDL_Rect hlRc = {panelX + S(4), iy - S(2), panelW - S(8), itemH};
                SDL_SetRenderDrawColor(g_sdlRdr, 37, 99, 235, 60);
                SDL_RenderFillRect(g_sdlRdr, &hlRc);
            }

            // file name
            std::string fn = std::filesystem::path(p).filename().string();
            if (fn.size() > 26) fn = fn.substr(0, 23) + "...";
            g_text.drawText(panelX + S(88), iy + S(4), fn, 12,
                isCurrent ? 255 : 200, isCurrent ? 255 : 200, isCurrent ? 255 : 200);

            // last position
            double hpos = 0;
            auto hit = g_cfg.history.find(p);
            if (hit != g_cfg.history.end()) hpos = hit->second;
            if (hpos > 1.0) {
                char posBuf[16];
                formatTime(posBuf, sizeof(posBuf), hpos);
                g_text.drawText(panelX + S(88), iy + S(24), posBuf, 10, 100, 100, 100);
            }

            // playing indicator
            if (isCurrent) {
                const char* icon = (g_mpv->state() == MpvBackend::State::Paused) ? "play" : "pause";
                svgicon::draw(g_sdlRdr, icon, panelX + panelW - S(24), iy + itemH / 2, S(14), 37, 99, 235, 255);
            }
        }

        // 拖拽排序视觉反馈：插入指示线 + 被拖项高亮
        if (g_ui.plDragging && g_ui.plDragFrom >= 0) {
            int itemH = S(52);
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

        // scrollbar
        {
            int contentH = (int)g_playlist.size() * itemH;
            int viewH = panelH - S(55);
            if (contentH > viewH && contentH > 0) {
                int trackX = panelX + panelW - S(5);
                int trackY = panelY + S(45);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 15);
                SDL_Rect trk = {trackX, trackY, S(3), viewH};
                SDL_RenderFillRect(g_sdlRdr, &trk);
                int barH = std::max(S(30), viewH * viewH / contentH);
                int barY = trackY + g_ui.playlistScroll * viewH / contentH;
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 70);
                SDL_Rect br = {trackX, barY, S(3), barH};
                SDL_RenderFillRect(g_sdlRdr, &br);
            }
        }

        if (g_playlist.empty()) {
            g_text.drawText(panelX + S(16), itemY + S(10), "No files in playlist", 12, 100, 100, 100);
        }
    }

    // --- settings modal panel ---
    if (g_ui.settingsOpen) {
        SettingsGeom sg = settingsGeom(w, h);

        // semi-transparent backdrop
        SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 180);
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
            g_cfg.resume, g_cfg.subAutoLoad, g_cfg.thumbCache };
        const char* rowLabels[SET_ROW_COUNT] = {
            "Hardware Decode",
            "Volume Normalization",
            "Resume Playback",
            "Subtitle Auto-Load",
            "Thumbnail Disk Cache",
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
            g_text.drawText(w / 2 - S(40), S(70), g_ui.toastMsg, 13, 255, 255, 255);
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
    mpvSetOpt("audio-filters", g_cfg.volNorm ? "loudnorm" : "");

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
    g_thumbQuit.store(false);
    g_thumbThread = std::thread(thumbWorkerMain);

    // ---- main loop ----
    bool running = true;
    Uint32 lastPosSave = 0;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        // 周期保存播放进度（3 秒）
        Uint32 now = SDL_GetTicks();
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

        if (g_ui.visible && SDL_GetTicks() > g_ui.hideAt)
            g_ui.visible = false;

        // 音量滑条超时自动收起（鼠标静止时无 MOUSEMOVE）
        if (g_ui.volumeSliderOpen && !g_ui.volumeDragging &&
            SDL_GetTicks() > g_ui.volHoverAt + 1200) {
            g_ui.volumeSliderOpen = false;
            LOG_DBG("MAIN", "volume slider auto-collapse (idle)");
        }

        renderOverlay();
        Sleep(1);
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
