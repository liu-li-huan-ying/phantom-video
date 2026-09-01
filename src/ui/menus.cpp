#include "ui/menus.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>

#include "app/app_state.h"
#include "ui/helpers.h"
#include "ui/dialogs.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/mpv_backend.h"

// ---- helper: layout row1 for menu positioning ----
static Row1Layout getRow1(bool volOpen) {
    Row1Layout L;
    layoutRow1(g_ui.totalW > 0 ? g_ui.totalW : g_ui.winW, sbTopY(), volOpen, L);
    return L;
}

// ---- helper: rect hit test ----
static bool inRc(const SDL_Rect& r, int mx, int my) {
    return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
}

// ================================================================
// Settings panel click handling
// ================================================================
bool handleSettingsClick(HWND hwnd, int mx, int my) {
    SettingsGeom sg = settingsGeom(g_ui.winW, g_ui.winH);
    bool inside = (mx >= sg.panelX && mx <= sg.panelX + sg.panelW &&
                   my >= sg.panelY && my <= sg.panelY + sg.panelH);
    LOG_DBG("MAIN", "settings click: mx=%d my=%d panel(%d,%d,%d,%d) closeC(%d,%d,R=%d) inside=%d",
            mx, my, sg.panelX, sg.panelY, sg.panelW, sg.panelH,
            sg.closeCx, sg.closeCy, sg.closeR, inside ? 1 : 0);

    if (!inside) {
        LOG_DBG("MAIN", "settings close: click outside panel");
        g_ui.settingsOpen = false;
        return true;
    }

    // Close button
    if (std::abs(mx - sg.closeCx) <= sg.closeR &&
        std::abs(my - sg.closeCy) <= sg.closeR) {
        LOG_DBG("MAIN", "settings close hit");
        g_ui.settingsOpen = false;
        saveConfig(configPath(), g_cfg);
        return true;
    }

    // Settings rows
    int* vals[SET_ROW_COUNT] = { &g_cfg.hwDecode, &g_cfg.volNorm,
        &g_cfg.subAutoLoad, &g_cfg.thumbCache, &g_cfg.resume,
        &g_cfg.nightMode, &g_cfg.audioExclusive, &g_cfg.motionInterp,
        &g_cfg.hiQScale, &g_cfg.interpolation, &g_cfg.superRes,
        &g_cfg.audioOutput };
    const char* keys[SET_ROW_COUNT] = { "hw", "vol", "sub", "thumb",
        "resume", "night", "excl", "interp", "hiq", "vsinterp", "vssr",
        "audioOut" };

    bool handled = false;
    for (int i = 0; i < SET_ROW_COUNT && !handled; ++i) {
        if (my >= sg.rowY[i] - U(5) && my <= sg.rowY[i] + sg.swH + U(5) &&
            mx >= sg.panelX + U(12)) {
            if (i == SET_ROW_COUNT - 1) {
                g_cfg.audioOutput = (g_cfg.audioOutput + 1) % 4;
                applySetting("audioOut", g_cfg.audioOutput);
                const char* modeNames[] = {"立体声", "5.1环绕", "7.1环绕", "直通"};
                char msg[32];
                std::snprintf(msg, sizeof(msg), "%s: %s", T("音频输出", "Audio"), modeNames[g_cfg.audioOutput]);
                showToast(msg);
            } else {
                *vals[i] = *vals[i] ? 0 : 1;
                applySetting(keys[i], *vals[i]);
                const char* tNames[] = { i18n::hwDecode(), i18n::volNorm(), i18n::subAutoLoad(),
                    i18n::thumbCache(), i18n::resume(), i18n::nightMode(),
                    i18n::exclusiveAudio(), i18n::motionInterp(), i18n::hiQScaling(),
                    i18n::vsInterp(), i18n::vsSuperRes() };
                showToast(tNames[i]);
            }
            LOG_INFO("MAIN", "setting %s -> %d", keys[i], *vals[i]);
            handled = true;
        }
    }

    // Play mode chips
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

    // Language selector
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
    g_ui.visible = true;
    g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
    return true;
}

