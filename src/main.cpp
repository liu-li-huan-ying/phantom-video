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
    if (args.size() > 1 && !args[1].empty()) {
        std::vector<std::string> files;
        for (std::size_t i = 1; i < args.size(); ++i)
            if (!args[i].empty()) files.push_back(args[i]);
        playlist.set(files);
    } else if (!cfg.lastFile.empty()) {
        playlist.set(cfg.lastFile);
    }

    player.setVolume(cfg.volume);

    auto openCurrent = [&]() {
        if (playlist.empty()) return;
        const std::string& p = playlist.current();
        std::string base = p;
        std::size_t slash = base.find_last_of("\\/");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        SDL_SetWindowTitle(win, ("VPlayer - " + base).c_str());
        if (player.openFile(p)) {
            auto it = cfg.history.find(p);
            if (it != cfg.history.end() && it->second > 2.0)
                player.seek(it->second);
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

    auto nextTrack = [&]() {
        if (playlist.next()) openCurrent();
    };
    auto prevTrack = [&]() {
        if (playlist.prev()) openCurrent();
    };

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_DROPFILE:
                if (player.openFile(e.drop.file))
                    std::printf("宸叉墦寮€: %s\n", e.drop.file);
                else
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "鎵撳紑澶辫触",
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
                        float pct = (float)(mx - 100) / (winW - 120);
                        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
                        player.seek(pct * player.duration());
                    }
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                {
                    int mx = e.button.x, my = e.button.y;
                    int winW = 0, winH = 0;
                    SDL_GetWindowSize(win, &winW, &winH);
                    int barY = winH - 60;
                    if (my >= barY + 18 && my < barY + 42) {
                        // Prev button (16, barY+18, 20x24)
                        if (mx >= 16 && mx < 36) {
                            prevTrack();
                        }
                        // Play/Pause button (44, barY+18, 24x24)
                        else if (mx >= 44 && mx < 68) {
                            player.togglePause();
                        }
                        // Next button (76, barY+18, 20x24)
                        else if (mx >= 76 && mx < 96) {
                            nextTrack();
                        }
                     // Volume button (winW-60, barY+18, 20x24)
                        else if (mx >= winW - 60 && mx < winW - 40) {
                            player.toggleMute();
                            volHideAt = SDL_GetTicks() + 2000;
                        }
                        // Fullscreen button (winW-40, barY+18, 20x24)
                        else if (mx >= winW - 40 && mx < winW - 20) {
                            fullscreen = !fullscreen;
                            SDL_SetWindowFullscreen(win,
                                fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        }
                    }
                    // Progress bar click-to-seek (100, barY+28, progressWidthx6)
                    if (mx >= 100 && mx < winW - 20 && my >= barY + 28 && my < barY + 34) {
                        float pct = (float)(mx - 100) / (winW - 120);
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
                case SDLK_f:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win,
                        fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
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
    cfg.volume = player.volume();
    saveConfig(configPath(), cfg);

    player.close();
    vrender.shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
