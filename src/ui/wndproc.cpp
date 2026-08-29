#include "ui/wndproc.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <atomic>

#include "app/app_state.h"
#include "ui/helpers.h"
#include "ui/dialogs.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/logger.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

LRESULT CALLBACK parentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    g_dirty.store(true);   // 锟轿猴拷锟斤拷息锟斤拷锟斤拷为潜锟斤拷锟接撅拷锟戒化锟斤拷锟斤拷锟酵骋伙拷锟斤拷啵?
    switch (msg) {

    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0;
        RECT rc; GetClientRect(hwnd, &rc);
        g_ui.totalW = rc.right;
        // 锟叫憋拷锟斤拷锟斤拷(锟斤拷全锟斤拷)时: 锟揭诧拷锟斤拷锟斤拷锟斤拷锟? mpv/overlay 只占锟斤拷频锟斤拷
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
        // 锟斤拷时锟截伙拷 锟斤拷 锟斤拷锟斤拷锟斤拷循锟斤拷锟斤拷锟斤拷
        renderOverlay();
        return 0;
    }
    case WM_DPICHANGED: {
        // 锟斤拷示锟斤拷 DPI 锟戒化锟斤拷锟较碉拷锟斤拷同锟斤拷锟斤拷锟斤拷/锟斤拷系统锟斤拷锟脚ｏ拷
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

    // ---- 锟斤拷锟斤拷锟斤拷锟? 锟斤拷锟斤拷锟斤拷失去锟斤拷锟斤拷时锟斤拷锟斤拷 overlay, 锟斤拷锟解浮锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷 ----
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
            case 0xDB:   // VK_OEM_4: Windows 锟斤拷 [ 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷(锟斤拷ASCII 0x5B)
            {
                g_mpv->setSpeed(g_mpv->speed() - 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("锟斤拷锟斤拷", "Speed"), g_mpv->speed());
                showToast(msg);
                break;
            }
            case ']':
            case 0xDD:   // VK_OEM_6
            {
                g_mpv->setSpeed(g_mpv->speed() + 0.25f);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("锟斤拷锟斤拷", "Speed"), g_mpv->speed());
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
                std::snprintf(msg, sizeof(msg), "%s: %.1fs", T("锟斤拷幕锟接筹拷", "Sub delay"), -g_mpv->subDelay());
                showToast(msg);
                break;
            }
            case 'Z': {
                g_mpv->addSubDelay(0.5);
                char msg[40];
                std::snprintf(msg, sizeof(msg), "%s: %.1fs", T("锟斤拷幕锟接筹拷", "Sub delay"), -g_mpv->subDelay());
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
                    // Shift+S: 锟斤拷锟斤拷锟解部锟斤拷幕锟侥硷拷
                    if (g_mpv && g_mpv->hasMedia()) {
                        std::string f = openSubtitleDialog(hwnd);
                        if (!f.empty()) {
                            g_mpv->loadSubtitle(f);
                            showToast(T("锟斤拷幕锟窖硷拷锟斤拷", "Subtitle loaded"));
                        }
                    }
                } else {
                    // S: 锟叫伙拷锟斤拷幕锟缴硷拷锟斤拷
                    bool vis = !g_mpv->subVisible();
                    g_mpv->setSubVisibility(vis);
                    showToast(vis ? i18n::subtitlesOn() : i18n::subtitlesOff());
                }
                break;
            case 'A': {  // AB 循锟斤拷: 锟斤拷一锟斤拷锟斤拷 A, 锟节讹拷锟斤拷锟斤拷 B, 锟斤拷锟斤拷锟斤拷锟斤拷锟?
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
            case 'G': {  // 锟铰斤拷锟斤拷转: 锟斤拷锟斤拷锟斤拷一锟斤拷
                auto chs = g_mpv->chapters();
                if (!chs.empty()) {
                    int cur = g_mpv->currentChapter();
                    int next = (cur + 1) % (int)chs.size();
                    g_mpv->seekToChapter(next);
                    char msg[64];
                    std::snprintf(msg, sizeof(msg), "%s %d/%d: %s",
                                 T("锟铰斤拷", "Chapter"),
                                 next + 1, (int)chs.size(),
                                 chs[next].title.empty() ? T("锟睫憋拷锟斤拷", "Untitled") : chs[next].title.c_str());
                    showToast(msg);
                }
                break;
            }
            case 'V': {  // 锟斤拷锟斤拷锟叫伙拷: 循锟斤拷锟斤拷一锟斤拷锟斤拷
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
                                 T("锟斤拷锟斤拷", "Audio"),
                                 tracks[nextIdx].desc.empty() ? T("默锟斤拷", "Default") : tracks[nextIdx].desc.c_str());
                    showToast(msg);
                } else {
                    showToast(i18n::singleTrack());
                }
                break;
            }
            case 'B': {  // 锟斤拷幕位锟斤拷: 循锟斤拷锟阶诧拷/锟斤拷锟斤拷/锟斤拷锟斤拷
                static int subPosIdx = 0;
                int positions[] = {100, 50, 10};
                const char* names[] = {i18n::subBottom(), i18n::subCenter(), i18n::subTop()};
                subPosIdx = (subPosIdx + 1) % 3;
                g_mpv->setSubPos(positions[subPosIdx]);
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %s", T("锟斤拷幕", "Sub"), names[subPosIdx]);
                showToast(msg);
                break;
            }
            case 'D': {  // 去色锟斤拷强锟斤拷: 锟截★拷锟斤拷锟斤拷小锟角匡拷锟斤拷锟?..
                int cur = g_mpv->debandLevel();
                int next = (cur + 1) % 4;
                g_mpv->setDebandLevel(next);
                const char* names[] = {i18n::debandOff(), i18n::debandLight(), i18n::debandMedium(), i18n::debandStrong()};
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %s", T("去色锟斤拷", "Deband"), names[next]);
                showToast(msg);
                break;
            }
            case 'E': {  // 锟斤拷频锟斤拷锟斤拷锟斤拷: 锟斤拷/锟截闭碉拷锟斤拷
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
                // Insert: 锟斤拷锟斤拷锟侥硷拷锟斤拷锟斤拷锟斤拷锟叫憋拷
                if (g_mpv && g_mpv->hasMedia()) {
                    std::string f = openFileDialog(hwnd);
                    if (!f.empty()) {
                        addToPlaylist(f);
                        if (!g_ui.playlistOpen) g_ui.playlistOpen = true;
                        showToast(T("锟斤拷锟斤拷锟接碉拷锟斤拷锟斤拷锟叫憋拷", "Added to playlist"));
                    }
                }
                break;
            }
            case VK_DELETE: {
                // Delete: 锟接诧拷锟斤拷锟叫憋拷锟狡筹拷锟斤拷前锟斤拷
                if (g_mpv && g_mpv->hasMedia() && g_playlist.size() > 1) {
                    int curIdx = playlistIndexOf(g_mpv->path());
                    if (curIdx >= 0) {
                        std::string fn = fileNameOf(g_playlist[curIdx]);
                        removeFromPlaylist(curIdx);
                        char msg[128];
                        std::snprintf(msg, sizeof(msg), "%s: %s",
                                      T("锟斤拷锟狡筹拷", "Removed"), fn.c_str());
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

        // seekbar 锟斤拷拽: 锟斤拷锟斤拷锟斤拷锟斤拷 seekTarget
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

        // 锟斤拷锟斤拷锟斤拷锟斤拷 hover 锟皆讹拷展锟斤拷锟斤拷锟诫开 0.5s 锟斤拷锟斤拷锟斤拷锟斤拷拽锟叫诧拷锟秸ｏ拷
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

        // 锟斤拷锟斤拷锟斤拷锟斤拷 hover 锟斤拷锟斤拷 (锟斤拷锟接撅拷, 锟斤拷锟斤拷锟斤拷锟斤拷; 锟斤拷锟斤拷锟斤拷锟斤拷只锟节碉拷锟斤拷锟阶?
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

        // 锟叫憋拷锟斤拷锟斤拷锟斤拷: hover 锟斤拷锟斤拷 + 锟斤拷拽锟斤拷锟斤拷
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

        // 锟叫憋拷锟斤拷拽锟斤拷锟斤拷位锟狡筹拷锟斤拷值锟斤拷锟斤拷锟斤拷拽态
        if (!g_ui.sbDragging && g_ui.plDragFrom >= 0 && !g_ui.plDragging &&
            std::abs(g_ui.mouseY - g_ui.plDownY) > U(8)) {
            g_ui.plDragging = true;
            LOG_DBG("MAIN", "playlist drag start from=%d", g_ui.plDragFrom);
        }
        if (g_ui.plDragging) {
            g_ui.plDragY = g_ui.mouseY;
            // 锟皆讹拷锟斤拷锟斤拷锟斤拷锟较碉拷锟斤拷锟斤拷锟斤拷卤锟皆凳憋拷锟斤拷锟斤拷斜锟?
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
        // 锟睫边匡拷锟皆绘：锟酵伙拷锟斤拷=锟斤拷锟斤拷锟斤拷锟斤拷(锟狡筹拷系统锟斤拷锟斤拷锟斤拷)锟斤拷锟斤拷锟斤拷 DWM 锟斤拷影
        if (!wp) break;
        auto* params = (NCCALCSIZE_PARAMS*)lp;
        if (IsZoomed(hwnd)) {   // 锟斤拷锟绞憋拷战锟斤拷锟侥伙拷呖锟斤拷锟?
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
        // 锟斤拷缘锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷全锟斤拷/锟斤拷锟姐）
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
        // 锟斤拷锟斤拷锟斤拷锟斤拷图锟斤拷锟斤拷锟斤拷为锟斤拷拽锟斤拷锟斤拷
        // 模态锟斤拷锟?锟剿碉拷锟斤拷时, 锟斤拷锟斤拷锟斤拷为 HTCAPTION(锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷诠乇瞻锟脚?
        // 锟斤拷锟斤拷锟叫憋拷锟斤拷锟斤拷也锟斤拷锟斤拷锟斤拷(锟叫憋拷锟截闭帮拷钮锟斤拷 topbar Y 锟斤拷围锟斤拷)
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

        // --- 锟斤拷锟斤拷锟叫憋拷锟截憋拷钮: 锟斤拷锟斤拷锟斤拷燃锟?---
        // (y 锟斤拷 topbar 锟竭讹拷锟节会被 topbar 锟斤拷支锟斤拷锟斤拷: 锟斤拷锟斤拷模式锟斤拷锟斤拷拽, 全锟斤拷时锟斤拷
        //  应锟矫关憋拷图锟斤拷锟截碉拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟?
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

        // --- topbar icon clicks (锟叫憋拷展锟斤拷时锟叫憋拷锟斤拷锟斤拷锟斤拷锟斤拷 topbar) ---
        // 模态锟斤拷锟?锟剿碉拷锟斤拷时, topbar 锟斤拷锟斤拷应锟斤拷锟?锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷诳锟斤拷系墓乇瞻锟脚?
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
            case 3: // playlist锟斤拷锟揭诧拷锟斤拷锟斤拷锟斤拷颍捍锟斤拷锟斤拷锟秸癸拷锟?
                g_ui.playlistOpen = !g_ui.playlistOpen;
                LOG_INFO("MAIN", "pl toggle -> %d (mx=%d my=%d winW=%d)",
                         g_ui.playlistOpen ? 1 : 0, mx, my, g_ui.winW);
                if (!g_ui.fullscreen) applyPlaylistWindow(hwnd);
                else g_dirty.store(true);
                return 0;
            case 4: { // PIP 锟矫讹拷锟斤拷锟斤拷小锟斤拷
                if (g_mpv && g_mpv->hasMedia()) {
                    if (g_ui.fullscreen) toggleFullscreen(hwnd);  // 全锟斤拷锟斤拷锟剿筹拷
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

        // --- M36: 锟斤拷迎页锟斤拷锟斤拷锟斤拷锟斤拷媒锟斤拷时锟斤拷: Hero 锟斤拷钮 / 锟斤拷锟斤拷锟桔匡拷 / 锟斤拷锟斤拷锟斤拷锟斤拷 ---
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
                    playPath(kv.first, true);   // forceResume: welcome 卡片始终恢复进度
                    return 0;
                }
            }
            for (auto& kv : g_ui.gridHits) {
                if (kv.first >= 0 && kv.first < (int)g_playlist.size() && inRc(kv.second)) {
                    std::string p = g_playlist[kv.first];   // 锟斤拷锟节讹拷锟斤拷, 锟斤拷锟截斤拷
                    playPath(p);
                    return 0;
                }
            }
        }

        // --- settings modal: 锟斤拷锟斤拷锟斤拷燃锟斤拷锟斤拷锟斤拷撞锟斤拷锟斤拷芨锟斤拷强锟斤拷锟斤拷锟?锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷,
        //     锟斤拷锟斤拷锟饺达拷锟斤拷, 锟斤拷锟斤拷锟斤拷/模式锟叫的碉拷锟斤拷岜伙拷锟斤拷锟斤拷锟斤拷缘锟斤拷锟?---
        if (g_ui.settingsOpen) {
            SettingsGeom sg = settingsGeom(g_ui.winW, g_ui.winH);
            bool inside = (mx >= sg.panelX && mx <= sg.panelX + sg.panelW &&
                           my >= sg.panelY && my <= sg.panelY + sg.panelH);
            LOG_DBG("MAIN", "settings click: mx=%d my=%d panel(%d,%d,%d,%d) closeC(%d,%d,R=%d) inside=%d",
                    mx, my, sg.panelX, sg.panelY, sg.panelW, sg.panelH,
                    sg.closeCx, sg.closeCy, sg.closeR, inside ? 1 : 0);
            if (!inside) {
                LOG_DBG("MAIN", "settings close: click outside panel");
                g_ui.settingsOpen = false;      // 锟斤拷锟斤拷 = 锟截憋拷(锟斤拷锟斤拷锟斤拷锟斤拷麓锟?
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
                // 锟斤拷锟斤拷锟叫伙拷
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
            return 0;   // 锟斤拷锟矫达拷锟节硷拷锟斤拷锟斤拷锟斤拷锟铰达拷锟斤拷锟斤拷锟斤拷锟斤拷/锟斤拷锟斤拷锟斤拷/锟斤拷频
        }

        // --- 锟斤拷锟斤拷锟剿碉拷模态锟斤拷(锟斤拷锟斤拷/锟斤拷锟斤拷/EQ/锟斤拷幕/锟斤拷锟斤拷): 锟斤拷锟节硷拷锟斤拷锟斤拷锟斤拷只锟斤拷锟斤拷锟节碉拷锟斤拷 ---
        // 锟斤拷锟斤拷说锟斤拷锟?锟斤拷效; 锟斤拷锟斤拷说锟斤拷锟?锟斤拷锟截闭★拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷频锟斤拷停/锟斤拷锟斤拷锟斤拷/锟斤拷钮,
        // 锟斤拷锟斤拷"锟斤拷锟斤拷锟斤拷锟斤拷透锟斤拷锟矫碉拷锟斤拷锟斤拷"锟斤拷锟斤拷锟解。
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
                        std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("锟斤拷锟斤拷", "Speed"), SPEED_PRESETS[idx]);
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
                        const char* qNames[] = { T("省锟斤拷", "Power Saving"), T("锟斤拷准", "Standard"), T("锟斤拷锟斤拷", "Ultimate") };
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
                        // P1-6: EQ 预锟斤拷锟斤拷
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
                        // 锟剿碉拷锟斤拷锟斤拷锟斤拷位锟斤拷: 锟斤拷锟街达拷
                        g_ui.visible = true;
                        g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
                        g_dirty.store(true);
                        return 0;
                    }
                } else {
                    g_ui.eqMenuOpen = false;
                }
            }
            // --- 锟斤拷幕锟斤拷选锟斤拷说锟?---
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
                        std::snprintf(msg, sizeof(msg), "%s: %s", T("锟斤拷幕", "Subtitle"), subs[idx - 1].desc.c_str());
                        showToast(msg);
                    } else if (idx == (int)subs.size() + 1) {
                        // 锟斤拷锟斤拷锟解部锟斤拷幕
                        g_ui.subMenuOpen = false;
                        if (g_mpv->hasMedia()) {
                            std::string f = openSubtitleDialog(hwnd);
                            if (!f.empty()) {
                                g_mpv->loadSubtitle(f);
                                showToast(T("锟斤拷幕锟窖硷拷锟斤拷", "Subtitle loaded"));
                            }
                        }
                    }
                }
                g_ui.subMenuOpen = false;
            }
            // --- 锟斤拷锟斤拷选锟斤拷说锟?---
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
                        std::snprintf(msg, sizeof(msg), "%s: %s", T("锟斤拷锟斤拷", "Audio"), tracks[idx].desc.c_str());
                        showToast(msg);
                    }
                }
                g_ui.audioMenuOpen = false;
            }
            // --- 锟铰斤拷选锟斤拷说锟?---
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
                            ? T("锟睫憋拷锟斤拷", "Untitled") : chs[idx].title.c_str();
                        char msg[128];
                        std::snprintf(msg, sizeof(msg), "%s %d: %s",
                                      T("锟铰斤拷", "Chapter"), idx + 1, name);
                        showToast(msg);
                    }
                }
                g_ui.chapterMenuOpen = false;
            }
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            g_dirty.store(true);
            return 0;   // 锟斤拷锟斤拷锟斤拷诩锟斤拷锟揭伙拷锟斤拷锟斤拷锟?
        }

        // --- seekbar锟斤拷锟斤拷直锟捷诧拷锟秸斤拷: 锟斤拷探锟斤拷锟斤拷锟皆碉拷锟矫伙拷锟姐按钮/锟斤拷频锟侥碉拷锟斤拷锟?--
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
            return 0;  // seekbar 锟斤拷锟斤拷锟斤拷锟酵革拷锟斤拷锟狡碉拷锟?
        }
        // --- controlbar row1 锟斤拷锟叫ｏ拷锟斤拷锟斤拷染锟斤拷锟斤拷 Row1Layout锟斤拷---
        {
            bool volOpen = (g_ui.volumeSliderOpen || g_ui.volumeDragging);
            Row1Layout L;
            layoutRow1(g_ui.winW, sbTopY(), volOpen, L);
            auto inRc = [&](const SDL_Rect& r) {
                return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
            };
            // 锟斤拷锟斤拷锟斤拷锟斤拷(展锟斤拷时)锟斤拷拽锟斤拷锟?锟斤拷锟斤拷 锟斤拷锟斤拷图锟斤拷锟叫讹拷; 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷 锟斤拷U(20)
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
                if (idx > 0) { playIndex(idx - 1); showToast(T("锟斤拷一锟斤拷", "Previous")); }
                else showToast(i18n::noPrev());
            }
            else if (inRc(L.play)) {
                g_mpv->togglePause();
            }
            else if (inRc(L.next)) {
                int idx = playlistIndexOf(g_mpv->path());
                int n = (int)g_playlist.size();
                if (idx >= 0 && idx + 1 < n) { playIndex(idx + 1); showToast(T("锟斤拷一锟斤拷", "Next")); }
                else showToast(i18n::noNext());
            }
            else if (inRc(L.subBtn)) {
                auto subs = g_mpv->subTracks();
                if (subs.size() > 1) {
                    // 锟斤拷锟斤拷幕锟斤拷: 锟斤拷选锟斤拷说锟?
                    g_ui.subMenuOpen = !g_ui.subMenuOpen;
                    g_ui.audioMenuOpen = false;
                    g_ui.chapterMenuOpen = false;
                } else {
                    // 锟斤拷锟斤拷/锟睫癸拷: 直锟斤拷锟叫伙拷锟缴硷拷锟斤拷
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
                    showToast(T("锟斤拷锟铰斤拷锟斤拷息", "No chapters"));
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
        // (锟斤拷锟斤拷/锟斤拷锟斤拷/EQ 锟剿碉拷锟斤拷锟斤拷锟斤拷锟斤拷模态锟斤拷统一锟斤拷锟斤拷, 锟剿达拷锟斤拷锟劫可达拷)
        // --- 锟斤拷锟斤拷图锟斤拷锟斤拷锟斤拷锟叫伙拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷 hover 展锟斤拷锟斤拷 ---
        if (g_mpv && mx >= g_ui.winW - U(68) && mx <= g_ui.winW - U(40) &&
                 my >= barTop + U(36) && my <= barTop + U(64)) {
            g_mpv->toggleMute();
            showToast(g_mpv->muted() ? i18n::muted() : i18n::unmuted());
            LOG_INFO("MAIN", "mute toggled -> %d", g_mpv->muted() ? 1 : 0);
        }
        // --- 锟斤拷锟斤拷锟叫憋拷锟斤拷锟斤拷锟斤拷颍汗乇锟脚?-> 锟斤拷锟斤拷锟斤拷 -> 锟叫憋拷锟斤拷锟窖?---
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
                    // 锟斤拷锟斤拷锟揭? bar 锟斤拷锟侥讹拷锟斤拷锟斤拷锟斤拷
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
                int rel = my - U(45) + g_ui.playlistScroll;   // panelY=0 锟斤拷锟斤拷染一锟斤拷
                g_ui.plDragFrom = -1; g_ui.plDragging = false;
                if (rel >= 0) {
                    int itemIdx = rel / itemH;
                    if (itemIdx < (int)g_playlist.size()) {
                        g_ui.plDragFrom = itemIdx;
                        g_ui.plDownY = my;
                        SetCapture(hwnd);   // 锟斤拷拽/锟斤拷锟街讹拷锟斤拷锟斤拷锟斤拷锟揭诧拷芨锟斤拷锟?
                    }
                }
            }
        }
        // --- click on video area锟斤拷锟接筹拷执锟斤拷锟斤拷停锟斤拷双锟斤拷锟斤拷锟斤拷全锟斤拷锟斤拷 ---
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
        // 锟斤拷锟斤拷锟斤拷锟斤拷锟斤拷双锟斤拷 -> 锟斤拷转锟斤拷锟斤拷锟轿伙拷锟?
        if (g_mpv && g_mpv->duration() > 0 &&
            my >= barTop - U(8) && my <= barTop + U(12) &&
            mx >= sbLeftX() && mx <= sbRightX()) {
            double ratio = (double)(mx - sbLeftX()) / sbWidth();
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_mpv->seek(g_mpv->duration() * ratio);
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
        }
        // 锟斤拷频锟斤拷双锟斤拷 -> 全锟斤拷锟叫伙拷锟斤拷取锟斤拷锟斤拷锟斤拷锟斤拷停锟斤拷
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
        // 锟叫憋拷锟斤拷拽锟斤拷位 / 锟斤拷锟斤拷锟斤拷锟斤拷
        if (g_ui.plDragFrom >= 0) {
            if (g_ui.plDragging) {
                int itemH = U(72);
                int topY = U(45);   // panelY=0 锟斤拷锟斤拷染一锟斤拷
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
                playIndex(g_ui.plDragFrom);   // 未锟较讹拷 = 锟斤拷锟斤拷锟斤拷锟斤拷
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

        // 锟斤拷锟斤拷锟叫憋拷锟斤拷锟斤拷锟斤拷颍汗锟斤拷锟斤拷斜锟?
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
            std::snprintf(msg, sizeof(msg), "%s %d%%", T("锟斤拷锟斤拷", "Volume"), (int)(g_mpv->volume() * 100 + 0.5f));
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
            // 锟斤拷锟侥硷拷: 锟斤拷锟斤拷锟斤拷锟斤拷锟叫憋拷, 锟斤拷锟脚碉拷一锟斤拷
            g_playlist = droppedFiles;
            playPath(firstPath);
            if (!g_ui.playlistOpen) g_ui.playlistOpen = true;
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_CLOSE:
        saveWindowPos(hwnd);          // 锟斤拷锟斤拷前抓取位锟斤拷
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
