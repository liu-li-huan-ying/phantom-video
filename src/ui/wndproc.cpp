#include "ui/wndproc.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <atomic>

#include "app/app_state.h"
#include "ui/helpers.h"
#include "ui/dialogs.h"
#include "ui/menus.h"
#include "ui/shortcuts.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/logger.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

LRESULT CALLBACK parentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    g_dirty.store(true);   // Mark overlay dirty so it redraws on next idle
    switch (msg) {

    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0;
        RECT rc; GetClientRect(hwnd, &rc);
        g_ui.totalW = rc.right;
        // Playlist panel extra width (fullscreen: no extra)
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
        // Force render during WM_SIZE to avoid flicker
        renderOverlay();
        return 0;
    }
    case WM_DPICHANGED: {
        // DPI changed: update scale factor and resize window
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

    // ---- WM_ACTIVATEAPP: show/hide overlay on focus change ----
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
        handleKeyboard(hwnd, wp, lp);
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

        // Seekbar drag: update seekTarget from mouse position
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

        // Volume slider: auto-expand on hover, collapse after 0.5s idle
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

        // Volume slider hover hit-test (expanded area, collapsed: only slider)
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

        // Playlist scrollbar: hover highlight + drag tracking
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

        // Playlist drag: check if movement exceeds threshold to enter drag mode
        if (!g_ui.sbDragging && g_ui.plDragFrom >= 0 && !g_ui.plDragging &&
            std::abs(g_ui.mouseY - g_ui.plDownY) > U(8)) {
            g_ui.plDragging = true;
            LOG_DBG("MAIN", "playlist drag start from=%d", g_ui.plDragFrom);
        }
        if (g_ui.plDragging) {
            g_ui.plDragY = g_ui.mouseY;
            // Auto-scroll playlist when dragging near top/bottom edge
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
        // 图像面板滑块拖拽
        if (g_ui.imageDraggingSlider >= 0 && g_mpv) {
            int panelW = U(380);
            int panelX = g_ui.winW / 2 - panelW / 2;
            int sliderX = panelX + U(90);
            int sliderW = panelW - U(180);
            float norm = (float)(g_ui.mouseX - sliderX) / sliderW;
            if (norm < 0.0f) norm = 0.0f; if (norm > 1.0f) norm = 1.0f;
            int val = (int)(norm * 200.0f - 100.0f);
            const char* keys[] = {"brt", "con", "sat", "gam"};
            applySetting(keys[g_ui.imageDraggingSlider], val);
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
        // NCCALCSIZE: client=window minus frame (system border + DWM shadow)
        if (!wp) break;
        auto* params = (NCCALCSIZE_PARAMS*)lp;
        if (IsZoomed(hwnd)) {   // Maximized: remove frame borders
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
        // Resize borders (8px, excludes fullscreen/mini mode)
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
        // Title area: drag window by default
        // Modal menus open: treat as HTCAPTION (not titlebar area)
        // But playlist close button (within topbar Y range) excluded
        {
            bool anyModalOpen = g_ui.settingsOpen || g_ui.speedMenuOpen || g_ui.qualityMenuOpen ||
                                g_ui.eqMenuOpen || g_ui.subMenuOpen || g_ui.audioMenuOpen ||
                                g_ui.chapterMenuOpen || g_ui.imageMenuOpen;
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

        // --- Playlist close button: handle first ---
        // (y is below topbar, caught by topbar handler: mini mode drag, fullscreen)
        //  Should use close icon hit-test for playlist toggle
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

        // --- topbar icon clicks (expanded panel area excluded from topbar) ---
        // Modal menus open: topbar should not trigger titlebar drag
        bool inPlaylistArea = (g_ui.playlistOpen && !g_ui.fullscreen && mx >= g_ui.winW);
        bool anyModalOpen = g_ui.settingsOpen || g_ui.speedMenuOpen || g_ui.qualityMenuOpen ||
                            g_ui.eqMenuOpen || g_ui.subMenuOpen || g_ui.audioMenuOpen ||
                            g_ui.chapterMenuOpen || g_ui.imageMenuOpen;
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
            case 3: // playlist toggle: also adjust panel size
                g_ui.playlistOpen = !g_ui.playlistOpen;
                LOG_INFO("MAIN", "pl toggle -> %d (mx=%d my=%d winW=%d)",
                         g_ui.playlistOpen ? 1 : 0, mx, my, g_ui.winW);
                if (!g_ui.fullscreen) applyPlaylistWindow(hwnd);
                else g_dirty.store(true);
                return 0;
            case 4: { // PIP: enter mini mode
                if (g_mpv && g_mpv->hasMedia()) {
                    if (g_ui.fullscreen) toggleFullscreen(hwnd);  // Exit fullscreen first
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

        // --- M36: Welcome page clicks: Hero buttons / continue watching / grid ---
        if (handleWelcomeClick(hwnd, mx, my)) {
            return 0;
        }

        // --- settings modal: ---
        if (g_ui.settingsOpen) {
            handleSettingsClick(hwnd, mx, my);
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            return 0;
        }

        // --- popup menus (speed/quality/EQ/image/sub/audio/chapter): ---
        if (handleMenuClicks(hwnd, mx, my)) {
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            g_dirty.store(true);
            return 0;
        }

        // --- seekbar: direct click (higher priority than ctrlBar/video area) ---
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
            return 0;  // seekbar click takes priority over video area
        }
        // --- controlbar row1 buttons: ---
        if (handleControlBarClick(hwnd, mx, my)) {
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
            return 0;
        }
    videoAreaClick:;
        // (Speed/Quality/EQ menus are modal, handled above in handleMenuClicks)
        // --- Mute button click area (volume icon hover region) ---
        if (g_mpv && mx >= g_ui.winW - U(68) && mx <= g_ui.winW - U(40) &&
                 my >= barTop + U(36) && my <= barTop + U(64)) {
            g_mpv->toggleMute();
            showToast(g_mpv->muted() ? i18n::muted() : i18n::unmuted());
            LOG_INFO("MAIN", "mute toggled -> %d", g_mpv->muted() ? 1 : 0);
        }
        // --- Playlist scrollbar click -> seekbar track -> playlist item ---
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
                    // Click outside scrollbar bar: jump to position
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
                int rel = my - U(45) + g_ui.playlistScroll;   // panelY=0, offset by one header
                g_ui.plDragFrom = -1; g_ui.plDragging = false;
                if (rel >= 0) {
                    int itemIdx = rel / itemH;
                    if (itemIdx < (int)g_playlist.size()) {
                        g_ui.plDragFrom = itemIdx;
                        g_ui.plDownY = my;
                        SetCapture(hwnd);   // Drag/keyboard: capture mouse to track outside window
                    }
                }
            }
        }
        // --- video area click: single click = pause, double = fullscreen ---
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
        // Seekbar double-click -> seek to position
        if (g_mpv && g_mpv->duration() > 0 &&
            my >= barTop - U(8) && my <= barTop + U(12) &&
            mx >= sbLeftX() && mx <= sbRightX()) {
            double ratio = (double)(mx - sbLeftX()) / sbWidth();
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            g_mpv->seek(g_mpv->duration() * ratio);
            g_ui.visible = true;
            g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
        }
        // Video area double-click -> fullscreen, cancel pending pause
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
        if (g_ui.imageDraggingSlider >= 0) {
            g_ui.imageDraggingSlider = -1;
        }
        if (g_ui.sbDragging) {
            g_ui.sbDragging = false;
        }
        // Playlist drag release / item click
        if (g_ui.plDragFrom >= 0) {
            if (g_ui.plDragging) {
                int itemH = U(72);
                int topY = U(45);   // panelY=0, offset by one header
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
                playIndex(g_ui.plDragFrom);   // Not dragged = click to play
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

        // Playlist area: scroll list vertically
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
            // Multi-file: merge into playlist, play first
            g_playlist = droppedFiles;
            playPath(firstPath);
            if (!g_ui.playlistOpen) g_ui.playlistOpen = true;
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_CLOSE:
        saveWindowPos(hwnd);          // Save window position before closing
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
