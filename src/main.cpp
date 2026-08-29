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
#include "app/app_state.h"
#include "ui/helpers.h"
#include "ui/dialogs.h"
#include "ui/primitives.h"
#include "ui/gradient.h"
#include "ui/ulw.h"

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
    { "ʡ��",  "bilinear", "bilinear", "bilinear", 0, 0.0f },
    { "��׼",  "spline36", "mitchell", "spline36", 1, 0.7f },
    { "����",  "ewa_lanczossharp", "ewa_lanczossharp", "ewa_lanczossharp", 1, 0.7f },
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

// ---- OSD��mpv ���Բ�ѯ ----
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

// ����ʱд mpv ���ԣ��ַ�����
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

// Ӧ�����ñ���� mpv�����ط�תʱ���ã�
void applySetting(const char* key, int value) {
    if      (std::strcmp(key, "hw") == 0)      mpvSetOpt("hwdec", value ? (g_cfg.enableZeroCopy ? "auto-safe" : "auto-copy-safe") : "no");
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

static std::mutex g_thumbMtx;
static std::vector<std::string> g_thumbWant;                   // ��ǰ�ɼ�����ȡ����
struct ThumbRgb { int w = 0, h = 0; std::vector<uint8_t> px; };
static std::map<std::string, ThumbRgb> g_thumbRgb;              // path -> RGB24(�� px=ʧ�ܱ��)
static std::map<std::string, SDL_Texture*> g_thumbTex;          // ��Ⱦ�߳�ר��
static std::atomic<bool> g_thumbQuit{false};
static std::thread g_thumbThread;

// ---- ����ͼ���̻��棺exe/cache/thumbs/<fnv1a64>.bin = "VPT1"+w+h+RGB24 ----
static std::string thumbCacheDir() {
    return exeDir() + "cache\\thumbs";
}

// ����ʱ�������� keepDays ��Ļ����ļ�
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

    // �ռ������ļ�
    struct ThumbFile { std::string name; ULARGE_INTEGER mtime; };
    std::vector<ThumbFile> files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER ulFile = {{fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}};
        files.push_back({fd.cFileName, ulFile});
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    int removed = 0;
    // 1) ɾ�������ļ�
    for (auto& f : files) {
        long long ageDays = (long long)(ulNow.QuadPart - f.mtime.QuadPart) / (10000000LL * 86400);
        if (ageDays > keepDays) {
            DeleteFileA((dir + "\\" + f.name).c_str());
            f.name.clear();  // �����ɾ
            ++removed;
        }
    }
    // 2) �������� 300: ��ʱ��������̭��ɵ�
    static const int kMaxThumbs = 300;
    files.erase(std::remove_if(files.begin(), files.end(),
        [](const ThumbFile& f) { return f.name.empty(); }), files.end());
    if ((int)files.size() > kMaxThumbs) {
        std::sort(files.begin(), files.end(),
            [](const ThumbFile& a, const ThumbFile& b) {
                return a.mtime.QuadPart < b.mtime.QuadPart;
            });
        int excess = (int)files.size() - kMaxThumbs;
        for (int i = 0; i < excess; ++i) {
            DeleteFileA((dir + "\\" + files[i].name).c_str());
            ++removed;
        }
    }
    if (removed) LOG_INFO("MAIN", "thumb cache cleanup: removed %d files (%zu remaining)",
                          removed, files.size() - (files.size() > 0 ? 0 : 0));
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

// ���з��� true ����� out���ļ�����ɾ��
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
    if (!ok) DeleteFileA(thumbDiskPath(path).c_str());   // �𻵼�ɾ
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
            g_thumbRgb[path] = std::move(out);   // ʧ��Ҳ�ǿձ�ǣ����ⷴ������
        }
    }
}

