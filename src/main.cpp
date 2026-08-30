#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_syswm.h>

const char* PHANTOM_VERSION = "0.1.0";

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
#include "app/app_state.h"
#include "ui/helpers.h"
#include "ui/dialogs.h"
#include "ui/primitives.h"
#include "ui/gradient.h"
#include "ui/ulw.h"
#include "ui/wndproc.h"
#include "ui/render_overlay.h"

// ---- helpers (declarations in app/app_state.h, impl in ui/helpers.cpp) ----
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

// ---- UI state (defined in app/app_state.h) ----

// ---- globals (declared extern in app/app_state.h) ----
HWND              g_parentHwnd = nullptr;
HWND              g_mpvHwnd    = nullptr;
HWND              g_overlayHwnd = nullptr;

std::atomic<bool> g_dirty{ true };
MpvBackend*       g_mpv        = nullptr;
SDL_Window*       g_sdlWin     = nullptr;
SDL_Renderer*     g_sdlRdr     = nullptr;
GdiTextCache      g_text;
UiState           g_ui;
AppConfig         g_cfg;
float             g_dpi = 1.0f;
float             g_uiBase = 1.0f;
std::vector<std::string> g_playlist;

// ---- i18n (defined in app/app_state.h) ----
const char* T(const char* zh, const char* en) {
    return g_cfg.lang == 0 ? zh : en;
}

