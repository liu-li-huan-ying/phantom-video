#pragma once
// Pure SDL2 rendering primitives (no state dependencies).
// Extracted from main.cpp to reduce monolithic file size.

#include <SDL.h>
#include <cmath>

// Fill a circle with color
inline void fillCircle(SDL_Renderer* r, int cx, int cy, int rad,
                       Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (rad <= 0) return;
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    for (int dy = -rad; dy <= rad; ++dy) {
        int dx = (int)std::sqrt((float)rad * rad - (float)dy * dy);
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

// Fill a rounded rectangle with color
inline void roundedRectFill(SDL_Renderer* r, int x, int y, int w, int h, int rad,
                            Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (w <= 0 || h <= 0) return;
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_Rect body = {x + rad, y, w - rad * 2, h};
    SDL_RenderFillRect(r, &body);
    SDL_Rect top = {x, y + rad, w, h - rad * 2};
    SDL_RenderFillRect(r, &top);
    fillCircle(r, x + rad, y + rad, rad, cr, cg, cb, ca);
    fillCircle(r, x + w - rad - 1, y + rad, rad, cr, cg, cb, ca);
    fillCircle(r, x + rad, y + h - rad - 1, rad, cr, cg, cb, ca);
    fillCircle(r, x + w - rad - 1, y + h - rad - 1, rad, cr, cg, cb, ca);
}

// Stroke a rounded rectangle outline
inline void roundedRectStroke(SDL_Renderer* r, int x, int y, int w, int h, int rad,
                              Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (w <= 0 || h <= 0) return;
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_RenderDrawLine(r, x + rad, y, x + w - rad - 1, y);
    SDL_RenderDrawLine(r, x + rad, y + h - 1, x + w - rad - 1, y + h - 1);
    SDL_RenderDrawLine(r, x, y + rad, x, y + h - rad - 1);
    SDL_RenderDrawLine(r, x + w - 1, y + rad, x + w - 1, y + h - rad - 1);
    // 四角圆弧 (近似: 内外两层像素)
    for (int i = 0; i <= rad; ++i) {
        int d = (int)(rad - std::sqrt((float)rad * rad - (float)(rad - i) * (rad - i)) + 0.5f);
        SDL_RenderDrawPoint(r, x + i, y + d);
        SDL_RenderDrawPoint(r, x + i, y + h - 1 - d);
        SDL_RenderDrawPoint(r, x + w - 1 - i, y + d);
        SDL_RenderDrawPoint(r, x + w - 1 - i, y + h - 1 - d);
    }
}