// ================================================================
// Popup menu click handling (speed/quality/EQ/image/sub/audio/chapter)
// ================================================================
bool handleMenuClicks(HWND hwnd, int mx, int my) {
    if (!g_ui.speedMenuOpen && !g_ui.qualityMenuOpen && !g_ui.eqMenuOpen &&
        !g_ui.subMenuOpen && !g_ui.audioMenuOpen && !g_ui.imageMenuOpen)
        return false;

    // ---- Speed menu ----
    if (g_ui.speedMenuOpen) {
        Row1Layout L = getRow1(false);
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

    // ---- Quality menu ----
    if (g_ui.qualityMenuOpen) {
        Row1Layout QL = getRow1(false);
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
                const char* qNames[] = { T("省电", "Power Saving"), T("标准", "Standard"), T("卓越", "Ultimate") };
                showToast(qNames[idx]);
            }
        }
        g_ui.qualityMenuOpen = false;
    }

    // ---- EQ menu ----
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
                // EQ preset buttons
                struct EqPreset { const char* name; float bands[6]; };
                static const EqPreset presets[] = {
                    { nullptr,    { 0,  0,  0,  0,  0,  0 } },
                    { nullptr,    { 6,  4,  1, -1, -2, -3 } },
                    { nullptr,    {-3, -2, -1,  1,  4,  6 } },
                    { nullptr,    {-2, -1,  3,  4,  2, -1 } },
                    { nullptr,    { 5,  3, -1, -1,  3,  5 } },
                };
                static const char* presetNames[] = {
                    i18n::presetFlat(), i18n::presetBass(), i18n::presetTreble(),
                    i18n::presetVocal(), i18n::presetRock()
                };
                int presetY = resetY + U(30) + U(14);
                int presetBtnW = (menuW - U(20)) / 5;
                for (int i = 0; i < 5; ++i) {
                    int bx = menuX + U(10) + i * presetBtnW;
                    int bw = presetBtnW - U(4);
                    if (mx >= bx && mx <= bx + bw && my >= presetY && my <= presetY + U(22)) {
                        for (int b = 0; b < 6; ++b)
                            g_mpv->setEQBand(b, presets[i].bands[b]);
                        showToast(presetNames[i]);
                        break;
                    }
                }
                g_ui.visible = true;
                g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
                g_dirty.store(true);
                return true;
            }
        } else {
            g_ui.eqMenuOpen = false;
        }
    }

    // ---- Image adjustment panel ----
    if (g_ui.imageMenuOpen) {
        int panelW = U(380), panelH = U(370);
        int panelX = g_ui.winW / 2 - panelW / 2;
        int panelY = g_ui.winH / 2 - panelH / 2;
        int closeR = U(10);
        int closeCx = panelX + panelW - U(22);
        int closeCy = panelY + U(20);
        if (mx >= closeCx - closeR && mx <= closeCx + closeR &&
            my >= closeCy - closeR && my <= closeCy + closeR) {
            g_ui.imageMenuOpen = false;
            g_dirty.store(true);
            return true;
        }
        if (mx >= panelX && mx <= panelX + panelW && my >= panelY && my <= panelY + panelH) {
            int sliderX = panelX + U(90);
            int sliderW = panelW - U(180);
            int rowH = U(44);
            int baseY = panelY + U(44);
            const char* keys[] = {"brt", "con", "sat", "gam"};
            bool hitSlider = false;
            for (int i = 0; i < 4; ++i) {
                int ry = baseY + i * rowH;
                if (mx >= sliderX - U(8) && mx <= sliderX + sliderW + U(8) &&
                    my >= ry && my <= ry + rowH) {
                    float norm = (float)(mx - sliderX) / sliderW;
                    if (norm < 0.0f) norm = 0.0f; if (norm > 1.0f) norm = 1.0f;
                    int val = (int)(norm * 200.0f - 100.0f);
                    applySetting(keys[i], val);
                    g_ui.imageDraggingSlider = i;
                    SetCapture(hwnd);
                    hitSlider = true;
                    break;
                }
            }
            if (!hitSlider) {
                int swY = baseY + 4 * rowH + U(4);
                if (mx >= panelX + U(16) && mx <= panelX + panelW - U(16) && my >= swY && my <= swY + U(24)) {
                    applySetting("deint", g_cfg.deinterlace ? 0 : 1);
                    g_dirty.store(true);
                    return true;
                }
                int tmY = swY + U(34);
                if (mx >= panelX + U(16) && mx <= panelX + panelW - U(16) && my >= tmY && my <= tmY + U(26)) {
                    g_cfg.toneMapping = (g_cfg.toneMapping + 1) % 5;
                    applySetting("tm", g_cfg.toneMapping);
                    g_dirty.store(true);
                    return true;
                }
                int gmY = tmY + U(32);
                if (mx >= panelX + U(16) && mx <= panelX + panelW - U(16) && my >= gmY && my <= gmY + U(26)) {
                    g_cfg.gamutMapping = (g_cfg.gamutMapping + 1) % 4;
                    applySetting("gm", g_cfg.gamutMapping);
                    g_dirty.store(true);
                    return true;
                }
                int hpY = gmY + U(32);
                if (mx >= panelX + U(16) && mx <= panelX + panelW - U(16) && my >= hpY && my <= hpY + U(24)) {
                    applySetting("hdrpk", g_cfg.hdrPeakDetect ? 0 : 1);
                    g_dirty.store(true);
                    return true;
                }
                g_dirty.store(true);
                return true;
            }
        } else {
            g_ui.imageMenuOpen = false;
        }
    }

    // ---- Subtitle menu ----
    if (g_ui.subMenuOpen) {
        auto subs = g_mpv->subTracks();
        Row1Layout L = getRow1(false);
        int itemH = U(32), menuW = U(180);
        int menuH = (int)(subs.size() + 2) * itemH + U(12);
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
                std::snprintf(msg, sizeof(msg), "%s: %s", T("字幕", "Subtitle"), subs[idx - 1].desc.c_str());
                showToast(msg);
            } else if (idx == (int)subs.size() + 1) {
                g_ui.subMenuOpen = false;
                if (g_mpv->hasMedia()) {
                    std::string f = openSubtitleDialog(hwnd);
                    if (!f.empty()) {
                        g_mpv->loadSubtitle(f);
                        showToast(T("字幕已加载", "Subtitle loaded"));
                    }
                }
            }
        }
        g_ui.subMenuOpen = false;
    }

    // ---- Audio track menu ----
    if (g_ui.audioMenuOpen) {
        auto tracks = g_mpv->audioTracks();
        Row1Layout L = getRow1(false);
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
                std::snprintf(msg, sizeof(msg), "%s: %s", T("音轨", "Audio"), tracks[idx].desc.c_str());
                showToast(msg);
            }
        }
        g_ui.audioMenuOpen = false;
    }

    // ---- Chapter menu ----
    if (g_ui.chapterMenuOpen) {
        auto chs = g_mpv->chapters();
        Row1Layout L = getRow1(false);
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
                    ? T("无标题", "Untitled") : chs[idx].title.c_str();
                char msg[128];
                std::snprintf(msg, sizeof(msg), "%s %d: %s",
                              T("章节", "Chapter"), idx + 1, name);
                showToast(msg);
            }
        }
        g_ui.chapterMenuOpen = false;
    }

    g_ui.visible = true;
    g_ui.hideAt = SDL_GetTicks() + ui::CTRLBAR_HIDE_MS;
    g_dirty.store(true);
    return true;
}