// ---- mpv �Ӵ������/������Ϣ�м� ----
// overlay(WS_EX_TRANSPARENT) ��������� mpv �� STATIC �Ӵ��ڶ��� parent��
// ����������꽻��ʧЧ���˴�����������Ϣת���� parent ͳһ������
// mpv �Ӵ����� parent �ͻ�����ȫ�غ�(0,0)��lParam �ͻ������ֱ��͸����
static WNDPROC g_mpvOldProc = nullptr;
static LRESULT CALLBACK mpvRelayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
        SetFocus(g_parentHwnd);   // ���̽����ջ� parent
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
const float SPEED_PRESETS[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
const int SPEED_PRESET_COUNT = 8;
const int TIMER_SINGLECLICK = 2;   // ������ͣ�ӳٶ�ʱ��

const QualityPreset QUALITY_PRESETS[] = {
    { "省电",  "bilinear", "bilinear", "bilinear", 0, 0.0f },
    { "标准",  "spline36", "mitchell", "spline36", 1, 0.7f },
    { "卓越",  "ewa_lanczossharp", "ewa_lanczossharp", "ewa_lanczossharp", 1, 0.7f },
};
const int QUALITY_PRESET_COUNT = 3;

void applyQualityPreset(int idx) {
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

// ---- ͳһ UI ���� (U/Tpt defined in app/app_state.h) ----

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

// ---- ����λ�ñ��棨���ڴ�������ǰ���ã� ----
void saveWindowPos(HWND hwnd) {
    if (!IsWindow(hwnd)) return;
    if (g_ui.fullscreen || g_ui.miniMode || IsIconic(hwnd) || IsZoomed(hwnd)) return;
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return;
    // ����: �쳣�ߴ粻���
    if (wr.right - wr.left < U(300) || wr.bottom - wr.top < U(200)) return;
    int w = wr.right - wr.left;
    // �б�չ��ʱ���ں� +U(430) ��չ��, ����۳�, �����´�������������
    if (g_ui.playlistOpen && !g_ui.fullscreen) w -= U(430);
    g_cfg.posX = wr.left; g_cfg.posY = wr.top;
    g_cfg.posW = w; g_cfg.posH = wr.bottom - wr.top;
}

void layoutRow1(int w, int barTopY, bool volOpen, Row1Layout& L) {
    const int pad = U(16);
    const int iconSz = U(34);
    const int playSz = U(42);
    int cy = barTopY + U(50);
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
    x += U(10) + g_text.measureText(sample, Tpt(12)) + U(12);

    // �Ҳ���: ��������
    int xr = w - pad;
    auto placeRight = [&](SDL_Rect& rc, int bw) {
        xr -= bw;
        rc = {xr, cy - iconSz / 2, bw, iconSz};
        xr -= U(4);  // ��ť���
    };
    placeRight(L.fullBtn, iconSz);
    placeRight(L.setBtn, g_text.measureText(i18n::settings(), Tpt(12)) + U(26));
    placeRight(L.qualityBtn, g_text.measureText(i18n::quality(), Tpt(12)) + g_text.measureText(qualityLabel(), Tpt(11)) + U(22));
    {
        char spd[16];
        float s = g_mpv ? g_mpv->speed() : 1.0f;
        if (s == (int)s) std::snprintf(spd, sizeof(spd), "%.0fx", s);
        else             std::snprintf(spd, sizeof(spd), "%.2fx", s);
        placeRight(L.speedBtn, g_text.measureText(spd, Tpt(12)) + g_text.measureText(i18n::speed(), Tpt(12)) + U(24));
    }
    placeRight(L.subBtn, g_text.measureText(i18n::subtitles(), Tpt(12)) + U(26));
    placeRight(L.audioBtn, g_text.measureText(i18n::audioTrack(), Tpt(12)) + U(26));
    placeRight(L.chapterBtn, g_text.measureText(i18n::chapName(), Tpt(12)) + U(26));
    // ����: �ȷŻ���(չ��̬), �ٷ�ͼ��; ������ͼ���Ҳ�
    L.volSliderW = volOpen ? U(80) : 0;
    if (volOpen) {
        xr -= U(80);
        L.volSliderX = xr;
        xr -= U(4);  // ������ͼ����
    }
    L.volIconCx = xr - U(17);
    xr -= U(34);
}

void showToast(const char* msg) {
    std::snprintf(g_ui.toastMsg, sizeof(g_ui.toastMsg), "%s", msg);
    g_ui.toastActive = true;
    g_ui.toastStart = SDL_GetTicks();
}

const char* qualityLabel() {
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

// ---- OSD/mpv property query ----

static void mpvSetOpt(const char* prop, const char* val) {
    if (!g_mpv || !g_mpv->mpv()) return;
    int r = mpv_set_property_string(g_mpv->mpv(), prop, val);
    LOG_DBG("MAIN", "set %s=%s ret=%d", prop, val, r);
}

// �� volNorm/nightMode �ؽ� af �˾���
static void rebuildAudioFilters() {
    std::string af;
    if (g_cfg.volNorm) af += "loudnorm";
    if (g_cfg.nightMode) {
        if (!af.empty()) af += ",";
        af += "@night:acompressor=threshold=-25dB:ratio=6";
    }
    mpvSetOpt("af", af.c_str());
}

// �˶���ֵ��display-resample ʱ�� + oversample���Ϳ���ȥ judder��
static void applyMotionInterp(bool on) {
    mpvSetOpt("video-sync", on ? "display-resample" : "audio");
    mpvSetOpt("interpolation", on ? "yes" : "no");
    if (on) mpvSetOpt("tscale", "oversample");
}

const int SET_ROW_COUNT = 9;

SettingsGeom settingsGeom(int w, int h) {
    SettingsGeom g;
    g.panelW = U(400); g.panelH = U(520);
    g.panelX = (w - g.panelW) / 2;
    g.panelY = (h - g.panelH) / 2;
    // С����: ��御������̧���������Ϸ�, �ײ��в����ڵ�
    {
        int limit = h - curCtrlH() - U(12);
        if (g.panelY + g.panelH > limit)
            g.panelY = std::max(U(6), limit - g.panelH);
    }
    g.closeCx = g.panelX + g.panelW - U(22);
    g.closeCy = g.panelY + U(22);
    g.closeR = U(18);
    g.swX = g.panelX + g.panelW - U(60);
    g.swW = U(40); g.swH = U(20);
    for (int i = 0; i < SET_ROW_COUNT; ++i)
        g.rowY[i] = g.panelY + U(55) + i * U(40);
    // ģʽ�з������һ�п����·�, �뿪�������������������ص�
    g.modeRowY = g.rowY[SET_ROW_COUNT - 1] + U(36);
    g.chipY = g.modeRowY;
    g.chipH = U(24); g.chipW = U(56);
    // ������: �ڲ���ģʽ�·�; �ֶμӿ���֤ "English" �����
    g.langRowY = g.modeRowY + U(40);
    g.langSegW = U(150);
    g.langSegX = g.swX - g.langSegW;
    g.langSegH = U(24);
    return g;
}

// FILE_LOADED 恢复 seek 位+ unpause 标志 (applySetting 里 excl 需要提前使用)
static double g_pendingResumePos = -1.0;
static bool g_needsUnpause = false;
static std::atomic<bool> g_suppressNextUnpause{false};  // exclusive toggle: 保持暂停状态

// Ӧ�����ñ���� mpv�����ط�תʱ���ã�
void applySetting(const char* key, int value) {
    if      (std::strcmp(key, "hw") == 0)      mpvSetOpt("hwdec", value ? (g_cfg.enableZeroCopy ? "auto-safe" : "auto-copy-safe") : "no");
    else if (std::strcmp(key, "sub") == 0)     mpvSetOpt("sub-auto", value ? "fuzzy" : "no");
    else if (std::strcmp(key, "excl") == 0) {
        g_cfg.audioExclusive = (value != 0);
        if (g_mpv) {
            auto snap = g_mpv->reinit(g_mpvHwnd, g_cfg.enableZeroCopy != 0);
            mpvSetOpt("audio-exclusive", value ? "yes" : "no");
            if (!snap.path.empty() && snap.pos > 1.0) {
                g_pendingResumePos = snap.pos;
                if (snap.wasPaused) g_suppressNextUnpause = true;
                g_mpv->loadFile(snap.path);
            }
        }
        showToast(value ? i18n::exclusiveAudio() : i18n::exclusiveAudio());
    }
    else if (std::strcmp(key, "vol") == 0 ||
             std::strcmp(key, "night") == 0)   rebuildAudioFilters();
    else if (std::strcmp(key, "interp") == 0)  applyMotionInterp(value != 0);
    else if (std::strcmp(key, "hiq") == 0) {
        const char* s = value ? "ewa_lanczossharp" : "spline36";
        mpvSetOpt("scale", s);
        mpvSetOpt("cscale", s);
    }
    // thumbCache/resume �����أ�����֪ͨ mpv
}

// ---- overlay z �� ----
void raiseOverlayAbove() {
    if (!g_overlayHwnd) return;
    // parent �� TOPMOST/��ԭ�󣬰� overlay �����ᵽͬ������
    SetWindowPos(g_overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// �����б���/�� -> �����ڿ������� U(430)���Ҳ�������򣬲��ڵ���Ƶ��
// ע��: �ޱ߿򴰿ڵ� GetWindowRect �� client ������ر߿�(~18px),
// ������ client ��������� window ����, �����б�������Ư��
void applyPlaylistWindow(HWND hwnd) {
    RECT rc, wr;
    GetClientRect(hwnd, &rc);
    GetWindowRect(hwnd, &wr);
    int frameW = (wr.right - wr.left) - (rc.right - rc.left);
    int frameH = (wr.bottom - wr.top) - (rc.bottom - rc.top);
    int newWinW = (rc.right - rc.left) + (g_ui.playlistOpen ? U(430) : -U(430)) + frameW;
    SetWindowPos(hwnd, nullptr, 0, 0, newWinW,
                 (rc.bottom - rc.top) + frameH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // WM_SIZE �а� playlistOpen ������Ƶ��/�б���
}

// ---- ȫ���л����ޱ߿򴰿ڣ����ƶ���������ʾ���ߴ磬������ʽ�� ----
void toggleFullscreen(HWND hwnd) {
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

void toggleMini(HWND hwnd) {
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

// ---- ���Ŷ��У��ȶ�˳���ļ���ɨ�����ɣ����沥�����ţ� ----

void clampPlaylistScroll() {
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

// ��Ȼ����: ���ֶΰ���ֵ�Ƚ�(V2<V10), �����ֶΰ����; ��Ϊ������Դ������
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
            size_t zi = i, zj = j;                       // ��ǰ����
            while (zi + 1 < ie && a[zi] == L'0') ++zi;
            while (zj + 1 < je && b[zj] == L'0') ++zj;
            if (ie - zi != je - zj) return ie - zi < je - zj;   // ��ֵ����
            int c = a.compare(zi, ie - zi, b, zj, je - zj);
            if (c != 0) return c < 0;
            // ��ֵ���: ǰ����������ǰ(01<001)
            if (ie - i != je - j) return ie - i < je - j;
            i = ie; j = je;
        } else {
            wchar_t la = ca, lb = cb;
            if (la >= L'A' && la <= L'Z') la += 32;
            if (lb >= L'A' && lb <= L'Z') lb += 32;
            if (la != lb) return la < lb;
            if (ca != cb) return ca < cb;                // ��Сд�ȶ���
            ++i; ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

// ɨ��Ŀ¼����Ƶ�ļ� (��Ȼ����, UTF-8 ·��) �� buildPlaylistAround/FromFolder ����
static std::vector<std::string> scanVideoDirUtf8(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    std::vector<fs::path> found;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (found.size() >= PLAYLIST_MAX) break;
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().string();   // ��չ�� ASCII, ANSI ��ȡ��ȫ
        for (auto* ve : kVideoExts) {
            if (_stricmp(ext.c_str(), ve) == 0) { found.push_back(e.path()); break; }
        }
    }
    std::sort(found.begin(), found.end(), [](const fs::path& x, const fs::path& y) {
        return naturalLess(x.filename().wstring(), y.filename().wstring());
    });
    for (auto& f : found) out.push_back(WideToUtf8(f.wstring()));   // ��->UTF-8
    return out;
}

// �� file ����Ŀ¼ɨ����Ƶ�ļ��������Ŷ��У���Ȼ˳��
void buildPlaylistAround(const std::string& file) {
    namespace fs = std::filesystem;
    g_playlist.clear();
    fs::path p(Utf8ToWide(file));                       // ���ַ�����, �ž� ANSI ���
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

// M36: ֱ�����ļ��й������Ŷ��� �� ��ӭҳ�����ļ��С���ť
bool buildPlaylistFromFolder(const std::string& dirUtf8) {
    g_playlist = scanVideoDirUtf8(Utf8ToWide(dirUtf8));
    return !g_playlist.empty();
}

// P1-5: �Ӳ����б��Ƴ�ָ�����������Ƴ����ǵ�ǰ���������Զ���ת��һ��
void removeFromPlaylist(int idx);  // forward decl

// P1-5: �����ļ��������б�
void addToPlaylist(const std::string& file) {
    g_playlist.push_back(file);
}

int playlistIndexOf(const std::string& path) {
    for (size_t i = 0; i < g_playlist.size(); ++i)
        if (g_playlist[i] == path) return (int)i;
    return -1;
}

// ---- ����ͼ����worker ����� RGB����Ⱦ�̶߳����ϴ�Ϊ���� ----
#include "core/thumbnail_extractor.h"


// ͳһ������ڣ���¼������λ�� + ���� lastFile + �����浱ǰ��
// EOF �Զ�����: �¼��߳�ֻͶ��, UI ��ѭ������(������߳� mpv/UI ��������)
static std::mutex g_autoNextMtx;
static std::string g_autoNextPath;
static bool g_autoNextPending = false;
static double g_resumeSeekPos = -1.0;      // FILE_LOADED Ͷ�ݵ�����λ��
static bool g_resumeSeekPending = false;
// M36: ͳһ��ʷд�� (pos + dur + ʱ���)
static void recordHistory(const std::string& path, double pos, double dur) {
    if (path.empty()) return;
    HistoryEntry& e = g_cfg.history[path];
    e.pos = pos;
    if (dur > 0) e.dur = dur;
    e.lastPlayed = (long long)std::time(nullptr);
    // P2: ��ʷ���� 500 ��, ��̭��ɵ�
    static const size_t kMaxHistory = 500;
    if (g_cfg.history.size() > kMaxHistory) {
        // �� lastPlayed ��С����Ŀ��̭
        auto oldest = g_cfg.history.begin();
        for (auto it = g_cfg.history.begin(); it != g_cfg.history.end(); ++it) {
            if (it->second.lastPlayed < oldest->second.lastPlayed)
                oldest = it;
        }
        g_cfg.history.erase(oldest);
    }
}
void playPath(const std::string& path, bool forceResume) {
    if (!g_mpv || path.empty()) return;
    g_pendingResumePos = -1.0;
    auto it = g_cfg.history.find(path);
    bool found = (forceResume || g_cfg.resume) && it != g_cfg.history.end() && it->second.pos > 1.0;
    if (found) g_pendingResumePos = it->second.pos;
    LOG_INFO("MAIN", "playPath: force=%d resume_cfg=%d found=%d pos=%.1f path=%s",
             forceResume, g_cfg.resume, found, g_pendingResumePos, path.c_str());
    if (!g_mpv->loadFile(path)) {
        showToast(i18n::failedOpen());
        return;
    }
    // g_needsUnpause 移到 onFileLoaded 回调中设置，
    // 避免竞态：主循环在 onFileLoaded 之前醒来 → 无 seek 位就 unpause → 从头播放
    g_cfg.lastFile = path;

    // �����б�����ʱ����������ǰ���
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

void playIndex(int idx, bool relative) {
    if (relative) {
        int cur = playlistIndexOf(g_mpv ? g_mpv->path() : "");
        if (cur < 0) cur = 0;
        idx = cur + idx;
    }
    if (idx < 0 || idx >= (int)g_playlist.size()) return;
    playPath(g_playlist[idx]);
}

// P1-5: �Ӳ����б��Ƴ�ָ�����������Ƴ����ǵ�ǰ���������Զ���ת��һ��
void removeFromPlaylist(int idx) {
    if (idx < 0 || idx >= (int)g_playlist.size()) return;
    std::string curPath = g_mpv ? g_mpv->path() : "";
    bool isCurrent = (g_playlist[idx] == curPath);
    g_playlist.erase(g_playlist.begin() + idx);
    if (g_playlist.empty()) return;
    if (isCurrent) {
        int newIdx = idx < (int)g_playlist.size() ? idx : 0;
        playPath(g_playlist[newIdx]);
    }
}


// ---- ������������ͼ��+������Χ, �ö�̬���֣� ----
bool inVolumeArea(int mx, int my) {
    Row1Layout L;
    layoutRow1(g_ui.totalW > 0 ? g_ui.totalW : g_ui.winW, sbTopY(), true, L);
    int barTop = sbTopY();
    int row1Off = U(50);
    int cy = barTop + row1Off;
    // Y: ������ �� 24px
    if (my < cy - U(24) || my > cy + U(24)) return false;
    // X: ������ͼ����Ե-8 ��������Ե+8
    int x0 = L.volIconCx - U(25);
    int x1 = L.volSliderW > 0 ? L.volSliderX + L.volSliderW + U(8) : L.volIconCx + U(25);
    return mx >= x0 && mx <= x1;
}

// ---- topbar icon hit test ----
int hitTestTopbarIcon(int mx, int my, int winW) {
    if (my < 0 || my > curTopH()) return -1;
    int iconY = curTopH() / 2;
    int iconHalf = U(21);
    int rx = winW - U(20);
    int icoSp = U(42);
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

// ---- Win32 WndProc (extracted to ui/wndproc.cpp) ----
// Forward declaration; implementation lives in wndproc.o.
// static removed; linked from ui/wndproc.cpp.

// P2: overlay �������� �� �� WM_NCHITTEST ���� HTTRANSPARENT, ʹ�����Ϣ��͸�� parent
static WNDPROC s_origOverlayWndProc = nullptr;
static LRESULT CALLBACK overlaySubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE:
        LOG_DBG("MAIN", "overlay subclass msg=0x%04X wp=0x%zX lp=(%d,%d)",
                msg, (size_t)wp, (short)LOWORD(lp), (short)HIWORD(lp));
        break;
    default:
        break;
    }
    if (msg == WM_NCHITTEST) {
        LOG_DBG("MAIN", "overlay WM_NCHITTEST -> HTTRANSPARENT");
        return HTTRANSPARENT;
    }
    return CallWindowProcW(s_origOverlayWndProc, h, msg, wp, lp);
}

// ---- SDL2 overlay ----
// ������ alpha �ϳ�(UpdateLayeredWindow): ��Ⱦ��� ReadPixels ��Ԥ�� alpha,
// �� ULW_ALPHA ������֧�����͸��(��������/ѹ������/ģ̬����), �� colorkey
// ��ֵ͸���Ķ���αװ�������͸���� WS_EX_TRANSPARENT ��֤��
static const Uint8 TRANSPARENT_R = 0;
static const Uint8 TRANSPARENT_G = 0;
static const Uint8 TRANSPARENT_B = 0;

static bool createOverlay(HWND parent, int w, int h) {
    // �����ޱ߿򴰿ڣ���ϵͳ��֧�� WS_EX_LAYERED �Ӵ��ڣ�ʵ�� err=87��
    // ͨ�� OWNER ���� + TOOLWINDOW ���������ڣ�����������/Alt+Tab���������ڹر�
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
    g_overlayHwnd = ov;   // �� mini ģʽ z �����ʹ��

    LONG_PTR ex = GetWindowLongPtrW(ov, GWL_EXSTYLE);
    SetWindowLongPtrW(ov, GWL_EXSTYLE,
        ex | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW);
    LOG_INFO("MAIN", "overlay HWND=%p exStyle=0x%zX (LAYERED|TRANSPARENT|TOOLWINDOW applied)", (void*)ov, ex | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW);
    // ����ʹ�� LWA_COLORKEY: ���� UpdateLayeredWindow �ṩ������ alpha

    // ��Ϊ parent �� Owned ���ڣ��ö��ڸ�������С��ʱ�������޶�����������
    SetWindowLongPtrW(ov, GWLP_HWNDPARENT, (LONG_PTR)parent);
    SetWindowPos(ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // ������������Ⱦ��: D3D11 �������󱸻��岻���� alpha(���غ�Ϊ 255),
    // ���� ULW ����䲻͸���ڰ塣������Ⱦ���󱸻������� alpha Surface��
    // UI �����ػ�, ������Ⱦ�����㹻��
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
    SetWindowPos(ov, nullptr, pt.x, pt.y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    LOG_INFO("MAIN", "overlay pos: screen(%d,%d) size(%d,%d)", pt.x, pt.y, w, h);

    // P2: ���໯ overlay ���ڣ��� WM_NCHITTEST ���� HTTRANSPARENT
    // ��� SDL ���������Ϣ���¹رհ�ť�㲻��������
    s_origOverlayWndProc = (WNDPROC)SetWindowLongPtrW(ov, GWLP_WNDPROC, (LONG_PTR)overlaySubclassProc);
    LOG_INFO("MAIN", "overlay subclass installed on HWND=%p orig=%p", (void*)ov, (void*)s_origOverlayWndProc);

    LOG_INFO("MAIN", "overlay created (%dx%d, owned)", w, h);
    return true;
}



// ---- DPI awareness ----
// 125%+ �����·� aware �������걻���⻯: �������/��ͼ����ȫ����λ,
// ����Ⱦ������ģ����PER_MONITOR_AWARE_V2 ����������ͳһΪ�������ء�
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
    Logger::instance().setLevel(diag ? LogLevel::Trace : LogLevel::Info);
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
wc.style         = CS_DBLCLKS;   // ���� WM_LBUTTONDBLCLK
    wc.lpfnWndProc   = parentProc;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName  = L"PhantomParent";
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    // ��������ǰ��ȡ���� DPI����֤ U(960)xU(540) ����������չ��
    {
        HDC dc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, dc);
        LOG_INFO("MAIN", "initial dpi scale=%.2f", g_dpi);
    }
    // ����λ�����ȣ���Ч��Ĭ�ϳߴ� + ϵͳ����λ��
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
    updateDpiForWindow(g_parentHwnd);   // �Դ���������ʾ��Ϊ׼����
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
    // ��װ�����м̣��� mpvRelayProc ע�ͣ�
    g_mpvOldProc = (WNDPROC)SetWindowLongPtrW(g_mpvHwnd, GWLP_WNDPROC,
                                              (LONG_PTR)mpvRelayProc);

    MpvBackend mpv;
    g_mpv = &mpv;
    mpv.setVolume(g_cfg.volume);
    if (g_cfg.speed >= 0.25f && g_cfg.speed <= 4.0f && std::abs(g_cfg.speed - 1.0f) > 0.01f)
        mpv.setSpeed(g_cfg.speed);
    mpv.onFileLoaded = [&]() {
        double pos = g_pendingResumePos;
        g_pendingResumePos = -1.0;
        LOG_INFO("MPV", "file loaded, pendingResumePos=%.1f", pos);
        std::lock_guard<std::mutex> lk(g_autoNextMtx);
        if (pos > 1.0) {
            g_resumeSeekPos = pos;
            g_resumeSeekPending = true;
        }
        if (!g_suppressNextUnpause) {
            g_needsUnpause = true;
        } else {
            g_suppressNextUnpause = false;
            LOG_INFO("MPV", "suppress next unpause");
        }
    };
    mpv.onPlaybackEnded = [&]() {
        LOG_INFO("MAIN", "playback ended");
        if (!g_mpv) return;
        std::string cur = g_mpv->path();
        if (!cur.empty()) recordHistory(cur, 0, g_mpv->duration());   // ��������
        int idx = playlistIndexOf(cur);
        int n = (int)g_playlist.size();
        if (idx < 0 || n == 0) return;

        // ע��: ���ص��� mpv �¼��߳�ִ�С��˴�������ֱ�ӵ� playPath/
        // showToast��mpv ���� + UI ״̬���� UI �߳��γ���ѭ������,
        // �� g_ui/g_playlist ��������д�����ݾ�������
        // ֻ������һ��·��, Ͷ�ݸ� UI ��ѭ��ִ�С�
        std::string next;
        if (g_cfg.playMode == 2) {                   // Shuffle
            if (n > 1) {
                int pick = idx;
                while (pick == idx) pick = std::rand() % n;
                next = g_playlist[pick];
            }
        } else if (g_cfg.playMode == 1) {            // Loop��˳��ѭ��
            next = g_playlist[(idx + 1) % n];
        }                                            // Single��ͣס(�� next)
        {
            std::lock_guard<std::mutex> lk(g_autoNextMtx);
            g_autoNextPath = next;
            g_autoNextPending = true;
        }
    };
    mpv.onPlaybackError = [&](const char* errorMsg) {
        LOG_ERROR("MAIN", "playback error: %s", errorMsg);
        // 区分网络错误和解码错误
        std::string msg = "Playback error: ";
        msg += errorMsg;
        showToast(msg.c_str());
    };

    if (!mpv.init(g_mpvHwnd, g_cfg.enableZeroCopy != 0)) { LOG_ERROR("MAIN", "mpv init failed"); return 1; }

    // Essential mpv options only (critical for first frame)
    mpvSetOpt("hwdec", g_cfg.hwDecode ? (g_cfg.enableZeroCopy ? "auto-safe" : "auto-copy-safe") : "no");
    mpvSetOpt("sub-auto", g_cfg.subAutoLoad ? "fuzzy" : "no");

    // ---- SDL2 overlay ----
    if (!createOverlay(g_parentHwnd, rc.right, rc.bottom)) { return 1; }

    ShowWindow(g_parentHwnd, SW_SHOW);
    UpdateWindow(g_parentHwnd);

    // Deferred mpv options (non-critical, applied after window visible)
    mpvSetOpt("audio-exclusive", g_cfg.audioExclusive ? "yes" : "no");
    rebuildAudioFilters();
    if (g_cfg.motionInterp) applyMotionInterp(true);
    if (g_cfg.hiQScale) {
        mpvSetOpt("scale", "ewa_lanczossharp");
        mpvSetOpt("cscale", "ewa_lanczossharp");
    }

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
        // �ָ��ϴβ��ţ������ؽ���������Ŀ¼
        buildPlaylistAround(initialFile);
        playPath(initialFile);
    } else if (!initialFile.empty()) {
        buildPlaylistAround(initialFile);
        playPath(initialFile);
    }

    LOG_INFO("MAIN", "entering main loop (playlist=%d)", (int)g_playlist.size());

    // Thumb worker: start thread first, cleanup runs inside worker
    CreateDirectoryA((exeDir() + "cache").c_str(), nullptr);
    CreateDirectoryA(thumbCacheDir().c_str(), nullptr);
    g_thumbQuit.store(false);
    g_thumbThread = std::thread(thumbWorkerMain);

    // ---- main loop��������Ⱦ������ + ��ʱ���� + ���������� ----
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
            DispatchMessageW(&msg);   // parentProc ������� dirty
        }
        if (!running) break;

        Uint32 now = SDL_GetTicks();

        // ����״̬��ѯ��������� / ����״̬�л� -> dirty
        if (g_mpv && g_mpv->hasMedia()) {
            double pos = g_mpv->clock();
            if ((int)pos != lastPosSec) { lastPosSec = (int)pos; g_dirty.store(true); }
            auto st = g_mpv->state();
            if (st != lastState) { lastState = st; g_dirty.store(true); }
        }

        // EOF �Զ�����: �����¼��߳�Ͷ�ݵ���һ��(UI �̰߳�ȫ��ִ��)
        {
            std::string next;
            bool fire = false;
            {
                std::lock_guard<std::mutex> lk(g_autoNextMtx);
                fire = g_autoNextPending;
                if (fire) { next = std::move(g_autoNextPath); g_autoNextPending = false; }
            }
            if (fire) {
                if (next.empty()) showToast(i18n::endOfTrack());   // Single ģʽͣס
                else playPath(next);
            }
        }

        // ���� seek + unpause: ���� FILE_LOADED Ͷ�ݵ�λ��
        {
            double pos = -1.0;
            bool unpause = false;
            {
                std::lock_guard<std::mutex> lk(g_autoNextMtx);
                if (g_resumeSeekPending) { pos = g_resumeSeekPos; g_resumeSeekPending = false; }
                if (g_needsUnpause) { unpause = true; g_needsUnpause = false; }
            }
            if (pos > 1.0 && g_mpv && g_mpv->hasMedia()) {
                LOG_INFO("MAIN", "resume at %.1fs", pos);
                g_mpv->seek(pos);
                char msg[48];
                std::snprintf(msg, sizeof(msg), "%s %02d:%02d",
                    T("已续播", "Resumed at"), (int)(pos / 60), (int)pos % 60);
                showToast(msg);
            }
            if (unpause && g_mpv && g_mpv->hasMedia()) {
                g_mpv->unpause();
            }
        }

        // ���ڱ��沥�Ž��ȣ�3 �룩
        if (now - lastPosSave >= 3000) {
            lastPosSave = now;
            if (g_mpv && g_mpv->hasMedia() && g_mpv->state() == MpvBackend::State::Playing) {
                double pos = g_mpv->clock();
                double dur = g_mpv->duration();
                std::string cur = g_mpv->path();
                if (!cur.empty() && dur > 0)
                    recordHistory(cur, (pos < dur - 2.0) ? pos : 0.0, dur);  // ��β��Ϊ����
            }
        }

        // ��ʱ״̬Ǩ�ƣ�Ǩ�ƶ��������� dirty��
        if (g_ui.visible && now > g_ui.hideAt) {
            g_ui.visible = false;
            g_dirty.store(true);
        }

        // �ؼ����뵭����ease-out ������
        {
            float target = g_ui.visible ? 1.0f : 0.0f;
            float cur = g_ui.ctrlAlpha;
            if (std::abs(target - cur) > 0.001f) {
                // ease-out: �ӽ�Ŀ��ʱ����
                float diff = target - cur;
                float step = diff * 0.12f;  // ÿ֡�ƶ�ʣ������12%
                if (std::abs(step) < 0.005f) step = (diff > 0 ? 0.005f : -0.005f);
                cur += step;
                g_ui.ctrlAlpha = cur;
                g_dirty.store(true);
                waitCap = 16;   // �����ڼ��Ƶˢ��
            } else {
                g_ui.ctrlAlpha = target;
            }
        }
        if (g_ui.volumeSliderOpen && !g_ui.volumeDragging &&
            now > g_ui.volHoverAt + 500) {
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

        // ����ʱ��Ⱦ
        if (g_dirty.exchange(false)) {
            renderOverlay();
        }

        // ����������ѵ㣨cap 200ms ��֤�����н��������Ӧ�����и��ã�
        Uint32 wait = 200;
        if (waitCap < wait) wait = waitCap;
        waitCap = 200;
        auto upd = [&](Uint32 deadline) {
            if (deadline > now && deadline - now < wait) wait = deadline - now;
        };
        upd(g_ui.hideAt);
        if (g_ui.volumeSliderOpen) upd(g_ui.volHoverAt + 500);
        if (g_ui.toastActive)      upd(g_ui.toastStart + ui::TOAST_MS + 60);
        if (g_ui.osdActive)        upd(g_ui.osdStart + 8050);
        // AB ѭ�����: ���ŵ� B ��ʱ���� A ��
        if (g_mpv && g_mpv->looping() && !g_ui.seekingDrag) {
            double cur = g_mpv->clock();
            if (cur >= g_mpv->loopB()) {
                g_mpv->seek(g_mpv->loopA());
                g_dirty.store(true);
            }
        }
        // ���ȱ���(3s ����)�� 200ms ������ѯ����, ����ר�ż���

        MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    // �˳�ǰ�������ս��� + ����λ�ã�WM_CLOSE �Ѵ棬�˴����ף�
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