#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/player.h"
#include "core/playlist.h"
#include "ui/osd.h"
#include "video/video_renderer.h"

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

    Player player;
    VideoRenderer vrender;
    OSD osd;
    if (!vrender.init(win)) {
        std::printf("鍒涘缓娓叉煋鍣ㄥけ璐? %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    osd.init(vrender.renderer());

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
    } else if (!cfg.lastFile.empty()) {
        // single file: auto-scan its directory into a playlist
        if (!playlist.scanDirectory(cfg.lastFile))
            playlist.set(cfg.lastFile);
    }
    playlist.setMode(static_cast<PlayMode>(cfg.playMode));

    player.setVolume(cfg.volume);

    auto openCurrent = [&]() {
        if (playlist.empty()) return;
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
        if (player.openFile(p)) {
            loadExternalSubtitle(player, p);
            if (cfg.resume) {
                auto it = cfg.history.find(p);
                if (it != cfg.history.end() && it->second > 2.0)
                    player.seek(it->second);
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

    static const float kSpeeds[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
    static const int kSpeedCount = (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0]));
    auto cycleSpeed = [&](int dir) {
        float cur = player.speed();
        int idx = 1;
        for (int i = 0; i < kSpeedCount; ++i)
            if (kSpeeds[i] == cur) { idx = i; break; }
        idx = (idx + dir + kSpeedCount) % kSpeedCount;
        player.setSpeed(kSpeeds[idx]);
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
        volHideAt = SDL_GetTicks() + 2000;
    };

    auto cycleResume = [&]() {
        cfg.resume = cfg.resume ? 0 : 1;
        std::printf("恢复播放位置: %s\n", cfg.resume ? "开启" : "关闭");
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
                    playlist.scanDirectory(e.drop.file);
                    std::string base = e.drop.file;
                    std::size_t slash = base.find_last_of("\\/");
                    if (slash != std::string::npos) base = base.substr(slash + 1);
                    char title[512];
                    std::snprintf(title, sizeof(title), "VPlayer - %s", base.c_str());
                    SDL_SetWindowTitle(win, title);
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
                    vrender.onMouseMove(mx, my);
                    if (draggingProgress) {
                        int winW = 0;
                        SDL_GetWindowSize(win, &winW, nullptr);
                        const int fsX = winW - 52;
                        const int progX = 180;
                        const int progW = fsX - progX - 24 - 96;
                        float pct = (float)(mx - progX) / progW;
                        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
                        player.seek(pct * player.duration());
                    } else if (draggingVolume) {
                        int winH = 0;
                        SDL_GetWindowSize(win, nullptr, &winH);
                        const int barY = winH - 64;
                        const int ph = 90, py = barY - ph - 12;
                        float v = 1.0f - (float)(my - py - 6) / (ph - 12);
                        if (v < 0) v = 0; if (v > 1) v = 1;
                        player.setVolume(v);
                        if (v == 0) { player.setVolume(0.0001f); }
                    }
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                {
                    int mx = e.button.x, my = e.button.y;
                    int winW = 0, winH = 0;
                    SDL_GetWindowSize(win, &winW, &winH);
                    const int barY = winH - 64;
                    const int btnY = barY + 12;
                    // Prev (12, btnY, 40x40)
                    if (mx >= 12 && mx < 52 && my >= btnY && my < btnY + 40) {
                        prevTrack();
                    }
                    // Play/Pause (64, btnY, 40x40)
                    else if (mx >= 64 && mx < 104 && my >= btnY && my < btnY + 40) {
                        player.togglePause();
                    }
                    // Next (116, btnY, 40x40)
                    else if (mx >= 116 && mx < 156 && my >= btnY && my < btnY + 40) {
                        nextTrack();
                    }
                    // Volume button (winW-104, btnY, 40x40) + popup area
                    else if (mx >= winW - 104 && mx < winW - 64 && my >= btnY && my < btnY + 40) {
                        // click toggles mute; drag handled via popup
                        if (!draggingVolume) player.toggleMute();
                        volHideAt = SDL_GetTicks() + 2000;
                    }
                    // Fullscreen button (winW-52, btnY, 40x40)
                    else if (mx >= winW - 52 && mx < winW - 12 && my >= btnY && my < btnY + 40) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win,
                            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    }
                    // Volume popup drag (above vol button)
                    const int ph = 90, py = barY - ph - 12, vx = winW - 104 + 20 - 3;
                    if (mx >= vx && mx < vx + 6 && my >= py && my < py + ph) {
                        draggingVolume = true;
                        float v = 1.0f - (float)(my - py - 6) / (ph - 12);
                        if (v < 0) v = 0; if (v > 1) v = 1;
                        player.setVolume(v == 0 ? 0.0001f : v);
                    }
                    // Progress bar click-to-seek
                    const int progX = 180, progY = barY + 29;
                    const int progW = (winW - 52) - progX - 24 - 96;
                    if (mx >= progX && mx < progX + progW && my >= progY - 6 && my < progY + 12) {
                        float pct = (float)(mx - progX) / progW;
                        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
                        player.seek(pct * player.duration());
                        draggingProgress = true;
                    }
                }
                // Double-click for fullscreen
                if (e.button.clicks == 2) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win,
                        fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                draggingProgress = false;
                draggingVolume = false;
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
            if (f) {
                RenderStats stats;
                stats.playing = (player.state() == Player::State::Playing);
                stats.paused = (player.state() == Player::State::Paused);
                stats.clock = player.clock();
                stats.duration = player.duration();
                stats.volume = player.muted() ? 0.0f : player.volume();
                stats.muted = player.muted();
                stats.fullscreen = fullscreen;
                stats.speed = player.speed();
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
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
