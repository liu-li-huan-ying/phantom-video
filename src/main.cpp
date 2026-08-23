#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_syswm.h>

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
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

static void loadExternalSubtitle(Player& player, const std::string& video,
                                 VideoRenderer* vrender = nullptr) {
    for (const char* ext : { ".srt", ".ass", ".ssa", ".sub" }) {
        std::string cand = replaceExt(video, ext);
        FILE* f = std::fopen(cand.c_str(), "rb");
        if (f) {
            std::fclose(f);
            if (player.loadExternalSubtitle(cand)) {
                std::printf("已加载字幕: %s\n", cand.c_str());
                // Set ASS styles on renderer if available
                if (vrender && (ext[1] == 'a' || ext[1] == 's')) {  // .ass or .ssa
                    std::ifstream ifs(cand, std::ios::binary);
                    if (ifs) {
                        std::string content((std::istreambuf_iterator<char>(ifs)),
                                            std::istreambuf_iterator<char>());
                        vrender->setAssContent(content);
                    }
                }
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
    // M31j: 相对 exe 目录解析（CWD 可能是双击打开的视频所在目录）。
    // 窗口图标与标题栏 logo 同源：assets/icons/vplay.bmp（CMake 拷贝到 build/assets）。
    {
        std::string base = exeDir();
        const char* rels[] = { "assets/icons/vplay.bmp", "ico/vplay.bmp", "ico/vplay.ico" };
        SDL_Surface* icon = nullptr;
        for (auto rel : rels) {
            if (icon) break;
            std::string p = base + rel;
            icon = SDL_LoadBMP(p.c_str());
        }
        if (!icon) {
            fprintf(stderr, "[warn] window icon not found (assets/icons/vplay.bmp)\n");
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
            if (!args[i].empty() && args[i] != "--debug") files.push_back(args[i]);
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

    // M31k: 自动化测试钩子（仅当环境变量存在时生效，正常用户无感知）
    if (const char* s = std::getenv("VPLAYER_AUTOTEST_SEEK")) {
        double t = std::atof(s);
        if (t > 0.0) {
            // 等待首帧就绪后再发起绝对跳转
            std::thread([t, &player]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(4000));
                LOG_INFO("MAIN", "AUTOTEST seek -> %.3f", t);
                player.seek(t);
            }).detach();
        }
    }

    // M32e: 设置面板业务状态（openCurrent 捕获用）
    bool subAutoLoad = true;
    int langIdx = 0, themeIdx = 0;

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
            loadExternalSubtitle(player, p, &vrender);
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
    // M32f.2: 播放列表开合 —— 窗口整体向右膨出面板宽（播放器本体尺寸不变）
    auto applyPlaylistToggle = [&]() {
        bool opening = !panel.isOpen();
        if (opening && (fullscreen || (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED))) {
            vrender.showToast("退出全屏后再打开播放列表");
            return;
        }
        int w = 0, h = 0;
        SDL_GetWindowSize(win, &w, &h);
        int pw;
        if (opening) {
            panel.toggle();               // 先翻转，width() 即为目标宽
            pw = panel.width();
            int newW = w + pw;
            // 超出屏幕右缘则整体左移钳制
            int di = SDL_GetWindowDisplayIndex(win);
            SDL_Rect b{};
            if (di < 0 || SDL_GetDisplayUsableBounds(di, &b) != 0) { b.x = 0; b.w = newW + 100; }
            int px = 0, py = 0;
            SDL_GetWindowPosition(win, &px, &py);
            if (px + newW > b.x + b.w)
                SDL_SetWindowPosition(win, std::max(b.x, b.x + b.w - newW), py);
            SDL_SetWindowSize(win, newW, h);
        } else {
            pw = panel.width();
            panel.toggle();
            SDL_SetWindowSize(win, std::max(640, w - pw), h);
        }
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
                    loadExternalSubtitle(player, e.drop.file, &vrender);
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
                        float v = (float)(mx - lay.volSlX) / lay.volSlW;
                        if (v < 0) v = 0; if (v > 1) v = 1;
                        player.setVolume(v == 0 ? 0.0001f : v);
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
                    bool hitControlTop = false;
                    // ---- M32c: 顶栏点击拦截 ----
                    if (my < VideoRenderer::TOPBAR_H) {
                        int act = vrender.topBarClick(mx, my);
                        // M32f.4: 列表展开时其头部区域让位给面板（关闭钮在此）
                        bool inPanelZone = panel.isOpen() && mx >= winW - panel.width();
                        if (act >= 0) {
                            switch (act) {
                        case VideoRenderer::TB_CAMERA: {
                            auto dir = exeDir() + "shots";
                            std::filesystem::create_directories(dir);
                            char ts[40];
                            time_t now = time(nullptr);
                            strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));
                            std::string path = dir + "\\shot_" + ts + ".png";
                            vrender.setShotPath(path);
                            vrender.showToast("已截图");
                            break;
                        }
                        case VideoRenderer::TB_PIP: vrender.showToast("画中画开发中"); break;      // pip
                        case VideoRenderer::TB_LIST: applyPlaylistToggle(); break;
                        case VideoRenderer::TB_MIN: SDL_MinimizeWindow(win); break;
                        case VideoRenderer::TB_MAX:                                                  // 最大化/还原
                            if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)
                                SDL_RestoreWindow(win);
                            else
                                SDL_MaximizeWindow(win);
                            break;
                        case VideoRenderer::TB_CLOSE: running = false; break;
                        default: break;  // 空白区=拖拽（由 WndProc HTCAPTION 处理）
                        }
                        hitControlTop = true;
                        if (inPanelZone) hitControlTop = false;  // 面板头部让位
                    }
                    if (hitControlTop) break;
                    // ---- M32e: 设置模态点击路由（模态打开时独占）----
                    if (vrender.settingsVisible()) {
                        int sa = vrender.settingsClick(mx, my);
                        if (sa == -2 || sa == -1) vrender.setSettingsVisible(false);
                        else if (sa == 1) {  // 音量标准化
                            bool on = !player.audio().normalization();
                            player.audio().setNormalization(on);
                            vrender.showToast(on ? "音量标准化: 开" : "音量标准化: 关");
                        } else if (sa == 2) {  // 记忆播放位置
                            cfg.resume = cfg.resume ? 0 : 1;
                            vrender.showToast(cfg.resume ? "记忆播放位置: 开" : "记忆播放位置: 关");
                        } else if (sa == 3) {  // 自动播放下一个 = 单曲/循环
                            playlist.setMode(playlist.mode() == PlayMode::Single
                                                 ? PlayMode::Loop : PlayMode::Single);
                            vrender.showToast(playlist.mode() != PlayMode::Single
                                                  ? "自动播放下一个: 开" : "自动播放下一个: 关");
                        } else if (sa == 4) {
                            subAutoLoad = !subAutoLoad;
                            vrender.showToast(subAutoLoad ? "字幕自动加载: 开" : "字幕自动加载: 关");
                        } else if (sa == 0) {
                            vrender.showToast(player.usingHardware()
                                                  ? "硬件解码已启用（重启应用后可切换）"
                                                  : "当前为软解（切换需重启应用）");
                        } else if (sa >= 10 && sa < 13) { langIdx = sa - 10;
                            vrender.showToast("界面语言切换开发中"); }
                        else if (sa >= 20 && sa < 22) { themeIdx = sa - 20;
                            vrender.showToast("主题切换开发中"); }
                        break;
                    }
                    // M16: 播放列表面板事件优先处理
                    if (panel.handleMouseDown(mx, my, winW, winH)) {
                        vrender.setPanelWidth(panel.width());
                        // M32f.2: 面板内部开合请求 → 窗口向右膨出/收回
                        if (panel.consumeToggleRequest()) {
                            applyPlaylistToggle();
                            break;
                        }
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
                    bool hitControl = false;
                    auto inR = [&](int x, int y, int w, int h) {
                        return mx >= x && mx < x + w && my >= y && my < y + h;
                    };
                    // ---- 行1：传输钮（prev34 / play42 / next34）----
                    if (inR(lay.prevX, lay.row1Y, 34, 34)) {
                        prevTrack(); hitControl = true;
                    } else if (inR(lay.playX, lay.row1Y, 42, 42)) {
                        player.togglePause(); hitControl = true;
                    } else if (inR(lay.nextX, lay.row1Y, 34, 34)) {
                        nextTrack(); hitControl = true;
                    }
                    // ---- 行2：字幕 / 倍速 / 画质 / 音量 / 设置 / 全屏 ----
                    else if (inR(lay.subX, lay.row1Y + 4, 44, 34)) {
                        // 字幕：内封字幕轨循环切换（-1=关闭）
                        int cnt = player.subtitleStreamCount();
                        if (cnt <= 0) {
                            vrender.showToast("无内封字幕");
                        } else {
                            static int subIdx = -1;
                            subIdx = (subIdx + 1) % (cnt + 1);
                            player.switchSubtitleTrack(subIdx < cnt ? subIdx : -1);
                            char msg[64];
                            std::snprintf(msg, sizeof(msg), "字幕轨: %s",
                                          subIdx < cnt ? "开启" : "关闭");
                            vrender.showToast(msg);
                        }
                        hitControl = true;
                    }
                    else if (inR(lay.spdX - 9, lay.row1Y + 4, 84, 34)) {
                        vrender.toggleSpeedMenu();
                        volHideAt = SDL_GetTicks() + 2000;
                        hitControl = true;
                    }
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
                    else if (inR(lay.qualX, lay.row1Y + 4, 48, 34)) {
                        vrender.showToast("本地播放，已是最佳画质");
                        hitControl = true;
                    }
                    else if (inR(lay.volBxX, lay.row1Y + 4, 34, 34)) {
                        player.toggleMute();
                        volHideAt = SDL_GetTicks() + 2000;
                        hitControl = true;
                    }
                    else if (inR(lay.volSlX, lay.row1Y + 12, lay.volSlW, 18)) {
                        draggingVolume = true;
                        float v = (float)(mx - lay.volSlX) / lay.volSlW;
                        if (v < 0) v = 0; if (v > 1) v = 1;
                        player.setVolume(v == 0 ? 0.0001f : v);
                        hitControl = true;
                    }
                    else if (inR(lay.setX, lay.row1Y + 4, 44, 34)) {
                        vrender.setSettingsVisible(true);
                        hitControl = true;
                    }
                    else if (inR(lay.fs2X, lay.row1Y + 4, 34, 34)) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win,
                            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        hitControl = true;
                    }
                    // 进度条点击跳转（seek 带命中）
                    if (!hitControl &&
                        mx >= lay.progX && mx < lay.progX + lay.progW &&
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
                case SDLK_b: {
                    // M31b: 字幕轨切换（循环 + 关闭）
                    int count = player.subtitleStreamCount();
                    if (count > 0) {
                        int cur = player.currentSubtitleTrack();
                        int next = cur + 1;
                        if (next >= count) next = -1;  // -1 = 关闭
                        if (next >= 0) {
                            player.switchSubtitleTrack(next);
                            std::string msg = "Sub: " + player.subtitleStreamName(next);
                            vrender.showToast(msg.c_str());
                        } else {
                            player.switchSubtitleTrack(-1);
                            vrender.showToast("Sub: OFF");
                        }
                    } else {
                        vrender.showToast("No subtitle tracks");
                    }
                    volHideAt = SDL_GetTicks() + 2000;
                    break;
                }
                case SDLK_MINUS:
                case SDLK_KP_MINUS: {
                    double d = player.subtitleDelay() - 0.5;
                    if (d < -10.0) d = -10.0;
                    player.setSubtitleDelay(d);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "SubDelay %.1fs", d);
                    vrender.showToast(buf);
                    volHideAt = SDL_GetTicks() + 2000;
                    break;
                }
                case SDLK_EQUALS:
                case SDLK_KP_PLUS: {
                    double d = player.subtitleDelay() + 0.5;
                    if (d > 10.0) d = 10.0;
                    player.setSubtitleDelay(d);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "SubDelay %+.1fs", d);
                    vrender.showToast(buf);
                    volHideAt = SDL_GetTicks() + 2000;
                    break;
                }
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
                // M32e: 设置面板状态镜像
                stats.swHw = player.usingHardware();
                stats.swNorm = player.audio().normalization();
                stats.swResume = cfg.resume != 0;
                stats.swAutoNext = (playlist.mode() != PlayMode::Single);
                stats.swSub = subAutoLoad;
                stats.langIdx = langIdx;
                stats.themeIdx = themeIdx;
                static std::string subtitleBuf;
                static std::string rawSubBuf;
                if (player.hasSubtitle()) {
                    subtitleBuf = player.subtitleText(stats.clock);
                    rawSubBuf = player.rawSubtitleText(stats.clock);
                    stats.subtitle = subtitleBuf.c_str();
                    stats.rawSubtitle = rawSubBuf.c_str();
                } else {
                    subtitleBuf.clear();
                    rawSubBuf.clear();
                    stats.subtitle = nullptr;
                    stats.rawSubtitle = nullptr;
                }
                stats.onPlayPause = [&]() { player.togglePause(); return true; };
                stats.onToggleFullscreen = [&]() { fullscreen = !fullscreen; SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); return true; };
                stats.onSeekTo = [&](double p) { player.seek(p); return true; };
                stats.onVolumeUp = [&]() { player.setVolume(player.volume() + 0.1f); volHideAt = SDL_GetTicks() + 2000; };
                stats.onNextTrack = [&]() { nextTrack(); };
                stats.onPrevTrack = [&]() { prevTrack(); };
                stats.onCycleMode = [&]() { cyclePlayMode(); };
                stats.onCycleSpeed = [&]() { cycleSpeed(1); };
                vrender.setPanelWidth(panel.width());  // M32f.2: 每帧同步面板宽
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
        // M32c: 顶栏由 VideoRenderer::drawTopBar 绘制（覆盖式），不再画旧标题栏
        // titlebar.draw(vrender.renderer());
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
