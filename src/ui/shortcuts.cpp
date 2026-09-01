#include "ui/shortcuts.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "app/app_state.h"
#include "ui/helpers.h"
#include "ui/dialogs.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/mpv_backend.h"

bool handleKeyboard(HWND hwnd, WPARAM wp, LPARAM lp) {
    if (!g_mpv) return false;

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
    case 0xDB: {  // VK_OEM_4: [
        g_mpv->setSpeed(g_mpv->speed() - 0.25f);
        char msg[32];
        std::snprintf(msg, sizeof(msg), "%s: %.2fx", T("倍速", "Speed"), g_mpv->speed());
        showToast(msg);
        break;
    }
    case ']':
    case 0xDD: {  // VK_OEM_6
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
    case 'S':
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            if (g_mpv && g_mpv->hasMedia()) {
                std::string f = openSubtitleDialog(hwnd);
                if (!f.empty()) {
                    g_mpv->loadSubtitle(f);
                    showToast(T("字幕已加载", "Subtitle loaded"));
                }
            }
        } else {
            bool vis = !g_mpv->subVisible();
            g_mpv->setSubVisibility(vis);
            showToast(vis ? i18n::subtitlesOn() : i18n::subtitlesOff());
        }
        break;
    case 'A': {  // AB loop
        if (g_mpv->loopA() < 0) {
            g_mpv->setLoopA();
            showToast(i18n::loopASet());
        } else if (g_mpv->loopB() < 0) {
            g_mpv->setLoopB();
            if (g_mpv->looping()) showToast(i18n::loopActive());
            else { g_mpv->clearLoop(); showToast(i18n::loopCleared()); }
        } else {
            g_mpv->clearLoop();
            showToast(i18n::loopCleared());
        }
        break;
    }
    case 'G': {  // Chapter jump
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
    case 'V': {  // Audio track cycle
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
    case 'B': {  // Subtitle position cycle
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
    case 'D': {  // Deband level cycle
        int cur = g_mpv->debandLevel();
        int next = (cur + 1) % 4;
        g_mpv->setDebandLevel(next);
        const char* names[] = {i18n::debandOff(), i18n::debandLight(), i18n::debandMedium(), i18n::debandStrong()};
        char msg[32];
        std::snprintf(msg, sizeof(msg), "%s: %s", T("去色带", "Deband"), names[next]);
        showToast(msg);
        break;
    }
    case 'R': {  // Aspect ratio cycle
        g_mpv->cycleAspectRatio();
        const char* names[] = {"auto", "16:9", "4:3", "1:1"};
        char msg[32];
        std::snprintf(msg, sizeof(msg), "%s: %s", T("比例", "Ratio"), names[g_mpv->aspectRatioIndex()]);
        showToast(msg);
        break;
    }
    case 'E': {  // EQ toggle
        g_ui.eqMenuOpen = !g_ui.eqMenuOpen;
        g_ui.eqDraggingBand = -1;
        break;
    }
    case VK_ESCAPE:
        if (g_ui.shortcutsOpen) g_ui.shortcutsOpen = false;
        else if (g_ui.speedMenuOpen) g_ui.speedMenuOpen = false;
        else if (g_ui.eqMenuOpen) g_ui.eqMenuOpen = false;
        else if (g_ui.volumeSliderOpen) g_ui.volumeSliderOpen = false;
        break;
    case VK_OEM_2:  // ? key (Shift + /)
        g_ui.shortcutsOpen = !g_ui.shortcutsOpen;
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
        if (g_mpv && g_mpv->hasMedia()) {
            std::string f = openFileDialog(hwnd);
            if (!f.empty()) {
                addToPlaylist(f);
                if (!g_ui.playlistOpen) g_ui.playlistOpen = true;
                showToast(T("已添加到播放列表", "Added to playlist"));
            }
        }
        break;
    }
    case VK_DELETE: {
        bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        bool shift = GetKeyState(VK_SHIFT) & 0x8000;
        if (ctrl && shift) {
            int n = g_cfg.historyCount();
            g_cfg.clearHistory();
            saveConfig(configPath(), g_cfg);
            char msg[64];
            std::snprintf(msg, sizeof(msg), "%s (%d %s)", T("已清除播放历史", "History cleared"), n, T("条", "items"));
            showToast(msg);
        } else if (g_mpv && g_mpv->hasMedia() && g_playlist.size() > 1) {
            int curIdx = playlistIndexOf(g_mpv->path());
            if (curIdx >= 0) {
                std::string fn = fileNameOf(g_playlist[curIdx]);
                removeFromPlaylist(curIdx);
                char msg[128];
                std::snprintf(msg, sizeof(msg), "%s: %s",
                              T("已移除", "Removed"), fn.c_str());
                showToast(msg);
            }
        }
        break;
    }
    case 'H': {  // Previous track
        int idx = playlistIndexOf(g_mpv->path());
        if (idx > 0) { playIndex(idx - 1); showToast(T("上一曲", "Previous")); }
        else showToast(i18n::noPrev());
        break;
    }
    case 'J': {  // Next track
        int idx = playlistIndexOf(g_mpv->path());
        int n = (int)g_playlist.size();
        if (idx >= 0 && idx + 1 < n) { playIndex(idx + 1); showToast(T("下一曲", "Next")); }
        else showToast(i18n::noNext());
        break;
    }
    case 'L':  // Playlist toggle
        g_ui.playlistOpen = !g_ui.playlistOpen;
        LOG_DBG("MAIN", "playlist toggle (key): %d", g_ui.playlistOpen ? 1 : 0);
        break;
    case VK_SNAPSHOT:  // PrintScreen: screenshot
        if (g_mpv && g_mpv->hasMedia()) {
            const char* cmd[] = { "screenshot", NULL };
            int r = mpv_command(g_mpv->mpv(), cmd);
            showToast(r < 0 ? i18n::screenshotFailed() : i18n::screenshotSaved());
        }
        break;
    default:
        return false;  // not consumed
    }

    g_ui.visible = true;
    g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
    return true;
}