// ��Ⱦ�̵߳��ã��Ѿ����� RGB ת������
static void uploadThumbs(SDL_Renderer* r) {
    std::lock_guard<std::mutex> lk(g_thumbMtx);
    for (auto it = g_thumbRgb.begin(); it != g_thumbRgb.end(); ) {
        auto& t = it->second;
        if (t.px.empty()) { ++it; continue; }                 // ʧ�ܱ������
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

// ͳһ������ڣ���¼������λ�� + ���� lastFile + �����浱ǰ��
static double g_pendingResumePos = -1.0;   // >0 ��ʾ FILE_LOADED �� seek ����
// EOF �Զ�����: �¼��߳�ֻͶ��, UI ��ѭ������(������߳� mpv/UI ��������)
static std::mutex g_autoNextMtx;
static std::string g_autoNextPath;
static bool g_autoNextPending = false;
static double g_resumeSeekPos = -1.0;      // FILE_LOADED Ͷ�ݵ�����λ��
static bool g_resumeSeekPending = false;
static bool g_needsUnpause = false;        // P4-1: loadFile ���� unpause
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
void playPath(const std::string& path) {
    if (!g_mpv || path.empty()) return;
    g_pendingResumePos = -1.0;
    auto it = g_cfg.history.find(path);
    if (g_cfg.resume && it != g_cfg.history.end() && it->second.pos > 1.0)
        g_pendingResumePos = it->second.pos;
    if (!g_mpv->loadFile(path)) {
        showToast(i18n::failedOpen());
        return;
    }
    g_needsUnpause = true;  // P4-1: loadFile ��ͣ, FILE_LOADED �� unpause
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

// ---- Win32 WndProc ----
static LRESULT CALLBACK parentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    g_dirty.store(true);   // �κ���Ϣ����ΪǱ���Ӿ��仯�����ͳһ���ࣩ
    switch (msg) {

    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0;
        RECT rc; GetClientRect(hwnd, &rc);
        g_ui.totalW = rc.right;
        // �б�����(��ȫ��)ʱ: �Ҳ��������, mpv/overlay ֻռ��Ƶ��
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
            LOG_DBG("MAIN", "overlay repositioned to screen(%d,%d) size(%d,%d)", pt.x, pt.y, rc.right, rc.bottom);
        }
        // ��ʱ�ػ� �� ������ѭ������
        renderOverlay();
        return 0;
    }
    case WM_DPICHANGED: {
        // ��ʾ�� DPI �仯���ϵ���ͬ������/��ϵͳ���ţ�
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

    // ---- �������: ������ʧȥ����ʱ���� overlay, ���⸡�������������� ----
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
            case 0xDB:   // VK_OEM_4: Windows �� [ ������������(��ASCII 0x5B)
            {
                g_mpv->setSpeed(g_mpv->speed() - 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("����", "Speed"), g_mpv->speed());
                showToast(msg);
                break;
            }
            case ']':
            case 0xDD:   // VK_OEM_6
            {
                g_mpv->setSpeed(g_mpv->speed() + 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("����", "Speed"), g_mpv->speed());
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
                std::snprintf(msg, sizeof(msg), "%s: %.1fs", T("��Ļ�ӳ�", "Sub delay"), -g_mpv->subDelay());
                showToast(msg);
                break;
            }
            case 'Z': {
                g_mpv->addSubDelay(0.5);
                char msg[40];
                std::snprintf(msg, sizeof(msg), "%s: %.1fs", T("��Ļ�ӳ�", "Sub delay"), -g_mpv->subDelay());
                showToast(msg);
                break;
            }
            case 'I':
                g_ui.osdActive = !g_ui.osdActive;
                g_ui.osdStart = SDL_GetTicks();
                LOG_DBG("MAIN", "osd -> %d", g_ui.osdActive ? 1 : 0);
                break;
            case 'S':
                if (GetKeyState(VK_SHIFT) & 0x8000) {
                    // Shift+S: �����ⲿ��Ļ�ļ�
                    if (g_mpv && g_mpv->hasMedia()) {
                        std::string f = openSubtitleDialog(hwnd);
                        if (!f.empty()) {
                            g_mpv->loadSubtitle(f);
                            showToast(T("��Ļ�Ѽ���", "Subtitle loaded"));
                        }
                    }
                } else {
                    // S: �л���Ļ�ɼ���
                    bool vis = !g_mpv->subVisible();
                    g_mpv->setSubVisibility(vis);
                    showToast(vis ? i18n::subtitlesOn() : i18n::subtitlesOff());
                }
                break;
            case 'A': {  // AB ѭ��: ��һ���� A, �ڶ����� B, ���������
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
            case 'G': {  // �½���ת: ������һ��
                auto chs = g_mpv->chapters();
                if (!chs.empty()) {
                    int cur = g_mpv->currentChapter();
                    int next = (cur + 1) % (int)chs.size();
                    g_mpv->seekToChapter(next);
                    char msg[64];
                    std::snprintf(msg, sizeof(msg), "%s %d/%d: %s",
                                 T("�½�", "Chapter"),
                                 next + 1, (int)chs.size(),
                                 chs[next].title.empty() ? T("�ޱ���", "Untitled") : chs[next].title.c_str());
                    showToast(msg);
                }
                break;
            }
            case 'V': {  // �����л�: ѭ����һ����
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
                                 T("����", "Audio"),
                                 tracks[nextIdx].desc.empty() ? T("Ĭ��", "Default") : tracks[nextIdx].desc.c_str());
                    showToast(msg);
                } else {
                    showToast(i18n::singleTrack());
                }
                break;
            }
            case 'B': {  // ��Ļλ��: ѭ���ײ�/����/����
                static int subPosIdx = 0;
                int positions[] = {100, 50, 10};
                const char* names[] = {i18n::subBottom(), i18n::subCenter(), i18n::subTop()};
                subPosIdx = (subPosIdx + 1) % 3;
                g_mpv->setSubPos(positions[subPosIdx]);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %s", T("��Ļ", "Sub"), names[subPosIdx]);
                showToast(msg);
                break;
            }
            case 'D': {  // ȥɫ��ǿ��: �ء�����С�ǿ����...
                int cur = g_mpv->debandLevel();
                int next = (cur + 1) % 4;
                g_mpv->setDebandLevel(next);
                const char* names[] = {i18n::debandOff(), i18n::debandLight(), i18n::debandMedium(), i18n::debandStrong()};
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %s", T("ȥɫ��", "Deband"), names[next]);
                showToast(msg);
                break;
            }
            case 'E': {  // ��Ƶ������: ��/�رյ���
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
            case 'U':
                if (GetKeyState(VK_CONTROL) & 0x8000) {
                    std::string url = openUrlDialog(hwnd);
                    if (!url.empty()) {
                        playPath(url);
                    }
                }
                break;
            case VK_INSERT: {
                // Insert: �����ļ��������б�
                if (g_mpv && g_mpv->hasMedia()) {
                    std::string f = openFileDialog(hwnd);
                    if (!f.empty()) {
                        addToPlaylist(f);
                        if (!g_ui.playlistOpen) g_ui.playlistOpen = true;
                        showToast(T("�����ӵ������б�", "Added to playlist"));
                    }
                }
                break;
            }
            case VK_DELETE: {
                // Delete: �Ӳ����б��Ƴ���ǰ��
                if (g_mpv && g_mpv->hasMedia() && g_playlist.size() > 1) {
                    int curIdx = playlistIndexOf(g_mpv->path());
                    if (curIdx >= 0) {
                        std::string fn = fileNameOf(g_playlist[curIdx]);
                        removeFromPlaylist(curIdx);
                        char msg[128];
                        std::snprintf(msg, sizeof(msg), "%s: %s",
                                      T("���Ƴ�", "Removed"), fn.c_str());
                        showToast(msg);
                    }
                }
                break;
            }
            } // switch (wp)
        } // if (g_mpv)
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

        // seekbar ��ק: �������� seekTarget
        if (g_ui.seekingDrag && g_mpv && g_mpv->duration() > 0) {
            double ratio = (double)(g_ui.mouseX - sbLeftX()) / sbWidth();
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_ui.seekTarget = g_mpv->duration() * ratio;
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + 5000;
            g_dirty.store(true);
        }

        bool onTopbar = (g_ui.mouseY >= 0 && g_ui.mouseY <= curTopH());
        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + (onTopbar ? 4000 : ui::CTRLBAR_HIDE_MS);

        // topbar icon hover
        g_ui.topbarHover = onTopbar ? hitTestTopbarIcon(g_ui.mouseX, g_ui.mouseY, g_ui.totalW) : -1;

        // �������� hover �Զ�չ�����뿪 0.5s ��������ק�в��գ�
        if (inVolumeArea(g_ui.mouseX, g_ui.mouseY)) {
            if (!g_ui.volumeSliderOpen) {
                g_ui.volumeSliderOpen = true;
                LOG_DBG("MAIN", "volume slider hover-expand");
            }
            g_ui.volHoverAt = SDL_GetTicks();
        } else if (g_ui.volumeSliderOpen && !g_ui.volumeDragging &&
                   SDL_GetTicks() > g_ui.volHoverAt + 500) {
            g_ui.volumeSliderOpen = false;
            g_ui.volumeSliderHover = false;
            LOG_DBG("MAIN", "volume slider auto-collapse");
        }

        // �������� hover ���� (���Ӿ�, ��������; ��������ֻ�ڵ����קʱ)
        g_ui.volumeSliderHover = false;
        if (g_ui.volumeSliderOpen && !g_ui.volumeDragging) {
            Row1Layout L;
    layoutRow1(g_ui.totalW > 0 ? g_ui.totalW : g_ui.winW, sbTopY(), true, L);
            if (L.volSliderW > 0 &&
                g_ui.mouseX >= L.volSliderX - U(4) && g_ui.mouseX <= L.volSliderX + L.volSliderW + U(4) &&
                g_ui.mouseY >= L.cy - U(14) && g_ui.mouseY <= L.cy + U(14)) {
                g_ui.volumeSliderHover = true;
                g_ui.volHoverAt = SDL_GetTicks();
            }
        }

        // �б�������: hover ���� + ��ק����
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

        // �б���ק����λ�Ƴ���ֵ������ק̬
        if (!g_ui.sbDragging && g_ui.plDragFrom >= 0 && !g_ui.plDragging &&
            std::abs(g_ui.mouseY - g_ui.plDownY) > U(8)) {
            g_ui.plDragging = true;
            LOG_DBG("MAIN", "playlist drag start from=%d", g_ui.plDragFrom);
        }
        if (g_ui.plDragging) {
            g_ui.plDragY = g_ui.mouseY;
            // �Զ��������ϵ�������±�Եʱ�����б�
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
            layoutRow1(g_ui.winW, sbTopY(), true, L);
            float ratio = (float)(g_ui.mouseX - L.volSliderX) / U(80);
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
        if (!g_ui.seekingDrag) g_ui.seekbarHover = false;
        g_ui.mouseX = g_ui.mouseY = -1;
        return 0;

    case WM_NCCALCSIZE: {
        // �ޱ߿��Ի棺�ͻ���=��������(�Ƴ�ϵͳ������)������ DWM ��Ӱ
        if (!wp) break;
        auto* params = (NCCALCSIZE_PARAMS*)lp;
        if (IsZoomed(hwnd)) {   // ���ʱ�ս���Ļ�߿���
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
        // ��Ե����������ȫ��/���㣩
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
        // ��������ͼ������Ϊ��ק����
        // ģ̬���/�˵���ʱ, ������Ϊ HTCAPTION(������������ڹرհ�ť)
        // �����б�����Ҳ������(�б��رհ�ť�� topbar Y ��Χ��)
        {
            bool anyModalOpen = g_ui.settingsOpen || g_ui.speedMenuOpen || g_ui.qualityMenuOpen ||
                                g_ui.eqMenuOpen || g_ui.subMenuOpen || g_ui.audioMenuOpen ||
                                g_ui.chapterMenuOpen;
            bool inPlArea = (g_ui.playlistOpen && !g_ui.fullscreen && pt.x >= g_ui.winW);
            if (pt.y >= 0 && pt.y <= curTopH() && !anyModalOpen && !inPlArea) {
                if (hitTestTopbarIcon(pt.x, pt.y, g_ui.winW) < 0)
                    return HTCAPTION;
            }
        }
        return HTCLIENT;
    }

    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        int barTop = sbTopY();
        LOG_DBG("MAIN", "parent LBUTTONDOWN (%d,%d) barTop=%d settings=%d playlist=%d winW=%d winH=%d",
                mx, my, barTop, g_ui.settingsOpen ? 1 : 0, g_ui.playlistOpen ? 1 : 0,
                g_ui.winW, g_ui.winH);

        // --- �����б��ر�ť: ������ȼ� ---
        // (y �� topbar �߶��ڻᱻ topbar ��֧����: ����ģʽ����ק, ȫ��ʱ��
        //  Ӧ�ùر�ͼ���ص����������������)
        if (g_ui.playlistOpen && g_ui.plCloseRect.w > 0) {
            LOG_DBG("MAIN", "plCloseRect(%d,%d,%d,%d) mx=%d my=%d",
                    g_ui.plCloseRect.x, g_ui.plCloseRect.y,
                    g_ui.plCloseRect.w, g_ui.plCloseRect.h, mx, my);
        }
        if (g_ui.playlistOpen && g_ui.plCloseRect.w > 0 &&
            mx >= g_ui.plCloseRect.x && mx <= g_ui.plCloseRect.x + g_ui.plCloseRect.w &&
            my >= g_ui.plCloseRect.y && my <= g_ui.plCloseRect.y + g_ui.plCloseRect.h) {
            g_ui.playlistOpen = false;
            LOG_INFO("MAIN", "playlist close btn");
            if (!g_ui.fullscreen) applyPlaylistWindow(hwnd);
            else g_dirty.store(true);
            return 0;
        }

        // --- topbar icon clicks (�б�չ��ʱ�б��������� topbar) ---
        // ģ̬���/�˵���ʱ, topbar ����Ӧ���(������������ڿ��ϵĹرհ�ť)
        bool inPlaylistArea = (g_ui.playlistOpen && !g_ui.fullscreen && mx >= g_ui.winW);
        bool anyModalOpen = g_ui.settingsOpen || g_ui.speedMenuOpen || g_ui.qualityMenuOpen ||
                            g_ui.eqMenuOpen || g_ui.subMenuOpen || g_ui.audioMenuOpen ||
                            g_ui.chapterMenuOpen;
        if (my >= 0 && my <= curTopH() && !inPlaylistArea && !anyModalOpen) {
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
            case 3: // playlist���Ҳ�������򣺴�����չ��
                g_ui.playlistOpen = !g_ui.playlistOpen;
                LOG_INFO("MAIN", "pl toggle -> %d (mx=%d my=%d winW=%d)",
                         g_ui.playlistOpen ? 1 : 0, mx, my, g_ui.winW);
                if (!g_ui.fullscreen) applyPlaylistWindow(hwnd);
                else g_dirty.store(true);
                return 0;
            case 4: { // PIP �ö�����С��
                if (g_mpv && g_mpv->hasMedia()) {
                    if (g_ui.fullscreen) toggleFullscreen(hwnd);  // ȫ�����˳�
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

        // --- M36: ��ӭҳ��������ý��ʱ��: Hero ��ť / �����ۿ� / �������� ---
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
                    std::string p = g_playlist[kv.first];   // ���ڶ���, ���ؽ�
                    playPath(p);
                    return 0;
                }
            }
        }

        // --- settings modal: ������ȼ������ײ����ܸ��ǿ�����/������������,
        //     �����ȴ���, ������/ģʽ�еĵ���ᱻ�������Ե��� ---
        if (g_ui.settingsOpen) {
            SettingsGeom sg = settingsGeom(g_ui.winW, g_ui.winH);
            bool inside = (mx >= sg.panelX && mx <= sg.panelX + sg.panelW &&
                           my >= sg.panelY && my <= sg.panelY + sg.panelH);
            LOG_DBG("MAIN", "settings click: mx=%d my=%d panel(%d,%d,%d,%d) closeC(%d,%d,R=%d) inside=%d",
                    mx, my, sg.panelX, sg.panelY, sg.panelW, sg.panelH,
                    sg.closeCx, sg.closeCy, sg.closeR, inside ? 1 : 0);
            if (!inside) {
                LOG_DBG("MAIN", "settings close: click outside panel");
                g_ui.settingsOpen = false;      // ���� = �ر�(��������´�)
            }
            else if (std::abs(mx - sg.closeCx) <= sg.closeR &&
                     std::abs(my - sg.closeCy) <= sg.closeR) {
                LOG_DBG("MAIN", "settings close hit: mx=%d my=%d cx=%d cy=%d R=%d",
                        mx, my, sg.closeCx, sg.closeCy, sg.closeR);
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
                // �����л�
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
            return 0;   // ���ô��ڼ��������´���������/������/��Ƶ
        }

        // --- �����˵�ģ̬��(����/����/EQ/��Ļ/����): ���ڼ�������ֻ�����ڵ��� ---
        // ����˵���=��Ч; ����˵���=���رա�����������Ƶ��ͣ/������/��ť,
        // ����"��������͸���õ�����"�����⡣
        if (g_ui.speedMenuOpen || g_ui.qualityMenuOpen || g_ui.eqMenuOpen ||
            g_ui.subMenuOpen || g_ui.audioMenuOpen) {
            if (g_ui.speedMenuOpen) {
                Row1Layout L;
                layoutRow1(g_ui.winW, sbTopY(), false, L);
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
                        std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("����", "Speed"), SPEED_PRESETS[idx]);
                        showToast(msg);
                    }
                }
                g_ui.speedMenuOpen = false;
            }
            if (g_ui.qualityMenuOpen) {
                Row1Layout QL;
                layoutRow1(g_ui.winW, sbTopY(), false, QL);
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
                        const char* qNames[] = { T("ʡ��", "Power Saving"), T("��׼", "Standard"), T("����", "Ultimate") };
                        showToast(qNames[idx]);
                    }
                }
                g_ui.qualityMenuOpen = false;
            }
            if (g_ui.eqMenuOpen) {
                int menuW = U(200), itemH = U(36);
                int menuH = U(32) + 6 * itemH + U(40) + U(50);
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
                        // P1-6: EQ Ԥ����
                        struct EqPreset { const char* name; float bands[6]; };
                        static const EqPreset presets[] = {
                            { "Flat",    { 0,  0,  0,  0,  0,  0 } },
                            { "Bass",    { 6,  4,  1, -1, -2, -3 } },
                            { "Treble",  {-3, -2, -1,  1,  4,  6 } },
                            { "Vocal",   {-2, -1,  3,  4,  2, -1 } },
                            { "Rock",    { 5,  3, -1, -1,  3,  5 } },
                        };
                        int presetY = resetY + U(30) + U(14);
                        int presetBtnW = (menuW - U(20)) / 5;
                        for (int i = 0; i < 5; ++i) {
                            int bx = menuX + U(10) + i * presetBtnW;
                            int bw = presetBtnW - U(4);
                            if (mx >= bx && mx <= bx + bw && my >= presetY && my <= presetY + U(22)) {
                                for (int b = 0; b < 6; ++b)
                                    g_mpv->setEQBand(b, presets[i].bands[b]);
                                showToast(presets[i].name);
                                break;
                            }
                        }
                        // �˵�������λ��: ���ִ�
                        g_ui.visible = true;
                        g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
                        g_dirty.store(true);
                        return 0;
                    }
                } else {
                    g_ui.eqMenuOpen = false;
                }
            }
            // --- ��Ļ��ѡ��˵� ---
            if (g_ui.subMenuOpen) {
                auto subs = g_mpv->subTracks();
                Row1Layout L;
                layoutRow1(g_ui.winW, sbTopY(), false, L);
                int itemH = U(32), menuW = U(180);
                int menuH = (int)(subs.size() + 2) * itemH + U(12);  // +1 off +1 load external
                int menuX = L.subBtn.x;
                int menuY = L.subBtn.y - menuH - U(6);
                if (menuY < 0) menuY = L.subBtn.y + L.subBtn.h + U(6);
                if (menuX + menuW > g_ui.winW - U(8)) menuX = g_ui.winW - menuW - U(8);
                if (mx >= menuX && mx <= menuX + menuW &&
                    my >= menuY && my <= menuY + menuH - U(12)) {
                    int idx = (my - menuY - U(6)) / itemH;
                    if (idx == 0) {
                        g_mpv->setSubVisibility(false);
                        showToast(i18n::subtitlesOff());
                    } else if (idx > 0 && idx <= (int)subs.size()) {
                        int trackId = subs[idx - 1].id;
                        g_mpv->setSubtitle(trackId);
                        g_mpv->setSubVisibility(true);
                        char msg[96];
                        std::snprintf(msg, sizeof(msg), "%s: %s", T("��Ļ", "Subtitle"), subs[idx - 1].desc.c_str());
                        showToast(msg);
                    } else if (idx == (int)subs.size() + 1) {
                        // �����ⲿ��Ļ
                        g_ui.subMenuOpen = false;
                        if (g_mpv->hasMedia()) {
                            std::string f = openSubtitleDialog(hwnd);
                            if (!f.empty()) {
                                g_mpv->loadSubtitle(f);
                                showToast(T("��Ļ�Ѽ���", "Subtitle loaded"));
                            }
                        }
                    }
                }
                g_ui.subMenuOpen = false;
            }
            // --- ����ѡ��˵� ---
            if (g_ui.audioMenuOpen) {
                auto tracks = g_mpv->audioTracks();
                Row1Layout L;
                layoutRow1(g_ui.winW, sbTopY(), false, L);
                int itemH = U(32), menuW = U(180);
                int menuH = (int)tracks.size() * itemH + U(12);
                int menuX = L.audioBtn.x;
                int menuY = L.audioBtn.y - menuH - U(6);
                if (menuY < 0) menuY = L.audioBtn.y + L.audioBtn.h + U(6);
                if (menuX + menuW > g_ui.winW - U(8)) menuX = g_ui.winW - menuW - U(8);
                if (mx >= menuX && mx <= menuX + menuW &&
                    my >= menuY && my <= menuY + menuH - U(12)) {
                    int idx = (my - menuY - U(6)) / itemH;
                    if (idx >= 0 && idx < (int)tracks.size()) {
                        g_mpv->setAudioTrack(tracks[idx].id);
                        char msg[96];
                        std::snprintf(msg, sizeof(msg), "%s: %s", T("����", "Audio"), tracks[idx].desc.c_str());
                        showToast(msg);
                    }
                }
                g_ui.audioMenuOpen = false;
            }
            // --- �½�ѡ��˵� ---
            if (g_ui.chapterMenuOpen) {
                auto chs = g_mpv->chapters();
                Row1Layout L;
                layoutRow1(g_ui.winW, sbTopY(), false, L);
                int itemH = U(32), menuW = U(240);
                int menuH = (int)chs.size() * itemH + U(12);
                int menuX = L.chapterBtn.x;
                int menuY = L.chapterBtn.y - menuH - U(6);
                if (menuY < 0) menuY = L.chapterBtn.y + L.chapterBtn.h + U(6);
                if (menuX + menuW > g_ui.winW - U(8)) menuX = g_ui.winW - menuW - U(8);
                if (mx >= menuX && mx <= menuX + menuW &&
                    my >= menuY && my <= menuY + menuH - U(12)) {
                    int idx = (my - menuY - U(6)) / itemH;
                    if (idx >= 0 && idx < (int)chs.size()) {
                        g_mpv->seekToChapter(idx);
                        const char* name = chs[idx].title.empty()
                            ? T("�ޱ���", "Untitled") : chs[idx].title.c_str();
                        char msg[128];
                        std::snprintf(msg, sizeof(msg), "%s %d: %s",
                                      T("�½�", "Chapter"), idx + 1, name);
                        showToast(msg);
                    }
                }
                g_ui.chapterMenuOpen = false;
            }
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            g_dirty.store(true);
            return 0;   // ������ڼ���һ������
        }

        // --- seekbar����ֱ�ݲ��ս�: ��̽�����Ե��û��㰴ť/��Ƶ�ĵ����---
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
            return 0;  // seekbar �������͸����Ƶ��
        }
        // --- controlbar row1 ���У�����Ⱦ���� Row1Layout��---
        {
            bool volOpen = (g_ui.volumeSliderOpen || g_ui.volumeDragging);
            Row1Layout L;
            layoutRow1(g_ui.winW, sbTopY(), volOpen, L);
            auto inRc = [&](const SDL_Rect& r) {
                return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
            };
            // ��������(չ��ʱ)��ק��� ���� ����ͼ���ж�; �������������� ��U(20)
            if (volOpen && mx >= L.volSliderX - U(6) &&
                mx <= L.volSliderX + U(86) &&
                my >= L.cy - U(20) && my <= L.cy + U(20)) {
                g_ui.volumeDragging = true;
                float ratio = (float)(mx - L.volSliderX) / U(80);
                if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
                g_mpv->setVolume(ratio);
                SetCapture(hwnd);
            }
            else if (inRc(L.prev)) {
                int idx = playlistIndexOf(g_mpv->path());
                if (idx > 0) { playIndex(idx - 1); showToast(T("��һ��", "Previous")); }
                else showToast(i18n::noPrev());
            }
            else if (inRc(L.play)) {
                g_mpv->togglePause();
            }
            else if (inRc(L.next)) {
                int idx = playlistIndexOf(g_mpv->path());
                int n = (int)g_playlist.size();
                if (idx >= 0 && idx + 1 < n) { playIndex(idx + 1); showToast(T("��һ��", "Next")); }
                else showToast(i18n::noNext());
            }
            else if (inRc(L.subBtn)) {
                auto subs = g_mpv->subTracks();
                if (subs.size() > 1) {
                    // ����Ļ��: ��ѡ��˵�
                    g_ui.subMenuOpen = !g_ui.subMenuOpen;
                    g_ui.audioMenuOpen = false;
                    g_ui.chapterMenuOpen = false;
                } else {
                    // ����/�޹�: ֱ���л��ɼ���
                    bool vis = !g_mpv->subVisible();
                    g_mpv->setSubVisibility(vis);
                    std::string trk = g_mpv->currentSubTrack();
                    char msg[96];
                    std::snprintf(msg, sizeof(msg), vis ? "%s [%s]" : "%s",
                                  vis ? i18n::subtitlesOn() : i18n::subtitlesOff(),
                                  trk.c_str());
                    showToast(msg);
                }
            }
            else if (inRc(L.speedBtn)) {
                g_ui.speedMenuOpen = !g_ui.speedMenuOpen;
                g_ui.subMenuOpen = false;
                g_ui.audioMenuOpen = false;
                g_ui.chapterMenuOpen = false;
            }
            else if (inRc(L.audioBtn)) {
                auto tracks = g_mpv->audioTracks();
                if (tracks.size() > 1) {
                    g_ui.audioMenuOpen = !g_ui.audioMenuOpen;
                    g_ui.subMenuOpen = false;
                    g_ui.chapterMenuOpen = false;
                } else {
                    showToast(i18n::audioTrack());
                }
            }
            else if (inRc(L.chapterBtn)) {
                auto chs = g_mpv->chapters();
                if (!chs.empty()) {
                    g_ui.chapterMenuOpen = !g_ui.chapterMenuOpen;
                    g_ui.subMenuOpen = false;
                    g_ui.audioMenuOpen = false;
                } else {
                    showToast(T("���½���Ϣ", "No chapters"));
                }
            }
            else if (inRc(L.qualityBtn)) {
                g_ui.qualityMenuOpen = !g_ui.qualityMenuOpen;
                g_ui.subMenuOpen = false;
                g_ui.audioMenuOpen = false;
                g_ui.chapterMenuOpen = false;
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
        // (����/����/EQ �˵���������ģ̬��ͳһ����, �˴����ٿɴ�)
        // --- ����ͼ�������л������������� hover չ���� ---
        if (g_mpv && mx >= g_ui.winW - U(68) && mx <= g_ui.winW - U(40) &&
                 my >= barTop + U(36) && my <= barTop + U(64)) {
            g_mpv->toggleMute();
            showToast(g_mpv->muted() ? i18n::muted() : i18n::unmuted());
            LOG_INFO("MAIN", "mute toggled -> %d", g_mpv->muted() ? 1 : 0);
        }
        // --- �����б�������򣺹ر�ť -> ������ -> �б����ѡ ---
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
                    // �����ҳ: bar ���Ķ�������
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
                int rel = my - U(45) + g_ui.playlistScroll;   // panelY=0 ����Ⱦһ��
                g_ui.plDragFrom = -1; g_ui.plDragging = false;
                if (rel >= 0) {
                    int itemIdx = rel / itemH;
                    if (itemIdx < (int)g_playlist.size()) {
                        g_ui.plDragFrom = itemIdx;
                        g_ui.plDownY = my;
                        SetCapture(hwnd);   // ��ק/���ֶ��������Ҳ�ܸ���
                    }
                }
            }
        }
        // --- click on video area���ӳ�ִ����ͣ��˫������ȫ���� ---
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
        // ����������˫�� -> ��ת�����λ��
        if (g_mpv && g_mpv->duration() > 0 &&
            my >= barTop - U(8) && my <= barTop + U(12) &&
            mx >= sbLeftX() && mx <= sbRightX()) {
            double ratio = (double)(mx - sbLeftX()) / sbWidth();
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_mpv->seek(g_mpv->duration() * ratio);
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
        }
        // ��Ƶ��˫�� -> ȫ���л���ȡ��������ͣ��
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
        // �б���ק��λ / ��������
        if (g_ui.plDragFrom >= 0) {
            if (g_ui.plDragging) {
                int itemH = U(72);
                int topY = U(45);   // panelY=0 ����Ⱦһ��
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
                playIndex(g_ui.plDragFrom);   // δ�϶� = ��������
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

        // �����б�������򣺹����б�
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
            std::snprintf(msg, sizeof(msg), "%s %d%%", T("����", "Volume"), (int)(g_mpv->volume() * 100 + 0.5f));
            showToast(msg);
        }
        g_ui.visible = true;
        g_ui.hideAt = SDL_GetTicks() + 2000;
        return 0;
    }

    // ---- drag-drop ----
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wp;
        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        std::string firstPath;
        std::vector<std::string> droppedFiles;
        for (UINT i = 0; i < fileCount; ++i) {
            wchar_t wpath[MAX_PATH * 2];
            if (DragQueryFileW(hDrop, i, wpath, (UINT)(MAX_PATH * 2)) > 0) {
                int u8len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                                nullptr, 0, nullptr, nullptr);
                std::string path(u8len > 0 ? u8len - 1 : 0, '\0');
                if (u8len > 1)
                    WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                        path.data(), u8len, nullptr, nullptr);
                droppedFiles.push_back(path);
                if (firstPath.empty()) firstPath = path;
            }
        }
        LOG_INFO("MAIN", "drop %zu files, first: %s", droppedFiles.size(), firstPath.c_str());
        if (droppedFiles.size() == 1) {
            buildPlaylistAround(firstPath);
            playPath(firstPath);
        } else if (droppedFiles.size() > 1) {
            // ���ļ�: ���������б�, ���ŵ�һ��
            g_playlist = droppedFiles;
            playPath(firstPath);
            if (!g_ui.playlistOpen) g_ui.playlistOpen = true;
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_CLOSE:
        saveWindowPos(hwnd);          // ����ǰץȡλ��
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

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

static void destroyGradCache();   // ������ drawGradientBar�������������棩

static UlwCtx g_ulw;

static void destroyOverlay() {
    ulwDestroy(g_ulw);
    g_text.shutdown();
    svgicon::shutdown();
    destroyGradCache();
    if (g_sdlRdr) { SDL_DestroyRenderer(g_sdlRdr); g_sdlRdr = nullptr; }
    if (g_sdlWin) { SDL_DestroyWindow(g_sdlWin);   g_sdlWin = nullptr; }  // ��ͬ HWND һ������
    g_overlayHwnd = nullptr;
}

// ---- dithered gradient helper ----
// �����������ػ��ƴ��� ~13 ��� FillRect/֡; ����Ϊ������ÿ֡һ�� RenderCopy��
// ͸����(alpha<��ֵ)д�� 0 ���� ��Ʒ�� colorkey ����, ��Ƶ�ճ���͸��
// P2-2: GradKey/GradCache/drawDitherDim/drawGradientBar ���� ui/gradient.h
static GradCache g_gradCache;

static void destroyGradCache() {
    g_gradCache.destroy();
}

// ---- rendering ----
// P2-3: UlwCtx/ulwDestroy/ulwResize ���� ui/ulw.h
static SDL_Texture* g_ovTex = nullptr;   // UI ��������(�� alpha)
static int g_ovTexW = 0, g_ovTexH = 0;

// M36: ��֡Բ�������б� (��Ⱦʱ���, overlayPresent ���Ѻ����)
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

static void overlayPresent() {
    if (!g_sdlRdr || !g_sdlWin || !g_overlayHwnd) return;
    int w = g_ui.totalW > 0 ? g_ui.totalW : g_ui.winW;
    int h = g_ui.winH > 0 ? g_ui.winH : 540;
    if (w <= 0 || h <= 0 || !g_ovTex) return;

    // 1. ������ ARGB �����ض� (�� alpha)
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

    // 1.5 M36: Բ������ͼ���� �� ����������ȫ͸��(����Ⱦ�޷��ü�����)
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

    // 2. Ԥ�� alpha + ULW ���� (P2-3: ���� ui/ulw.h)
    ulwPresent(g_ulw, g_overlayHwnd, g_parentHwnd, px.data(), w, h);
}

// M36: ���ؿ���Լ���ĵ���ʡ�� (UTF-8 ��ȫ, ��������)
static std::string ellipsize(const std::string& s, int pt, int maxW) {
    if (s.empty() || g_text.measureText(s, Tpt(pt)) <= maxW) return s;
    std::string out = s;
    while (out.size() > 1) {
        size_t n = out.size();
        while (n > 0 && (out[n - 1] & 0xC0) == 0x80) --n;   // �������ֽ�
        if (n > 0) --n;                                      // ȥ��һ��ǰ���ֽ�
        out.resize(n);
        if (g_text.measureText(out + "...", Tpt(pt)) <= maxW) break;
    }
    return out + "...";
}

// M36: ����ͼ cover ���� + ע��Բ������; δ����ʱ��ռλ���
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
        if (srcA > dstA + 0.01) {          // Դ���� �� ������
            int cw = (int)(th * dstA);
            src.x = (tw - cw) / 2; src.w = cw;
        } else if (srcA < dstA - 0.01) {   // Դ��խ �� ������
            int ch = (int)(tw / dstA);
            src.y = (th - ch) / 2; src.h = ch;
        }
        SDL_RenderCopy(g_sdlRdr, it->second, &src, &rc);
    }
    if (rad > 0) g_roundMasks.push_back({rc.x, rc.y, rc.w, rc.h, rad});
}

