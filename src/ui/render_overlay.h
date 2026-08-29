#pragma once
// ---- Overlay rendering module ----
// renderOverlay() is declared in app/app_state.h.
// This header exposes overlay lifecycle + thumb texture state for main.cpp.

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

// ---- Thumb state (shared between worker thread + renderOverlay) ----
struct ThumbRgb { int w = 0, h = 0; std::vector<uint8_t> px; };

// ---- Overlay rendering state (used by createOverlay/destroyOverlay in main) ----
struct UlwCtx;
struct RoundMask;
struct GradCache;

extern std::mutex                g_thumbMtx;
extern std::vector<std::string>  g_thumbWant;
extern std::map<std::string, ThumbRgb>  g_thumbRgb;
extern std::map<std::string, SDL_Texture*> g_thumbTex;
extern std::atomic<bool>         g_thumbQuit;
extern std::thread               g_thumbThread;

extern UlwCtx          g_ulw;
extern GradCache       g_gradCache;
extern SDL_Texture*    g_ovTex;
extern int             g_ovTexW;
extern int             g_ovTexH;

struct RoundMask { int x, y, w, h, r; };
extern std::vector<RoundMask> g_roundMasks;

// Thumb worker lifecycle (called from main)
std::string thumbCacheDir();
void thumbCacheCleanup(int keepDays);
void thumbWorkerMain();
void initOverlayThumbs();    // launch worker thread
void shutdownOverlayThumbs(); // stop worker thread

// Overlay texture lifecycle (called from main)
void destroyOverlay();
void destroyGradCache();
