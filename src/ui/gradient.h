#pragma once
// Dithered dim + gradient bar rendering with texture caching.
// Extracted from main.cpp to reduce monolithic file size.

#include <SDL.h>

// GradKey: texture cache key for gradient/dither textures
struct GradKey {
    int w = 0, h = 0;
    Uint8 cr = 0, cg = 0, cb = 0, aTop = 0, aBot = 0;
    bool operator==(const GradKey& o) const {
        return w == o.w && h == o.h && cr == o.cr && cg == o.cg &&
               cb == o.cb && aTop == o.aTop && aBot == o.aBot;
    }
};

// Gradient bar cache (3 slots: 0=control bar, 1=playlist, 2=dither dim)
struct GradCache {
    SDL_Texture* tex[3] = { nullptr, nullptr, nullptr };
    GradKey      key[3]  = {};

    void destroy() {
        for (auto& t : tex)
            if (t) { SDL_DestroyTexture(t); t = nullptr; }
    }
};

// Draw dithered dim overlay (Bayer dither simulates semi-transparency)
// Uses its own cached texture (slot 2)
inline void drawDitherDim(SDL_Renderer* r, int x, int y, int w, int h,
                          Uint8 cr, Uint8 cg, Uint8 cb, Uint8 alpha,
                          GradCache& cache) {
    static const int bayer[4][4] = {
        {  0, 136,  34, 170},
        {204,  68, 238, 102},
        { 51, 187,  17, 153},
        {255, 119, 221,  85}
    };
    if (w <= 0 || h <= 0) return;
    GradKey key{ w, h, cr, cg, cb, alpha, alpha };

    // slot 2 专用缓存
    static SDL_Texture* dimTex = nullptr;
    static GradKey      dimKey = {};
    if (!dimTex || !(dimKey == key)) {
        if (dimTex) { SDL_DestroyTexture(dimTex); dimTex = nullptr; }
        SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!tex) return;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        Uint32* pixels = nullptr; int pitch = 0;
        if (SDL_LockTexture(tex, nullptr, (void**)&pixels, &pitch) == 0) {
            Uint32 rgb = ((Uint32)cr << 16) | ((Uint32)cg << 8) | cb;
            for (int dy = 0; dy < h; ++dy) {
                int by = dy % 4;
                Uint32* row = (Uint32*)((Uint8*)pixels + dy * pitch);
                for (int dx = 0; dx < w; ++dx) {
                    row[dx] = (alpha > bayer[by][dx % 4])
                            ? (0xFF000000u | rgb)
                            : 0x00000000u;
                }
            }
            SDL_UnlockTexture(tex);
        }
        dimTex = tex;
        dimKey = key;
    }
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(r, dimTex, nullptr, &dst);
}

// Draw gradient bar with linear alpha interpolation (top to bottom)
inline void drawGradientBar(SDL_Renderer* r, int slot, int x, int y, int w, int h,
                            Uint8 cr, Uint8 cg, Uint8 cb, Uint8 aTop, Uint8 aBot,
                            GradCache& cache) {
    if (w <= 0 || h <= 0) return;
    GradKey key{ w, h, cr, cg, cb, aTop, aBot };

    if (!cache.tex[slot] || !(cache.key[slot] == key)) {
        if (cache.tex[slot]) { SDL_DestroyTexture(cache.tex[slot]); cache.tex[slot] = nullptr; }
        SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!tex) return;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        Uint32* pixels = nullptr; int pitch = 0;
        if (SDL_LockTexture(tex, nullptr, (void**)&pixels, &pitch) == 0) {
            Uint32 rgb = ((Uint32)cr << 16) | ((Uint32)cg << 8) | cb;
            for (int dy = 0; dy < h; ++dy) {
                int a = aTop + ((int)(aBot - aTop)) * dy / h;
                Uint32* row = (Uint32*)((Uint8*)pixels + dy * pitch);
                for (int dx = 0; dx < w; ++dx) {
                    row[dx] = ((Uint32)a << 24) | rgb;
                }
            }
            SDL_UnlockTexture(tex);
        }
        cache.tex[slot] = tex;
        cache.key[slot] = key;
    }
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(r, cache.tex[slot], nullptr, &dst);
}
