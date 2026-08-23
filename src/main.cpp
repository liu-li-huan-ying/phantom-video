#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_syswm.h>

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/player.h"
#include "core/playlist.h"
#include "core/thumbnail_extractor.h"
#include "ui/osd.h"
#include "ui/custom_titlebar.h"
#include "ui/playlist_panel.h"
#include "video/video_renderer.h"
#include "core/logger.h"

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
    if (h > 0)
        std::snprintf(buf, n, "%d:%02d:%02d", h, m, ss);
    else
        std::snprintf(buf, n, "%02d:%02d", m, ss);
}

static std::string replaceExt(const std::string& path, const char* newExt) {
    std::size_t dot = path.find_last_of('.');
    std::size_t slash = path.find_last_of("\\/");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return path.substr(0, dot) + newExt;
    return path + newExt;
}

static void loadExternalSubtitle(Player& player, const std::string& video) {
    for (const char* ext : { ".srt", ".ass", ".ssa", ".sub" }) {
        std::string cand = replaceExt(video, ext);
        FILE* f = std::fopen(cand.c_str(), "rb");
        if (f) {
            std::fclose(f);
            if (player.loadExternalSubtitle(cand)) {
                std::printf("已加载字幕: %s\n", cand.c_str());
                return;
            }
        }
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    SDL_SetMainReady();
    Logger::instance().init("vplayer", 7);
    // 日常模式默认 WARN（仅警告/错误），--debug 开启诊断模式（TRACE 以上全记录）
    bool diagMode = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--debug") { diagMode = true; break; }
    Logger::instance().setLevel(diagMode ? LogLevel::Trace : LogLevel::Warn);
    LOG_INFO("MAIN", "vplayer starting (log=%s)", diagMode ? "debug" : "normal");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::printf("SDL 鍒濆鍖栧け璐? %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("VPlayer", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 960, 540,
                                       SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::printf("鍒涘缓绐楀彛澶辫触: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // --- 自定义窗口图标 ---
    // 图标位于项目根目录 ico/vplay.ico
    // SDL2 SDL_LoadBMP 原生仅支持 BMP 格式，ICO 格式兼容有限。
    // 步骤：将 vplay.ico 另存为 vplay.bmp (32x32 或 256x256 均可)，
    //       将 vplay.bmp 放在同一目录，代码将自动加载 BMP。
    // 若仅有 ICO 且不另存 BMP，将打印提示并使用默认系统图标。
    {
        const char* iconBmpPath = "ico/vplay.bmp";
        const char* iconIcoPath = "ico/vplay.ico";
        SDL_Surface* icon = SDL_LoadBMP(iconBmpPath);
        if (!icon) {
            // 若无 BMP，尝试 ICO (SDL2 支持有限，建议另存为 BMP)
            icon = SDL_LoadBMP(iconIcoPath);
            if (!icon) {
                fprintf(stderr, "[warn] 图标加载失败：%s / %s (ICO/SDL2 有限支持)\n",
                        iconIcoPath, iconBmpPath);
                fprintf(stderr, "      将使用默认系统图标。请将 vplay.ico 另存为 vplay.bmp 以启用自定义图标。\n");
            }
        }
        if (icon) {
            SDL_SetWindowIcon(win, icon);
            SDL_FreeSurface(icon);
        }
    }

    // --- DWM: 启用窗口阴影 + 圆角（Windows 11+）---
    HWND hwnd = nullptr;
    {
        SDL_SysWMinfo wmi;
        SDL_VERSION(&wmi.version);
        if (SDL_GetWindowWMInfo(win, &wmi)) {
            hwnd = wmi.info.win.window;
            // DwmExtendFrameIntoClientArea: 使窗口有系统阴影
            MARGINS m = {0, 0, 0, 0};
            DwmExtendFrameIntoClientArea(hwnd, &m);
            // DWMWA_WINDOW_CORNER_PREFERENCE (33): 2=圆角 (Windows 11)
            int pref = 2;
            DwmSetWindowAttribute(hwnd, 33, &pref, sizeof(pref));
        }
    }

    // --- 自定义标题栏 ---
    CustomTitlebar titlebar;
    if (hwnd) {
        // 去掉系统默认标题栏（WS_CAPTION），保留窗口边框用于 DWM 阴影
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        style &= ~WS_CAPTION;   // 去掉标题栏
        style &= ~WS_THICKFRAME; // 去掉可调边框（自绘控件不需要）
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        // 通知系统窗口样式已改变，重新计算非客户区
        RECT rc;
        GetWindowRect(hwnd, &rc);
        SetWindowPos(hwnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER);

        titlebar.init(hwnd);
        titlebar.setTitle("VPlayer");
    }

    Player player;
    VideoRenderer vrender;
    OSD osd;
    ThumbnailExtractor thumbnail;
    if (!vrender.init(win)) {
        std::printf("鍒涘缓娓叉煋鍣ㄥけ璐? %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    osd.init(vrender.renderer());
    // M18: seeking 状态回调 → 显示/隐藏 Seeking 指示器
    player.onSeekingChanged = [&](bool seeking) {
        if (seeking) vrender.showSeekingOverlay();
        else vrender.hideSeekingOverlay();
    };

auto args = utf8Args();
    AppConfig cfg;
    loadConfig(configPath(), cfg);

    Playlist playlist;
    bool multiArgs = args.size() > 1 && !args[1].empty();
    if (multiArgs) {
        std::vector<std::string> files;
        for (std::size_t i = 1; i < args.size(); ++i)
            if (!args[i].empty()) files.push_back(args[i]);
        playlist.set(files);
    } else if (cfg.resume && !cfg.lastFile.empty()) {
        // resume=1: reopen last file with its directory playlist
        if (!playlist.scanDirectory(cfg.lastFile))
            playlist.set(cfg.lastFile);
    }
    playlist.setMode(static_cast<PlayMode>(cfg.playMode));

    PlaylistPanel panel;
    panel.init(vrender.renderer());
    panel.setPlaylist(&playlist);

    player.setVolume(cfg.volume);

    auto openCurrent = [&]() {
        if (playlist.empty()) {
            SDL_SetWindowTitle(win, "VPlayer");
            vrender.clear();
            return;
        }
        const std::string& p = playlist.current();
        std::string base = p;
        std::size_t slash = base.find_last_of("\\/");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        char title[512];
        if (playlist.size() > 1)
            std::snprintf(title, sizeof(title), "VPlayer - %s (%d/%d)", base.c_str(),
                          playlist.index() + 1, playlist.size());
        else
            std::snprintf(title, sizeof(title), "VPlayer - %s", base.c_str());
        SDL_SetWindowTitle(win, title);
        titlebar.setTitle(title);
        if (player.openFile(p)) {
            loadExternalSubtitle(player, p);
            thumbnail.open(p);  // M15: 打开缩略图提取器
            if (cfg.resume) {
                auto it = cfg.history.find(p);
                if (it != cfg.history.end() && it->second > 2.0) {
                    player.seek(it->second);
                    vrender.showToast("已从上次位置续播 (按 R 关闭)");
                }
            }
        } else {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "打开失败",
                                     player.error().c_str(), win);
        }
    };
    openCurrent();

    bool running = true;
    bool fullscreen = false;
    Uint32 volHideAt = 0;
    bool draggingProgress = false;
    bool draggingVolume = false;

    static const float kSpeeds[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f };
    static const int kSpeedCount = (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0]));
    auto cycleSpeed = [&](int dir) {
        float cur = player.speed();
        int idx = 1;
        for (int i = 0; i < kSpeedCount; ++i)
            if (kSpeeds[i] == cur) { idx = i; break; }
        idx = (idx + dir + kSpeedCount) % kSpeedCount;
        player.setSpeed(kSpeeds[idx]);
        char msg[32];
        std::snprintf(msg, sizeof(msg), "倍速: x%.2g", kSpeeds[idx]);
        vrender.showToast(msg);
        volHideAt = SDL_GetTicks() + 2000;
    };

    auto nextTrack = [&]() {
        if (playlist.next()) openCurrent();
    };
    auto prevTrack = [&]() {
        if (playlist.prev()) openCurrent();
    };
    auto cyclePlayMode = [&]() {
        int m = ((int)playlist.mode() + 1) % 3;
        playlist.setMode(static_cast<PlayMode>(m));
        const char* names[] = { "单独播放", "循环播放", "随机播放" };
        std::printf("播放模式: %s\n", names[m]);
        vrender.showToast(std::string("播放模式: ").append(names[m]).c_str());
        volHideAt = SDL_GetTicks() + 2000;
    };

    auto cycleResume = [&]() {
        cfg.resume = cfg.resume ? 0 : 1;
        std::printf("恢复播放位置: %s\n", cfg.resume ? "开启" : "关闭");
        vrender.showToast(cfg.resume ? "恢复播放位置: 开启" : "恢复播放位置: 关闭");
        volHideAt = SDL_GetTicks() + 2000;
    };

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_DROPFILE:
                if (player.openFile(e.drop.file)) {
                    loadExternalSubtitle(player, e.drop.file);
                    thumbnail.open(e.drop.file);
                    playlist.scanDirectory(e.drop.file);
                    std::string base = e.drop.file;
                    std::size_t slash = base.find_last_of("\\/");
                    if (slash != std::string::npos) base = base.substr(slash + 1);
                    char title[512];
                    std::snprintf(title, sizeof(title), "VPlayer - %s", base.c_str());
                    SDL_SetWindowTitle(win, title);
                    titlebar.setTitle(title);
                    std::printf("已打开: %s\n", e.drop.file);
                }
                else
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "打开失败",
                                             player.error().c_str(), win);
                SDL_free(e.drop.file);
                break;
            case SDL_MOUSEMOTION:
                {
                    int mx = e.motion.x, my = e.motion.y;
                    int winW = 0, winH = 0;
                    SDL_GetWindowSize(win, &winW, &winH);
                    // M16: 播放列表面板事件优先处理
                    panel.setPlaylist(&playlist);
                    if (panel.handleMouseMove(mx, my, winW, winH)) {
                        vrender.setPanelWidth(panel.width());
                        break;
                    }
                    vrender.onMouseMove(mx, my);
                    if (draggingProgress) {
                        ControlLayout lay = ControlLayout::compute(winW, winH, panel.width());
                        float pct = (float)(mx - lay.progX) / lay.progW;
                        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
                        player.seek(pct * player.duration());
                        vrender.setThumbnail(nullptr, 0, 0, -1);
                    } else if (draggingVolume) {
                        ControlLayout lay = ControlLayout::compute(winW, winH, panel.width());
                        const int ph = 90, py = lay.barY - ph - 12;
                        float v = 1.0f - (float)(my - py - 6) / (ph - 12);
                        if (v < 0) v = 0; if (v > 1) v = 1;
                        player.setVolume(v);
                        if (v == 0) { player.setVolume(0.0001f); }
                    } else if (player.hasMedia()) {
                        // 进度条 hover 缩略图（同步提取，可靠显示）
                        int winW = 0, winH = 0;
                        SDL_GetWindowSize(win, &winW, &winH);
                        ControlLayout lay = ControlLayout::compute(winW, winH, panel.width());
                        bool overProg = mx >= lay.progX && mx < lay.progX + lay.progW &&
                                        my >= lay.progY - 10 && my < lay.progY + 12;
                        if (overProg && player.duration() > 0) {
                            float pct = (float)(mx - lay.progX) / lay.progW;
                            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
                            double targetTime = pct * player.duration();
                            // 只在时间差距较大时重新提取（避免频繁 seek 卡顿）
                            static double lastThumbTime = -1;
                            if (std::abs(targetTime - lastThumbTime) > 1.0) {
                                lastThumbTime = targetTime;
                                if (thumbnail.isOpen()) {
                                    uint8_t* pixels = nullptr;
                                    int tw = 0, th = 0;
                                    if (thumbnail.getFrame(targetTime, &pixels, tw, th) && pixels) {
                                        SDL_Texture* tex = SDL_CreateTexture(
                                            vrender.renderer(), SDL_PIXELFORMAT_RGB24,
                                            SDL_TEXTUREACCESS_STREAMING, tw, th);
                                        if (tex) {
                                            void* tbits = nullptr;
                                            int pitch = 0;
                                            if (SDL_LockTexture(tex, nullptr, &tbits, &pitch) == 0) {
                                                for (int row = 0; row < th; ++row)
                                                    memcpy((Uint8*)tbits + row * pitch,
                                                           pixels + row * tw * 3, tw * 3);
                                                SDL_UnlockTexture(tex);
                                                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                                                vrender.setThumbnail(tex, tw, th, targetTime);
                                            } else {
                                                SDL_DestroyTexture(tex);
                                            }
                                        }
                                        av_free(pixels);
                                    }
                                }
                            }
                        } else {
                            vrender.setThumbnail(nullptr, 0, 0, -1);
                        }
                    }
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                {
                    int mx = e.button.x, my = e.button.y;
                    int winW = 0, winH = 0;
                    SDL_GetWindowSize(win, &winW, &winH);
                    // M16: 播放列表面板事件优先处理
                    if (panel.handleMouseDown(mx, my, winW, winH)) {
                        vrender.setPanelWidth(panel.width());
                        // 检查面板点击选曲
                        int clicked = panel.clickedIndex();
                        if (clicked >= 0 && clicked < (int)playlist.size()) {
                            // 跳转到点击的曲目
                            while (playlist.index() < clicked) playlist.next();
                            while (playlist.index() > clicked) playlist.prev();
                            openCurrent();
                            panel.clearClick();
                        }
                        break;
                    }
                    ControlLayout lay = ControlLayout::compute(winW, winH, panel.width());
                    const int btnY = lay.btnY, bs = lay.btnSize;
                    bool hitControl = false;
                    // Prev
                    if (mx >= lay.prevX && mx < lay.prevX + bs && my >= btnY && my < btnY + bs) {
                        prevTrack(); hitControl = true;
                    }
                    // Play/Pause
                    else if (mx >= lay.playX && mx < lay.playX + bs && my >= btnY && my < btnY + bs) {
                        player.togglePause(); hitControl = true;
                    }
                    // Next
                    else if (mx >= lay.nextX && mx < lay.nextX + bs && my >= btnY && my < btnY + bs) {
                        nextTrack(); hitControl = true;
                    }
                    // Play mode
                    else if (mx >= lay.modeX && mx < lay.modeX + bs && my >= btnY && my < btnY + bs) {
                        cyclePlayMode(); hitControl = true;
                    }
                    // Speed button toggles popup menu
                    else if (mx >= lay.speedX && mx < lay.speedX + bs && my >= btnY && my < btnY + bs) {
                        vrender.toggleSpeedMenu();
                        volHideAt = SDL_GetTicks() + 2000;
                        hitControl = true;
                    }
                    // Speed menu item selection
                    else if (vrender.speedMenuOpen()) {
                        bool inMenu = false;
                        for (int i = 0; i < 8; ++i) {
                            SDL_Rect r = VideoRenderer::speedMenuItemRect(lay, i);
                            if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) {
                                player.setSpeed(kSpeeds[i]);
                                vrender.setSpeedMenuOpen(false);
                                char msg[32];
                                std::snprintf(msg, sizeof(msg), "倍速: x%.2g", kSpeeds[i]);
                                vrender.showToast(msg);
                                hitControl = true;
                                inMenu = true;
                                break;
                            }
                        }
                        if (!inMenu) vrender.setSpeedMenuOpen(false);
                    }
                    // Volume button (click toggles mute; drag handled via popup)
                    else if (mx >= lay.volX && mx < lay.volX + bs && my >= btnY && my < btnY + bs) {
                        if (!draggingVolume) player.toggleMute();
                        volHideAt = SDL_GetTicks() + 2000;
                        hitControl = true;
                    }
                    // Fullscreen button
                    else if (mx >= lay.fsX && mx < lay.fsX + bs && my >= btnY && my < btnY + bs) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win,
                            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        hitControl = true;
                    }
                    // Volume popup drag (above vol button)
                    const int ph = 90, py = lay.barY - ph - 12, vx = lay.volX + bs / 2 - 3;
                    if (mx >= vx && mx < vx + 6 && my >= py && my < py + ph) {
                        draggingVolume = true;
                        float v = 1.0f - (float)(my - py - 6) / (ph - 12);
                        if (v < 0) v = 0; if (v > 1) v = 1;
                        player.setVolume(v == 0 ? 0.0001f : v);
                        hitControl = true;
                    }
                    // Progress bar click-to-seek (full-width bottom bar)
                    if (mx >= lay.progX && mx < lay.progX + lay.progW &&
                        my >= lay.progY - 10 && my < lay.progY + 12) {
                        float pct = (float)(mx - lay.progX) / lay.progW;
                        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
                        player.seek(pct * player.duration());
                        draggingProgress = true;
                        hitControl = true;
                    }
                    // 单击视频区域暂停/继续
                    if (e.button.clicks == 1 && !hitControl && player.hasMedia()) {
                        player.togglePause();
                    }
                    // Double-click for fullscreen only outside controls
                    if (e.button.clicks == 2 && !hitControl) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win,
                            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                panel.handleMouseUp(e.button.x, e.button.y);
                draggingProgress = false;
                draggingVolume = false;
                break;
            case SDL_MOUSEWHEEL:
                {
                    int winH = 0;
                    SDL_GetWindowSize(win, nullptr, &winH);
                    int winW2 = 0, winH2 = 0;
                    SDL_GetWindowSize(win, &winW2, &winH2);
                    panel.handleMouseWheel(e.wheel.y, winW2, winH2);
                }
                break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                case SDLK_SPACE:
                    player.togglePause();
                    break;
                case SDLK_LEFT:
                    player.seekRelative(e.key.keysym.mod & KMOD_CTRL ? -30.0 : -5.0);
                    break;
                case SDLK_RIGHT:
                    player.seekRelative(e.key.keysym.mod & KMOD_CTRL ? 30.0 : 5.0);
                    break;
                case SDLK_UP:
                    player.setVolume(player.volume() + 0.1f);
                    volHideAt = SDL_GetTicks() + 2000;
                    break;
                case SDLK_DOWN:
                    player.setVolume(player.volume() - 0.1f);
                    volHideAt = SDL_GetTicks() + 2000;
                    break;
                case SDLK_m:
                    player.toggleMute();
                    volHideAt = SDL_GetTicks() + 2000;
                    break;
                case SDLK_n:
                    nextTrack();
                    break;
                case SDLK_p:
                    prevTrack();
                    break;
                case SDLK_x:
                    cyclePlayMode();
                    break;
                case SDLK_r:
                    cycleResume();
                    break;
                case SDLK_f:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win,
                        fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                case SDLK_s:
                    cycleSpeed(-1);
                    break;
                case SDLK_l:
                    cycleSpeed(1);
                    break;
                case SDLK_a: {
                    bool on = !player.audio().normalization();
                    player.audio().setNormalization(on);
                    vrender.showToast(on ? "音量标准化: 开启" : "音量标准化: 关闭");
                    volHideAt = SDL_GetTicks() + 2000;
                    break;
                }
                case SDLK_i:
                    osd.toggleInfo();
                    break;
                case SDLK_ESCAPE:
                case SDLK_q:
                    running = false;
                    break;
                default:
                    break;
                }
                break;
            default:
                break;
            }
        }

        if (player.hasMedia()) {
            FramePtr f = player.pullFrame();
            // seek / 切倍速后，时钟追上 target 时解除 UI 冻结
            if (player.uiSeeking()) {
                if (player.clock() >= player.uiTargetPts() - 0.1) {
                    player.clearUiSeeking();
                }
            }
            if (f) {
                RenderStats stats;
                stats.playing = (player.state() == Player::State::Playing);
                stats.paused = (player.state() == Player::State::Paused);
                stats.clock = player.clock();
                stats.uiClock = player.uiClock();
                stats.duration = player.duration();
                stats.volume = player.muted() ? 0.0f : player.volume();
                stats.muted = player.muted();
                stats.fullscreen = fullscreen;
                stats.speed = player.speed();
                stats.playMode = (int)playlist.mode();
                stats.draggingVolume = draggingVolume;
                static std::string subtitleBuf;
                if (player.hasSubtitle()) {
                    subtitleBuf = player.subtitleText(stats.clock);
                    stats.subtitle = subtitleBuf.c_str();
                } else {
                    subtitleBuf.clear();
                    stats.subtitle = nullptr;
                }
                stats.onPlayPause = [&]() { player.togglePause(); return true; };
                stats.onToggleFullscreen = [&]() { fullscreen = !fullscreen; SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); return true; };
                stats.onSeekTo = [&](double p) { player.seek(p); return true; };
                stats.onVolumeUp = [&]() { player.setVolume(player.volume() + 0.1f); volHideAt = SDL_GetTicks() + 2000; };
                stats.onNextTrack = [&]() { nextTrack(); };
                stats.onPrevTrack = [&]() { prevTrack(); };
                stats.onCycleMode = [&]() { cyclePlayMode(); };
                stats.onCycleSpeed = [&]() { cycleSpeed(1); };
                vrender.render(f.get(), stats);
            }
            else if (player.state() == Player::State::Ended) {
                if (playlist.hasNext()) {
                    nextTrack();
                } else {
                    vrender.clear();
                }
            }
        } else {
            vrender.clear();
        }
        // 绘制自定义标题栏（每帧，覆盖 SDL 渲染）
        titlebar.draw(vrender.renderer());
        // M16: 绘制播放列表面板
        {
            int pw = 0, ph = 0;
            SDL_GetWindowSize(win, &pw, &ph);
            panel.setPlaylist(&playlist);
            panel.draw(playlist.index(), pw, ph);
        }
        // M30c: OSD 信息叠加
        if (osd.isInfoVisible()) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(win, &ww, &wh);
            osd.drawInfoOverlay(ww, wh,
                player.videoWidth(), player.videoHeight(),
                player.videoBitrate(), player.videoFps(), player.videoCodecName(),
                player.audioSampleRate(), player.audioBitrate(), player.audioCodecName(),
                player.usingHardware(), player.duration());
        }

        SDL_RenderPresent(vrender.renderer());
        SDL_Delay(8);
    }

    if (!playlist.empty() && player.hasMedia())
        cfg.history[playlist.current()] = player.clock();
    if (!playlist.empty())
        cfg.lastFile = playlist.current();
    cfg.playMode = (int)playlist.mode();
    cfg.volume = player.volume();
    saveConfig(configPath(), cfg);

    player.close();
    vrender.shutdown();
    titlebar.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