void renderOverlay() {
    if (!g_sdlRdr || !g_sdlWin) return;

    uploadThumbs(g_sdlRdr);   // �����ϴ�����������ͼ����

    // ���� UI �������� ARGB ����(�� alpha); �󱸻��岻ʹ��
    // ֱ���� g_ui.winW/winH (WM_SIZE �Ѹ���), ���� SDL_GetWindowSize �����ӳ�
    int ow = g_ui.totalW > 0 ? g_ui.totalW : g_ui.winW;
    int oh = g_ui.winH > 0 ? g_ui.winH : 540;
    if (ow <= 0 || oh <= 0) return;
    if (!ovTexEnsure(ow, oh)) return;
    SDL_SetRenderTarget(g_sdlRdr, g_ovTex);

    SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 0);   // ȫ͸����(per-pixel alpha)
    SDL_RenderClear(g_sdlRdr);

    int w = g_ui.winW, h = g_ui.winH, totalW = g_ui.totalW;

    if (!g_mpv || !g_mpv->hasMedia()) {
        // --- M36 welcome page: Apple ���� Hero + YouTube ����ͼ��Ƭ ---
        int w = g_ui.winW, h = g_ui.winH, totalW = g_ui.totalW;

        // �볡���� (�뿪��ӭҳʱ����, ���·����ŷ�֧����)
        g_ui.introAlpha = std::min(1.0f, g_ui.introAlpha + 0.055f);
        Uint8 fa8 = (Uint8)(255 * g_ui.introAlpha);
        auto A8 = [&](Uint8 base) { return (Uint8)(base * g_ui.introAlpha); };

        g_ui.continueHits.clear();
        g_ui.gridHits.clear();
        std::vector<std::string> wantThumbs;

        // ��ɫ��: ��ý��ʱ mpv �Ӵ����ǰ׵�, ��ӭҳ�����Լ�������ס
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, ui::SURFACE0_R, ui::SURFACE0_G,
                               ui::SURFACE0_B, A8(255));
        SDL_Rect fullBg = {0, 0, totalW, h};
        SDL_RenderFillRect(g_sdlRdr, &fullBg);

        // ---- topbar (�벥��̬һ�µ�ȫ��ͼ��) ----
        drawGradientBar(g_sdlRdr, 0, 0, 0, totalW, curTopH(), 11, 11, 11,
                        (Uint8)(ui::TOPBAR_A0 * g_ui.introAlpha), 0, g_gradCache);
        {
            std::string title = i18n::appName();
            g_text.drawText(U(20), U(14), title, Tpt(14), 255, 255, 255, A8(255));
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

        // ---- Hero: ����Բ��Ӧ��ͼ�� (�������Զ�����) ----
        const int margin = U(48);
        bool compact = (h < U(660));
        int iconSz = compact ? U(60) : U(88);
        int ix = (w - iconSz) / 2;
        int iy = curTopH() + (compact ? U(20) : U(36));
        roundedRectFill(g_sdlRdr, ix, iy, iconSz, iconSz, U(20),
                        ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, A8(255));
        // �ڹ���: ���Ϲ�Դ, ˫��ͬ��Բ (ȫ����ͼ���ڲ�)
        fillCircle(g_sdlRdr, ix + U(28), iy + U(28), U(18),
                   ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, A8(38));
        fillCircle(g_sdlRdr, ix + U(28), iy + U(28), U(10),
                   ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, A8(52));
        svgicon::draw(g_sdlRdr, "play", ix + iconSz / 2, iy + iconSz / 2, U(30),
                      255, 255, 255, A8(255));

        // ---- ��Ʒ�� + ���� (��ఴ�ֺ�ʵ�ʸ߶�, ���ص�) ----
        int namePt = Tpt(ui::T_DISPLAY);
        int nameHpx = (int)(namePt * g_dpi * 1.4f);   // GDI �и߽���
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
            int tgw = g_text.measureText(tg, Tpt(ui::T_BODY));
            g_text.drawText((w - tgw) / 2, tagY, tg, Tpt(ui::T_BODY),
                            170, 170, 178, A8(255));
        }

        // ---- ˫ҩ�谴ť (MD3: ��������� + ��ߴβ���) ----
        int btnH = compact ? U(40) : U(46);
        int btnY = tagY + U(18) + (compact ? U(6) : U(18));
        {
            std::string l1 = i18n::openFile(), l2 = i18n::openFolder();
            int w1 = g_text.measureText(l1, Tpt(ui::T_BODY)) + U(48);
            int w2 = g_text.measureText(l2, Tpt(ui::T_BODY)) + U(48);
            int gap = U(14);
            int bx1 = (w - (w1 + gap + w2)) / 2;
            int bx2 = bx1 + w1 + gap;
            g_ui.heroFileBtn   = {bx1, btnY, w1, btnH};
            g_ui.heroFolderBtn = {bx2, btnY, w2, btnH};

            bool hov1 = (g_ui.mouseX >= bx1 && g_ui.mouseX <= bx1 + w1 &&
                         g_ui.mouseY >= btnY && g_ui.mouseY <= btnY + btnH);
            bool hov2 = (g_ui.mouseX >= bx2 && g_ui.mouseX <= bx2 + w2 &&
                         g_ui.mouseY >= btnY && g_ui.mouseY <= btnY + btnH);

            // ���ʽ����ť: ��, ��ͣ����Ϊ��������
            roundedRectFill(g_sdlRdr, bx1, btnY, w1, btnH, btnH / 2,
                            hov1 ? ui::ACCENT2_R : ui::ACCENT_R_,
                            hov1 ? ui::ACCENT2_G : ui::ACCENT_G_,
                            hov1 ? ui::ACCENT2_B : ui::ACCENT_B_, A8(255));
            int t1w = g_text.measureText(l1, Tpt(ui::T_BODY));
            g_text.drawText(bx1 + (w1 - t1w) / 2, btnY + U(12), l1,
                            Tpt(ui::T_BODY), 255, 255, 255, A8(255));

            // ���ʽ�ΰ�ť: �����, ��ͣ���𱳾�
            if (hov2) {
                SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, A8(22));
                SDL_Rect fb = {bx2, btnY, w2, btnH};
                SDL_RenderFillRect(g_sdlRdr, &fb);
            }
            roundedRectStroke(g_sdlRdr, bx2, btnY, w2, btnH, btnH / 2,
                              255, 255, 255, A8(hov2 ? 90 : 60));
            int t2w = g_text.measureText(l2, Tpt(ui::T_BODY));
            g_text.drawText(bx2 + (w2 - t2w) / 2, btnY + U(12), l2,
                            Tpt(ui::T_BODY), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5, A8(255));
        }
        // ��ק��ʾ (������ť�·�)
        int hintY = btnY + btnH + U(8);
        {
            std::string dh = i18n::dropAnywhere();
            int dw = g_text.measureText(dh, Tpt(ui::T_CAPTION));
            g_text.drawText((w - dw) / 2, hintY, dh,
                            Tpt(ui::T_CAPTION), 140, 140, 148, A8(255));
        }

        // ---- �����ۿ��� (YouTube ����ͼ��Ƭ) ----
        int contentY = hintY + U(22) + (compact ? U(24) : U(40));
        struct CWItem { std::string path; double pos, dur; long long ts; };
        static std::vector<CWItem> cw;
        static uint32_t cwBuildTick = 0;
        uint32_t nowTick = SDL_GetTicks();
        if (nowTick - cwBuildTick > 2000 || cwBuildTick == 0) {
            cw.clear();
            for (const auto& kv : g_cfg.history) {
                const HistoryEntry& e = kv.second;
                if (e.pos > 1.0 && (e.dur <= 0 || e.pos < e.dur * 0.95))
                    cw.push_back({kv.first, e.pos, e.dur, e.lastPlayed});
            }
            if (!cw.empty()) {
                std::sort(cw.begin(), cw.end(),
                          [](const CWItem& a, const CWItem& b) { return a.ts > b.ts; });
            }
            cwBuildTick = nowTick;
        }
        if (!cw.empty()) {
            int cardW = compact ? U(150) : U(180);
            int gap = U(14);
            int maxCards = std::max(1, (totalW - margin * 2 + gap) / (cardW + gap));
            int nShow = std::min((int)cw.size(), maxCards);
            int rowW = nShow * cardW + (nShow - 1) * gap;
            int gx = ((totalW > w ? totalW : w) - rowW) / 2;   // �б�����ʱ������ȫ�ͻ���

            {
                std::string hd = i18n::continueWatching();
                g_text.drawText(std::max(margin, gx), contentY - U(30), hd,
                                Tpt(ui::T_HEADLINE), 255, 255, 255, A8(255));
            }
            for (int i = 0; i < nShow; ++i) {
                const CWItem& it = cw[i];
                int cx = gx + i * (cardW + gap);
                int thumbH = cardW * 9 / 16;
                SDL_Rect trc = {cx, contentY, cardW, thumbH};
                drawThumbCover(it.path, trc, U(8));

                // ������ (��)
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
                // ���� (�������ؼ�ʡ��)
                std::string fn = fileNameOf(it.path);
                fn = ellipsize(fn, ui::T_BODY, cardW);
                g_text.drawText(cx, contentY + thumbH + U(8), fn,
                                Tpt(ui::T_BODY), 235, 235, 240, A8(255));
                // ������: ���� xx% �� ʱ���
                char tb1[16];
                formatTime(tb1, sizeof(tb1), it.pos);
                std::string sub2 = std::string(T("���� ", "Watched ")) +
                                   (it.dur > 0 ? std::to_string((int)(it.pos / it.dur * 100 + 0.5)) + "% �� " : "") +
                                   tb1;
                g_text.drawText(cx, contentY + thumbH + U(30), sub2,
                                Tpt(ui::T_CAPTION), 150, 150, 158, A8(255));

                g_ui.continueHits.push_back({it.path, {cx, contentY - U(4), cardW, thumbH + U(52)}});
                wantThumbs.push_back(it.path);
            }
            contentY += cardW * 9 / 16 + (compact ? U(58) : U(76));
        }

        // ---- �ļ��ж������� (����ͼ��) ----
        if (!g_playlist.empty()) {
            {
                std::string hd = i18n::playlist();
                g_text.drawText(std::max(margin, (w - 0) / 2), contentY - U(26), hd,
                                Tpt(ui::T_HEADLINE), 255, 255, 255, A8(255));
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
                // ��ǰ��컷 / ��ͣ�׻�
                if (isCur)
                    roundedRectStroke(g_sdlRdr, cx - U(2), cy - U(2), cardW + U(4),
                                      thumbH + U(4), U(9),
                                      ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, A8(255));
                else if (hov)
                    roundedRectStroke(g_sdlRdr, cx - U(2), cy - U(2), cardW + U(4),
                                      thumbH + U(4), U(9), 255, 255, 255, A8(80));

                std::string fn = ellipsize(fileNameOf(g_playlist[i]), ui::T_CAPTION, cardW);
                g_text.drawText(cx, cy + thumbH + U(8), fn,
                                Tpt(ui::T_CAPTION), isCur ? 255 : 225, isCur ? 255 : 225,
                                isCur ? 255 : 230, A8(255));
                // ʱ��/���ȸ�����
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
                                    Tpt(ui::T_CAPTION), 140, 140, 148, A8(255));

                g_ui.gridHits.push_back({i, {cx, cy, cardW, thumbH + U(50)}});
                wantThumbs.push_back(g_playlist[i]);
            }
        }

        // �ύ��ӭҳ�ɼ�����ͼ���� (�벥���б����ϲ�����)
        if (!wantThumbs.empty()) {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            for (auto& p : wantThumbs)
                if (!g_thumbRgb.count(p) && !g_thumbTex.count(p) &&
                    std::find(g_thumbWant.begin(), g_thumbWant.end(), p) == g_thumbWant.end())
                    g_thumbWant.push_back(p);
        }

        // ---- �ײ�: ������ʾ(����) + �汾(����) ----
        {
            std::string hint = T("�ո� ����/��ͣ �� ���� ����� �� F ȫ�� �� M ����",
                                 "Space Play/Pause �� Arrows Seek �� F Fullscreen �� M Mute");
            int hw = g_text.measureText(hint, Tpt(ui::T_CAPTION));
            g_text.drawText((totalW - hw) / 2, h - U(30), hint,
                            Tpt(ui::T_CAPTION), ui::HINT_TEXT, ui::HINT_TEXT, ui::HINT_TEXT + 6, A8(200));
            std::string ver = std::string("v") + PHANTOM_VERSION;
            int vw = g_text.measureText(ver, Tpt(ui::T_CAPTION));
            g_text.drawText(totalW - vw - U(16), h - U(30), ver,
                            Tpt(ui::T_CAPTION), 110, 110, 116, A8(160));
        }

        overlayPresent();
        return;
    }

    g_ui.introAlpha = 0.0f;   // �뿪��ӭҳ, �´ν������µ���
    double dur = g_mpv->duration();
    // �ٶ��л�����ݶ��������, ��ֹ time-pos ���䵼�¶���
    static double s_lastPos = 0.0;
    static Uint32 s_freezeStart = 0;
    double pos;
    if (g_ui.seekingDrag) {
        pos = g_ui.seekTarget;
    } else if (g_mpv->seekbarFrozen()) {
        // �����ڼ�: �ö���ǰ�� pos + ������ǽ��ʱ�� * ���ٶ� �ƽ�
        if (s_freezeStart == 0) { s_lastPos = g_mpv->clock(); s_freezeStart = SDL_GetTicks(); }
        double elapsed = (SDL_GetTicks() - s_freezeStart) / 1000.0;
        pos = s_lastPos + elapsed * g_mpv->speed();
        double d = g_mpv->duration();
        if (d > 0 && pos > d) pos = d;
    } else {
        pos = g_mpv->clock();
        s_freezeStart = 0;  // �ⶳ: ����
    }

    // �ؼ���������: alpha=0 ʱ��������������������������
    float fa = g_ui.ctrlAlpha;
    Uint8 fade = (Uint8)(fa * 255.0f);
    int topOff = -(int)((1.0f - fa) * curTopH() + 0.5f);

    // --- topbar (gradient: glass ��͸��Ч��, ��Ƶ��Լ�ɼ�) ---
    {
        drawGradientBar(g_sdlRdr, 0, 0, topOff, w, U(52), 11, 11, 11,
                        (Uint8)(ui::TOPBAR_A0 * fa), 0, g_gradCache);

        // title (left)
        std::string title = g_mpv->title();
        if (title.empty()) title = "��Ӱ��Ƶ";
        if (title.size() > 55) title = title.substr(0, 52) + "...";
        g_text.drawText(U(20), U(14) + topOff, title, Tpt(14), 255, 255, 255);

        // icons (right) �� ����ͼ�� + ��ͣ��������
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
            // ��ͣ����
            if (g_ui.topbarHover == i) {
                SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
                if (i == 0) {
                    // close: ��ɫ��ͣ
                    SDL_SetRenderDrawColor(g_sdlRdr, 232, 17, 35, A(240));
                } else {
                    SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, A(50));
                }
                SDL_Rect hrc = {rx - hoverR, iconY - hoverR, hoverR * 2, hoverR * 2};
                SDL_RenderFillRect(g_sdlRdr, &hrc);
            }
            // ����ͼ��, �Ŵ�Ӵ�
            svgicon::draw(g_sdlRdr, topIcons[i].id, rx, iconY, iconDrawSz,
                          255, 255, 255, A(255));
            rx -= iconSz;
        }
    }

    // �ؼ�����: �������� alpha ��������
    int ctrlH = U(80);
    int barTop = sbTopY() + (int)((1.0f - fa) * ctrlH + 0.5f);

    // --- ��ͣѹ������ + ���벥��ͼ�� (per-pixel alpha ���͸��, ȫ�������) ---
    if (fa > 0.01f && g_mpv->state() == MpvBackend::State::Paused) {
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, (Uint8)(110 * fa));
        SDL_Rect dim = {0, 0, w, h};
        SDL_RenderFillRect(g_sdlRdr, &dim);
        int ccx = w / 2, ccy = h / 2;
        svgicon::draw(g_sdlRdr, "play", ccx, ccy, U(52),
                      255, 255, 255, (Uint8)(220 * fa));
    }

    // --- gradient background (Ч��ͼ: ���㽥�� �ײ�������ȫ͸) ---
    drawGradientBar(g_sdlRdr, 1, 0, barTop, w, ctrlH, 0, 0, 0, ui::CTRLBAR_A0, ui::CTRLBAR_A1, g_gradCache);

    // --- seekbar (at very top of bar) ---
    if (dur > 0) {
        int tx = sbLeftX(), tw = sbWidth();
        int ty = barTop + U(9);
        bool thumbActive = g_ui.seekbarHover || g_ui.seekingDrag;
        int th = thumbActive ? ui::SEEK_TRACK_H_HOVER : ui::SEEK_TRACK_H;

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

        // --- seekbar thumb (always-visible small dot + hover/drag enlarged glow) ---
        int cx = tx + progW;
        int cy = ty + th / 2;
        // Ĭ��̬: СԲ�� (Ʒ��ɫ)
        int rDefault = std::max(ui::THUMB_R_DEFAULT, (int)(g_ui.winW * 0.003f));
        fillCircle(g_sdlRdr, cx, cy, rDefault,
                   ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
        // hover/drag ̬: �Ŵ��Բ + ��͸������
        if (thumbActive) {
            int rHov = std::max(ui::THUMB_R_HOVER, (int)(g_ui.winW * 0.006f));
            // ��Ȧ���� (ͬɫ��͸��)
            fillCircle(g_sdlRdr, cx, cy, rHov + ui::THUMB_GLOW_R,
                       ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 50);
            // ��ɫʵ��Բ
            fillCircle(g_sdlRdr, cx, cy, rHov, 255, 255, 255, 255);
        }

        // ʱ��Ԥ������ (hover/drag only)
        if (thumbActive) {

            // Ԥ��ʱ�������
            double hoverPos = dur * ((double)(g_ui.mouseX - tx) / tw);
            if (hoverPos < 0) hoverPos = 0;
            if (hoverPos > dur) hoverPos = dur;
            char pv[16];
            formatTime(pv, sizeof(pv), hoverPos);
            int bw = g_text.measureText(pv, Tpt(11)) + U(16);
            int bh = U(22);
            int bx = g_ui.mouseX - bw / 2;
            if (bx < tx) bx = tx;
            if (bx + bw > tx + tw) bx = tx + tw - bw;
            int by = ty - bh - U(10);
            // ���ݱ���(Բ�ǽ���)
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
            // ��������
            g_text.drawText(bx + U(8), by + U(4), pv, Tpt(11), 255, 255, 255);
        }
    }

    // --- controlbar row1 (Ч��ͼ����): prev/PLAY�׵�/next/time ... ��Ļ/����/����/����/����/ȫ�� ---
    {
        Row1Layout L;
        bool volOpen = (g_ui.volumeSliderOpen || g_ui.volumeDragging);
        layoutRow1(w, barTop, volOpen, L);
        auto A = [&](Uint8 base) { return (Uint8)(base * fa); };
        const int iconC = ui::ICON_BRIGHT, text2 = ui::ICON_DIM;

        // prev
        int ctrlIconSz = U(42);
        svgicon::draw(g_sdlRdr, "prev", L.prev.x + ctrlIconSz / 2, L.prev.y + ctrlIconSz / 2, U(28),
                      255, 255, 255, A(255));
        // PLAY ��ͼ��
        {
            const char* pi = (g_mpv->state() == MpvBackend::State::Paused) ? "play" : "pause";
            svgicon::draw(g_sdlRdr, pi, L.play.x + L.play.w / 2, L.play.y + L.play.h / 2,
                          U(28), 255, 255, 255, A(255));
        }
        // next
        svgicon::draw(g_sdlRdr, "next", L.next.x + ctrlIconSz / 2, L.next.y + ctrlIconSz / 2, U(28),
                      255, 255, 255, A(255));
        // time��tabular �۸�: �ȿ������屣֤��
        {
            char cur[32], tot[32], ts[80];
            formatTime(cur, sizeof(cur), pos);
            formatTime(tot, sizeof(tot), dur);
            std::snprintf(ts, sizeof(ts), "%s / %s", cur, tot);
            g_text.drawText(L.timeX, L.cy - U(9), ts, Tpt(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        }

        // �Ҳ� textbtn �� (���� + ͼ��)
        auto drawTextBtn = [&](const SDL_Rect& rc, const char* label,
                               const char* iconId, Uint8 ir, Uint8 ig, Uint8 ib) {
            int tw = g_text.measureText(label, Tpt(12));
            int tx = rc.x + U(8);
            g_text.drawText(tx, rc.y + U(10), label, Tpt(12), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5);
            svgicon::draw(g_sdlRdr, iconId, tx + tw + U(9), rc.y + U(17), U(22),
                          ir, ig, ib, A(255));
        };
        // ��Ļ
        {
            Uint8 ic = g_mpv->subVisible() ? 255 : 110;
            drawTextBtn(L.subBtn, i18n::subtitles(), "cc", ic, ic, ic);
        }
        // ���� (���ְ�ť)
        {
            Uint8 ic = g_mpv->audioTracks().size() > 1 ? 255 : 110;
            drawTextBtn(L.audioBtn, i18n::audioTrack(), "cc", ic, ic, ic);
        }
        // �½� (���ְ�ť)
        {
            Uint8 ic = g_mpv->chapters().size() > 1 ? 255 : 110;
            drawTextBtn(L.chapterBtn, i18n::chapName(), "list", ic, ic, ic);
        }
        // ����
        {
            char spd[16];
            float s = g_mpv->speed();
            if (s == (int)s) std::snprintf(spd, sizeof(spd), "%.0fx", s);
            else             std::snprintf(spd, sizeof(spd), "%.2fx", s);
            int lw = g_text.measureText(i18n::speed(), Tpt(12));
            g_text.drawText(L.speedBtn.x + U(8), L.speedBtn.y + U(10), i18n::speed(), Tpt(12), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5);
            g_text.drawText(L.speedBtn.x + U(8) + lw + U(4), L.speedBtn.y + U(10), spd, Tpt(12),
                            ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B);
        }
        // ���� + �ֱ��ʱ�ǩ
        {
            const char* ql = qualityLabel();
            int qw = g_text.measureText(i18n::quality(), Tpt(12));
            g_text.drawText(L.qualityBtn.x + U(8), L.qualityBtn.y + U(10), i18n::quality(), Tpt(12), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5);
            g_text.drawText(L.qualityBtn.x + U(8) + qw + U(4), L.qualityBtn.y + U(11), ql, Tpt(11), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        }
        // ����ͼ��
        {
            const char* vid = g_mpv->muted() ? "mute" : "volume";
            svgicon::draw(g_sdlRdr, vid, L.volIconCx, L.cy, U(28),
                          255, 255, 255, A(255));
        }
        // ����(����+gear)
        drawTextBtn(L.setBtn, i18n::settings(), "gear", 255, 255, 255);
        // ȫ��
        const char* fid = g_ui.fullscreen ? "exitfull" : "full";
        svgicon::draw(g_sdlRdr, fid, L.fullBtn.x + ctrlIconSz / 2, L.fullBtn.y + ctrlIconSz / 2, U(28),
                      255, 255, 255, A(255));

        // ��������(չ��̬)
        if (volOpen && L.volSliderW > 0) {
            int sldW = U(80);
            int sx = L.volSliderX;
            bool hov = g_ui.volumeSliderHover || g_ui.volumeDragging;
            int slH = hov ? U(5) : U(4);
            int sy = L.cy - slH / 2;
            // �������
            Uint8 trackA = hov ? 65 : 40;
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, trackA);
            SDL_Rect trk = {sx, sy, sldW, slH};
            SDL_RenderFillRect(g_sdlRdr, &trk);
            // �����
            float v = g_mpv->volume();
            int fw = (int)(sldW * v);
            if (fw > 1) {
                SDL_Rect fl = {sx, sy, fw, slH};
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
                SDL_RenderFillRect(g_sdlRdr, &fl);
            }
            // Բ�� thumb (hover ʱ�ӹ���)
            int tx = sx + fw;
            int ty = L.cy;
            int thumbR = hov ? U(7) : U(6);
            if (hov) {
                // ����
                fillCircle(g_sdlRdr, tx, ty, thumbR + U(3), 255, 255, 255, 40);
            }
            fillCircle(g_sdlRdr, tx, ty, thumbR, 255, 255, 255, 255);
        }
    }

    // --- buffering indicator ---
    if (g_mpv->bufferFill() < 0.5) {
        g_text.drawText(w / 2 - U(30), barTop + U(75), "Buffering...", Tpt(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
    }

    // --- speed popup menu��Ч��ͼ���: Բ��r8/����չ��/k��ע�� ---
    if (g_ui.speedMenuOpen) {
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32);
        int menuW = U(132);
        int menuH = SPEED_PRESET_COUNT * itemH + U(12);
        int menuX = L.speedBtn.x;                        // �밴ť�����
        int menuY = L.speedBtn.y - menuH - U(6);        // ����չ��
        if (menuY < 0) menuY = L.speedBtn.y + L.speedBtn.h + U(6);  // �ռ䲻��ʱ��������
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);

        // Բ�Ǿ���: �Ȼ���������, ����Բ����Ľ�
        int cr = U(8);  // corner radius
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        // �м���Բ�ǲ���(������)
        SDL_Rect midH = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH);
        // �Ľ�Բ
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        // �߿�(��: ֻ��ֱ�߶�)
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
            g_text.drawText(menuX + U(10), iy + U(6), label, Tpt(13), tr, tg, tb);
            // k ��ע: ��/����/��
            const char* k = (sp < 0.99f) ? T("��", "Slow") : (sp < 1.01f) ? T("����", "Normal") :
                            (sp < 2.01f) ? nullptr : T("��", "Fast");
            if (k) {
                int kw = g_text.measureText(k, Tpt(11));
                g_text.drawText(menuX + menuW - kw - U(10), iy + U(7), k, Tpt(11), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
            }
        }
    }

    // --- quality popup menu (����: ��Ƶ��Ϣ + ����Ԥ��) ---
    if (g_ui.qualityMenuOpen) {
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32);
        int menuW = U(140);
        int infoH = U(38);
        int menuH = infoH + QUALITY_PRESET_COUNT * itemH + U(12);
        int menuX = L.qualityBtn.x;
        int menuY = L.qualityBtn.y - menuH - U(6);
        if (menuY < 0) menuY = L.qualityBtn.y + L.qualityBtn.h + U(6);
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);

        // Բ�Ǿ��α���
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

        // ��Ƶ��Ϣ��
        int iw = g_mpv->videoWidth(), ih = g_mpv->videoHeight();
        char info[64];
        std::snprintf(info, sizeof(info), "%dx%d", iw, ih);
        g_text.drawText(menuX + U(10), menuY + U(8), info, Tpt(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        // �ָ���
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 20);
        SDL_RenderDrawLine(g_sdlRdr, menuX + U(8), menuY + infoH - U(4),
                           menuX + menuW - U(8), menuY + infoH - U(4));

        // Ԥ��ѡ��
        const char* qNames[] = { T("ʡ��", "Power Saving"), T("��׼", "Standard"), T("����", "Ultimate") };
        for (int i = 0; i < QUALITY_PRESET_COUNT; ++i) {
            int iy = menuY + infoH + i * itemH;
            bool sel = (g_ui.qualityPreset == i);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            g_text.drawText(menuX + U(10), iy + U(8), qNames[i], Tpt(13), tr, tg, tb);
            // ��ǰѡ�б��
            if (sel) {
                g_text.drawText(menuX + menuW - U(24), iy + U(8), "?", Tpt(13), ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B);
            }
        }
    }
    // --- EQ popup menu (6Ƶ�ξ�����) ---
    if (g_ui.eqMenuOpen) {
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        static const char* bandNames[] = {"60Hz","170Hz","310Hz","600Hz","3kHz","12kHz"};
        int sliderW = U(100);
        int itemH = U(36);
        int menuW = U(200);
        int menuH = U(32) + 6 * itemH + U(40) + U(50);  // title + 6 bands + reset + presets
        int menuX = w / 2 - menuW / 2;           // ������ʾ
        int menuY = h / 2 - menuH / 2;

        // ����
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
        // ����
        g_text.drawText(menuX + U(10), menuY + U(10), i18n::equalizer(), Tpt(13), 255, 255, 255);
        // ����״̬
        const char* st = g_mpv->eqEnabled() ? "ON" : "OFF";
        Uint8 sr = g_mpv->eqEnabled() ? 59 : 161, sg = g_mpv->eqEnabled() ? 130 : 161, sb = g_mpv->eqEnabled() ? 246 : 166;
        g_text.drawText(menuX + menuW - U(40), menuY + U(10), st, Tpt(12), sr, sg, sb);

        // 6 Ƶ�λ���
        int baseY = menuY + U(32);
        int trackX = menuX + U(60);
        int trackW = sliderW;
        for (int i = 0; i < 6; ++i) {
            int iy = baseY + i * itemH;
            g_text.drawText(menuX + U(10), iy + U(8), bandNames[i], Tpt(11), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
            // ���
            SDL_SetRenderDrawColor(g_sdlRdr, 58, 58, 62, 255);
            SDL_Rect trk = {trackX, iy + U(14), trackW, U(4)};
            SDL_RenderFillRect(g_sdlRdr, &trk);
            // ����λ��: gain -12..+12 �� 0..1
            float gain = g_mpv->eqGain(i);
            float norm = (gain + 12.0f) / 24.0f;
            if (norm < 0.0f) norm = 0.0f; if (norm > 1.0f) norm = 1.0f;
            int thumbX = trackX + (int)(norm * trackW);
            // ���� thumb
            fillCircle(g_sdlRdr, thumbX, iy + U(16), U(6), ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, 255);
            // ��ֵ
            char val[16];
            std::snprintf(val, sizeof(val), "%+.0f", gain);
            g_text.drawText(trackX + trackW + U(8), iy + U(8), val, Tpt(11), ui::ICON_BRIGHT, ui::ICON_BRIGHT, 231);
            // �洢�����������ڵ��
            static SDL_Rect s_bandRects[6];
            s_bandRects[i] = {trackX - U(8), iy, trackW + U(16), itemH};
            // (hit-test �ں��洦��)
        }
        // Reset ��ť
        int resetY = baseY + 6 * itemH + U(4);
        SDL_Rect resetRc = {menuX + menuW / 2 - U(30), resetY, U(60), U(26)};
        SDL_SetRenderDrawColor(g_sdlRdr, 58, 58, 62, 255);
        SDL_RenderFillRect(g_sdlRdr, &resetRc);
        g_text.drawText(resetRc.x + U(14), resetRc.y + U(5), i18n::reset(), Tpt(11), ui::ICON_BRIGHT, ui::ICON_BRIGHT, 231);

        // P1-6: EQ Ԥ�谴ť
        struct EqPreset { const char* name; float bands[6]; };
        static const EqPreset presets[] = {
            { "Flat",    { 0,  0,  0,  0,  0,  0 } },
            { "Bass",    { 6,  4,  1, -1, -2, -3 } },
            { "Treble",  {-3, -2, -1,  1,  4,  6 } },
            { "Vocal",   {-2, -1,  3,  4,  2, -1 } },
            { "Rock",    { 5,  3, -1, -1,  3,  5 } },
        };
        static const int kPresetCount = (int)(sizeof(presets) / sizeof(presets[0]));
        int presetY = resetY + U(30);
        int presetBtnW = (menuW - U(20)) / kPresetCount;
        g_text.drawText(menuX + U(10), presetY - U(2), T("Ԥ��:", "Presets:"), Tpt(10), 140, 140, 148);
        for (int i = 0; i < kPresetCount; ++i) {
            int bx = menuX + U(10) + i * presetBtnW;
            int by = presetY + U(14);
            int bw = presetBtnW - U(4);
            int bh = U(22);
            // ����Ƿ�ǰƥ��
            bool match = true;
            for (int b = 0; b < 6; ++b) {
                if (std::abs(g_mpv->eqGain(b) - presets[i].bands[b]) > 0.5f) { match = false; break; }
            }
            SDL_SetRenderDrawColor(g_sdlRdr, match ? 59 : 48, match ? 130 : 48, match ? 246 : 52, 255);
            SDL_Rect btnRc = {bx, by, bw, bh};
            SDL_RenderFillRect(g_sdlRdr, &btnRc);
            g_text.drawText(bx + U(4), by + U(4), presets[i].name, Tpt(9),
                            match ? 255 : 180, match ? 255 : 180, match ? 255 : 186);
            // �洢��ť����
            static SDL_Rect s_presetRects[5];
            s_presetRects[i] = btnRc;
        }
    }
    // --- subtitle track popup menu ---
    if (g_ui.subMenuOpen) {
        auto subs = g_mpv->subTracks();
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32), menuW = U(180);
        int menuH = (int)(subs.size() + 2) * itemH + U(12);  // +1 off +1 load external
        int menuX = L.subBtn.x;
        int menuY = L.subBtn.y - menuH - U(6);
        if (menuY < 0) menuY = L.subBtn.y + L.subBtn.h + U(6);
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);
        // rounded rect bg
        int cr = U(8);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH2 = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH2);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);
        int curSubId = g_mpv->currentSubId();
        bool subVis = g_mpv->subVisible();
        // item 0: �ر���Ļ
        {
            int iy = menuY + U(6);
            bool sel = !subVis;
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            g_text.drawText(menuX + U(10), iy + U(6), T("�ر�", "Off"), Tpt(13), tr, tg, tb);
        }
        for (int i = 0; i < (int)subs.size(); ++i) {
            int iy = menuY + U(6) + (i + 1) * itemH;
            bool sel = (subs[i].id == curSubId && subVis);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            g_text.drawText(menuX + U(10), iy + U(6), subs[i].desc.c_str(), Tpt(13), tr, tg, tb);
        }
        // �ָ���
        int sepY = menuY + U(6) + (int)(subs.size() + 1) * itemH - U(2);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + U(10), sepY, menuX + menuW - U(10), sepY);
        // �����ⲿ��Ļ
        {
            int iy = menuY + U(6) + (int)(subs.size() + 1) * itemH;
            g_text.drawText(menuX + U(10), iy + U(6), T("�����ⲿ��Ļ...", "Load external..."),
                            Tpt(13), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        }
    }
    // --- audio track popup menu ---
    if (g_ui.audioMenuOpen) {
        auto tracks = g_mpv->audioTracks();
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32), menuW = U(180);
        int menuH = (int)tracks.size() * itemH + U(12);
        int menuX = L.audioBtn.x;
        int menuY = L.audioBtn.y - menuH - U(6);
        if (menuY < 0) menuY = L.audioBtn.y + L.audioBtn.h + U(6);
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);
        int cr = U(8);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH2 = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH2);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);
        int curAudioId = g_mpv->currentAudioTrack();
        for (int i = 0; i < (int)tracks.size(); ++i) {
            int iy = menuY + U(6) + i * itemH;
            bool sel = (tracks[i].id == curAudioId);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            g_text.drawText(menuX + U(10), iy + U(6), tracks[i].desc.c_str(), Tpt(13), tr, tg, tb);
        }
    }
    // --- chapter popup menu ---
    if (g_ui.chapterMenuOpen) {
        auto chs = g_mpv->chapters();
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32), menuW = U(240);
        int menuH = (int)chs.size() * itemH + U(12);
        int menuX = L.chapterBtn.x;
        int menuY = L.chapterBtn.y - menuH - U(6);
        if (menuY < 0) menuY = L.chapterBtn.y + L.chapterBtn.h + U(6);
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);
        int cr = U(8);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH2 = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH2);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);
        int curCh = g_mpv->currentChapter();
        for (int i = 0; i < (int)chs.size(); ++i) {
            int iy = menuY + U(6) + i * itemH;
            bool sel = (i == curCh);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            const char* name = chs[i].title.empty()
                ? T("�ޱ���", "Untitled") : chs[i].title.c_str();
            char label[128];
            std::snprintf(label, sizeof(label), "%d. %s", i + 1, name);
            g_text.drawText(menuX + U(10), iy + U(6), label, Tpt(13), tr, tg, tb);
        }
    }
    if (g_ui.playlistOpen) {
        int panelW, panelX;
        if (!g_ui.fullscreen) {
            panelW = totalW - w;                 // ������չ���Ķ�������
            panelX = w;
        } else {                                  // ȫ���޷�����: ����ʽ
            panelW = U(430);
            panelX = w - panelW;
        }
        if (panelW < U(200)) { panelW = U(200); panelX = w - panelW; }   // ����
        int panelH = h;
        int panelY = 0;

        // panel background����������͸����
        SDL_Rect pRc = {panelX, panelY, panelW, panelH};
        SDL_SetRenderDrawColor(g_sdlRdr, 16, 16, 17, 255);
        SDL_RenderFillRect(g_sdlRdr, &pRc);
        // left border
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
        SDL_RenderDrawLine(g_sdlRdr, panelX, panelY, panelX, panelY + panelH);

        // title + �ر�ť��Ч��ͼ .pl-head��
        g_text.drawText(panelX + U(14), panelY + U(16), i18n::playlist(), Tpt(13), 255, 255, 255);
        int closeX = panelX + panelW - U(44);
        int closeY = panelY + U(8);
        SDL_Rect closeRc = {closeX, closeY, U(36), U(36)};
        svgicon::draw(g_sdlRdr, "close", closeX + U(18), closeY + U(18), U(22),
                      255, 255, 255, 255);
        g_ui.plCloseRect = closeRc;

        // items from playlist queue����Ƭ��: thumb100��56+dur�Ǳ�+title+state��
        int itemY = panelY + U(45);
        int itemH = U(72);                       // ��Ƭ�߶�(56 thumb+padding)
        int scroll = g_ui.playlistScroll;
        std::vector<std::string> visiblePaths;
        // �ü����б���: ����ʱ���ݲ����ס�̶�������
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

            // hover ����������ڱ����ڣ�
            bool hovered = (g_ui.mouseX >= panelX + U(8) &&
                            g_ui.mouseX <= panelX + panelW - U(8) &&
                            g_ui.mouseY >= iy && g_ui.mouseY <= iy + itemH - U(6));
            if (isCurrent || hovered) {
                SDL_Rect hlRc = {panelX + U(7), iy, panelW - U(15), itemH - U(4)};
                if (isCurrent) SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 46);
                else           SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 15);
                SDL_RenderFillRect(g_sdlRdr, &hlRc);
            }

            // ����ͼ 100��56 r7������ռλ�� #26262c��#15151a ���ƣ�
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
            // dur �Ǳ�(right4 bottom4 ��.72)
            {
                char durBuf[16] = "";
                if (hpos > 1.0) {
                    std::snprintf(durBuf, sizeof(durBuf), "%02d:%02d",
                                  (int)(hpos / 60), (int)hpos % 60);
                    int dw = g_text.measureText(durBuf, Tpt(9)) + U(8);
                    int dx = thRc.x + thRc.w - dw - U(4);
                    int dy = thRc.y + thRc.h - U(18);
                    SDL_Rect db = {dx, dy, dw, U(15)};
                    SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 184);
                    SDL_RenderFillRect(g_sdlRdr, &db);
                    g_text.drawText(dx + U(4), dy + U(2), durBuf, Tpt(9), 255, 255, 255);
                }
            }

            // meta: title һ�� + state ��
            std::string fn = fileNameOf(p);
            int maxTw = panelW - U(140);
            if (maxTw < U(80)) maxTw = U(80);
            {
                // �����ؿ��ض�
                if (g_text.measureText(fn, Tpt(12)) > maxTw) {
                    while (fn.size() > 4 && g_text.measureText(fn + "...", Tpt(12)) > maxTw)
                        fn.pop_back();
                    fn += "...";
                }
                Uint8 tr = isCurrent ? 191 : 240, tg = isCurrent ? 214 : 240,
                      tb = isCurrent ? 255 : 240;   // playing #bfd6ff
                g_text.drawText(thRc.x + thRc.w + U(10), iy + U(10), fn, Tpt(12), tr, tg, tb);
            }
            // state: ���ڲ���(accent2)/�Ѳ���(#6b7280)/δ����(#3f3f46)
            {
                const char* st; Uint8 sr, sg_, sb_;
                if (isCurrent) { st = i18n::playing(); sr = 59; sg_ = 130; sb_ = 246; }
                else if (hpos > 1.0) { st = i18n::played(); sr = 107; sg_ = 114; sb_ = 128; }
                else { st = i18n::unplayed(); sr = 63; sg_ = 63; sb_ = 70; }
                g_text.drawText(thRc.x + thRc.w + U(10), iy + U(32), st, Tpt(11), sr, sg_, sb_);
            }
        }
        SDL_RenderSetClipRect(g_sdlRdr, nullptr);   // ����ü�(��קָʾ��/��������Խ��)

        // ��ק�����Ӿ�����������ָʾ�� + ���������
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

        // �ύ�ɼ���������ͼ worker����ȱͼ��; �ϲ�����, ����������ͼ������
        {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            for (auto& p : visiblePaths) {
                if (!g_thumbRgb.count(p) && !g_thumbTex.count(p) &&
                    std::find(g_thumbWant.begin(), g_thumbWant.end(), p) == g_thumbWant.end())
                    g_thumbWant.push_back(p);
            }
        }

        // scrollbar��M33d: ��ͣ����/��ק/�����ҳ��
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
                // hover/��קʱ����
                Uint8 ba = (g_ui.sbHover || g_ui.sbDragging) ? 160 : 70;
                SDL_SetRenderDrawColor(g_sdlRdr, 235, 235, 240, ba);
                SDL_Rect br = {trackX, barY, trackW, barH};
                SDL_RenderFillRect(g_sdlRdr, &br);

                // ��¶���θ����в���
                g_ui.sbTrackX = trackX; g_ui.sbTrackY = trackY;
                g_ui.sbTrackW = trackW; g_ui.sbTrackH = viewH;
                g_ui.sbBarY = barY;     g_ui.sbBarH = barH;
            } else {
                g_ui.sbTrackX = -1;
            }
        }

        if (g_playlist.empty()) {
            g_text.drawText(panelX + U(16), itemY + U(10), i18n::emptyPlaylist(), Tpt(12), 100, 100, 100);
        }
    }

    // --- settings modal panel ---
    if (g_ui.settingsOpen) {
        SettingsGeom sg = settingsGeom(w, h);

        // ģ̬����: ���͸��ѹ��(per-pixel alpha), ��Ƶ��Լ�ɼ�
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 140);
        SDL_Rect fullRc = {0, 0, w, h};
        SDL_RenderFillRect(g_sdlRdr, &fullRc);

        // �����Ӱ�������ɢ��
        for (int i = 4; i >= 1; --i) {
            Uint8 sha = (Uint8)(12 * i);
            SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, sha);
            SDL_Rect sr = {sg.panelX - i*2, sg.panelY - i*2, sg.panelW + i*4, sg.panelH + i*4};
            SDL_RenderDrawRect(g_sdlRdr, &sr);
        }

        // panel (Բ�Ǿ���)
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
        // �߿�
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 20);
        SDL_Rect borderH = {sg.panelX + cr, sg.panelY, sg.panelW - cr*2, sg.panelH};
        SDL_RenderDrawRect(g_sdlRdr, &borderH);
        SDL_Rect borderV = {sg.panelX, sg.panelY + cr, sg.panelW, sg.panelH - cr*2};
        SDL_RenderDrawRect(g_sdlRdr, &borderV);

        // title + close
        g_text.drawText(sg.panelX + U(20), sg.panelY + U(16), i18n::settingsTitle(), Tpt(16), 255, 255, 255);
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
            g_text.drawText(sg.panelX + U(20), ry + U(3), rowLabels[i], Tpt(13), on ? 230 : 170, on ? 230 : 170, on ? 230 : 170);

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

        // playback mode row (ѡ��=��ɫ����, δѡ��=�������ޱ߿�)
        g_text.drawText(sg.panelX + U(20), sg.modeRowY + U(3), i18n::playbackMode(), Tpt(13), 200, 200, 200);
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
            int tw = g_text.measureText(modes[i], Tpt(11));
            g_text.drawText(lx + (sg.chipW - tw) / 2, sg.chipY + U(4), modes[i], Tpt(11),
                            sel ? 255 : 150, sel ? 255 : 150, sel ? 255 : 150);
        }

        // �����л��� (ͬ���: ѡ��=��ɫ����, δѡ��=������)
        g_text.drawText(sg.panelX + U(20), sg.langRowY + U(3), i18n::language(), Tpt(13), 200, 200, 200);
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
            int tw2 = g_text.measureText(langLabels[i], Tpt(11));
            g_text.drawText(lx + (halfW - tw2) / 2, sg.langRowY + U(4), langLabels[i], Tpt(11),
                            sel ? 255 : 150, sel ? 255 : 150, sel ? 255 : 150);
        }
    }

    // --- toast notification��M32g ������ʽ: ����Բ����� + ���֣� ---
    if (g_ui.toastActive) {
        Uint32 elapsed = SDL_GetTicks() - g_ui.toastStart;
        if (elapsed > ui::TOAST_MS) {
            g_ui.toastActive = false;
        } else {
            float alpha = 1.0f;
            if (elapsed > ui::TOAST_MS - 300)
                alpha = 1.0f - (float)(elapsed - (ui::TOAST_MS - 300)) / 300.0f;
            Uint8 a = (Uint8)(alpha * 255);
            int tw = g_text.measureText(g_ui.toastMsg, Tpt(13));
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
            g_text.drawText(w / 2 - tw / 2, by + U(8), g_ui.toastMsg, Tpt(13), 255, 255, 255);
        }
    }

    // --- OSD ��Ϣ���ӣ��� I �л���8 ���Զ���ʧ�� ---
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

            char line1[128] = {}, line2[64] = {}, line3[96] = {}, line4[64] = {};
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
            const char* hwPath = g_mpv->hwdecCurrent();
            if (hwPath && hwPath[0] && std::strcmp(hwPath, "no") != 0)
                std::snprintf(line4, sizeof(line4), "hwdec: %s%s", hwPath,
                              g_mpv->hwdecRetryCount() > 0 ? " (fallback)" : "");

            // ���ߴ�������
            int lines = 0;
            if (line1[0]) ++lines;
            if (line2[0]) ++lines;
            if (line3[0]) ++lines;
            if (line4[0]) ++lines;
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
                if (line1[0]) { g_text.drawText(boxX + padX, ty, line1, Tpt(12), 255, 255, 255); ty += lineH; }
                if (line2[0]) { g_text.drawText(boxX + padX, ty, line2, Tpt(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B); ty += lineH; }
                if (line3[0]) { g_text.drawText(boxX + padX, ty, line3, Tpt(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B); ty += lineH; }
                if (line4[0]) { g_text.drawText(boxX + padX, ty, line4, Tpt(12), 0, 200, 120); }
            }
        }
    }

    overlayPresent();
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
        wc.lpszClassName, L"��Ӱ��Ƶ",
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
        // �������¼��߳�ֻͶ�ݴ� seek λ��, UI ��ѭ��ִ��
        // (�¼��߳�ֱ�ӵ� mpv.seek/showToast ���� UI �߳̾���)
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

    if (!mpv.init(g_mpvHwnd, g_cfg.enableZeroCopy != 0)) { LOG_ERROR("MAIN", "mpv init failed"); return 1; }

    // ������Ӧ������ʱѡ��
    mpvSetOpt("hwdec", g_cfg.hwDecode ? (g_cfg.enableZeroCopy ? "auto-safe" : "auto-copy-safe") : "no");
    mpvSetOpt("sub-auto", g_cfg.subAutoLoad ? "fuzzy" : "no");
    mpvSetOpt("audio-exclusive", g_cfg.audioExclusive ? "yes" : "no");
    rebuildAudioFilters();
    if (g_cfg.motionInterp) applyMotionInterp(true);
    if (g_cfg.hiQScale) {
        mpvSetOpt("scale", "ewa_lanczossharp");
        mpvSetOpt("cscale", "ewa_lanczossharp");
    }

    // ---- SDL2 overlay��owned ���㴰�ڣ������������� ----
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
        // �ָ��ϴβ��ţ������ؽ���������Ŀ¼
        buildPlaylistAround(initialFile);
        playPath(initialFile);
    } else if (!initialFile.empty()) {
        buildPlaylistAround(initialFile);
        playPath(initialFile);
    }

    LOG_INFO("MAIN", "entering main loop (playlist=%d)", (int)g_playlist.size());

    // ����ͼ worker�������̻���Ŀ¼��
    CreateDirectoryA((exeDir() + "cache").c_str(), nullptr);
    CreateDirectoryA(thumbCacheDir().c_str(), nullptr);
    thumbCacheCleanup(7);
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
            {
                std::lock_guard<std::mutex> lk(g_autoNextMtx);
                if (g_resumeSeekPending) { pos = g_resumeSeekPos; g_resumeSeekPending = false; }
            }
            if (g_needsUnpause && g_mpv && g_mpv->hasMedia()) {
                if (pos > 1.0) {
                    LOG_INFO("MAIN", "resume at %.1fs", pos);
                    g_mpv->seek(pos);
                    char msg[48];
                    std::snprintf(msg, sizeof(msg), "%s %02d:%02d",
                        T("������", "Resumed at"), (int)(pos / 60), (int)pos % 60);
                    showToast(msg);
                }
                // P4-1: seek ��ɺ� unpause (�� resume Ҳ unpause)
                g_mpv->unpause();
                g_needsUnpause = false;
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