// ================================================================
// Welcome page click handling
// ================================================================
bool handleWelcomeClick(HWND hwnd, int mx, int my) {
    if (g_mpv && g_mpv->hasMedia()) return false;

    auto inRc = [&](const SDL_Rect& rc) {
        return mx >= rc.x && mx <= rc.x + rc.w && my >= rc.y && my <= rc.y + rc.h;
    };

    if (g_ui.heroFileBtn.w > 0 && inRc(g_ui.heroFileBtn)) {
        std::string f = openFileDialog(hwnd);
        LOG_INFO("MAIN", "welcome open-file -> %s", f.c_str());
        if (!f.empty()) { buildPlaylistAround(f); playPath(f); }
        return true;
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
        return true;
    }
    for (auto& kv : g_ui.continueHits) {
        if (inRc(kv.second)) {
            LOG_INFO("MAIN", "welcome continue-watch click");
            buildPlaylistAround(kv.first);
            playPath(kv.first, true);
            return true;
        }
    }
    for (auto& kv : g_ui.gridHits) {
        if (kv.first >= 0 && kv.first < (int)g_playlist.size() && inRc(kv.second)) {
            std::string p = g_playlist[kv.first];
            playPath(p);
            return true;
        }
    }
    return false;
}

// ================================================================
// Control bar click handling
// ================================================================
bool handleControlBarClick(HWND hwnd, int mx, int my) {
    bool volOpen = (g_ui.volumeSliderOpen || g_ui.volumeDragging);
    Row1Layout L = getRow1(volOpen);
    auto inRc = [&](const SDL_Rect& r) {
        return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
    };

    // Volume slider drag start
    if (volOpen && mx >= L.volSliderX - U(6) &&
        mx <= L.volSliderX + U(86) &&
        my >= L.cy - U(20) && my <= L.cy + U(20)) {
        g_ui.volumeDragging = true;
        float ratio = (float)(mx - L.volSliderX) / U(80);
        if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
        g_mpv->setVolume(ratio);
        SetCapture(hwnd);
        return true;
    }

    // Previous
    if (inRc(L.prev)) {
        int idx = playlistIndexOf(g_mpv->path());
        if (idx > 0) { playIndex(idx - 1); showToast(T("上一曲", "Previous")); }
        else showToast(i18n::noPrev());
        return true;
    }
    // Play/Pause
    if (inRc(L.play)) {
        g_mpv->togglePause();
        return true;
    }
    // Next
    if (inRc(L.next)) {
        int idx = playlistIndexOf(g_mpv->path());
        int n = (int)g_playlist.size();
        if (idx >= 0 && idx + 1 < n) { playIndex(idx + 1); showToast(T("下一曲", "Next")); }
        else showToast(i18n::noNext());
        return true;
    }
    // Subtitles
    if (inRc(L.subBtn)) {
        auto subs = g_mpv->subTracks();
        if (subs.size() > 1) {
            g_ui.subMenuOpen = !g_ui.subMenuOpen;
            g_ui.audioMenuOpen = false;
            g_ui.chapterMenuOpen = false;
        } else {
            bool vis = !g_mpv->subVisible();
            g_mpv->setSubVisibility(vis);
            std::string trk = g_mpv->currentSubTrack();
            char msg[96];
            std::snprintf(msg, sizeof(msg), vis ? "%s [%s]" : "%s",
                          vis ? i18n::subtitlesOn() : i18n::subtitlesOff(),
                          trk.c_str());
            showToast(msg);
        }
        return true;
    }
    // Speed
    if (inRc(L.speedBtn)) {
        g_ui.speedMenuOpen = !g_ui.speedMenuOpen;
        g_ui.subMenuOpen = false;
        g_ui.audioMenuOpen = false;
        g_ui.chapterMenuOpen = false;
        return true;
    }
    // Audio track
    if (inRc(L.audioBtn)) {
        auto tracks = g_mpv->audioTracks();
        if (tracks.size() > 1) {
            g_ui.audioMenuOpen = !g_ui.audioMenuOpen;
            g_ui.subMenuOpen = false;
            g_ui.chapterMenuOpen = false;
        } else {
            showToast(i18n::audioTrack());
        }
        return true;
    }
    // Chapter
    if (inRc(L.chapterBtn)) {
        auto chs = g_mpv->chapters();
        if (!chs.empty()) {
            g_ui.chapterMenuOpen = !g_ui.chapterMenuOpen;
            g_ui.subMenuOpen = false;
            g_ui.audioMenuOpen = false;
        } else {
            showToast(T("无章节信息", "No chapters"));
        }
        return true;
    }
    // AB loop
    if (inRc(L.abBtn)) {
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
        return true;
    }
    // EQ
    if (inRc(L.eqBtn)) {
        g_ui.eqMenuOpen = !g_ui.eqMenuOpen;
        g_ui.eqDraggingBand = -1;
        return true;
    }
    // Image adjustments
    if (inRc(L.imgBtn)) {
        g_ui.imageMenuOpen = !g_ui.imageMenuOpen;
        g_ui.eqMenuOpen = false;
        g_ui.subMenuOpen = false;
        g_ui.audioMenuOpen = false;
        g_ui.chapterMenuOpen = false;
        g_ui.speedMenuOpen = false;
        g_ui.qualityMenuOpen = false;
        return true;
    }
    // Quality
    if (inRc(L.qualityBtn)) {
        g_ui.qualityMenuOpen = !g_ui.qualityMenuOpen;
        g_ui.subMenuOpen = false;
        g_ui.audioMenuOpen = false;
        g_ui.chapterMenuOpen = false;
        g_ui.imageMenuOpen = false;
        return true;
    }
    // Volume icon (mute toggle)
    if (mx >= L.volIconCx - U(17) && mx <= L.volIconCx + U(17) &&
        my >= L.cy - U(17) && my <= L.cy + U(17)) {
        g_mpv->toggleMute();
        showToast(g_mpv->muted() ? i18n::muted() : i18n::unmuted());
        LOG_INFO("MAIN", "mute toggled -> %d", g_mpv->muted() ? 1 : 0);
        return true;
    }
    // Settings
    if (inRc(L.setBtn)) {
        g_ui.settingsOpen = !g_ui.settingsOpen;
        g_ui.imageMenuOpen = false;
        LOG_INFO("MAIN", "setBtn click -> open=%d", g_ui.settingsOpen ? 1 : 0);
        return true;
    }
    // Fullscreen
    if (inRc(L.fullBtn)) {
        toggleFullscreen(hwnd);
        return true;
    }

    return false;  // not consumed
}
