#include "ui/osd.h"

#include <cstring>

static const char* kCharset = "0123456789:.% ";

static const unsigned char kFont[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},  // :
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},  // .
    {0x11, 0x12, 0x04, 0x08, 0x10, 0x11, 0x12},  // %
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // (space)
};

OSD::~OSD() {
    for (SDL_Texture* t : glyphs_) SDL_DestroyTexture(t);
    glyphs_.clear();
}

void OSD::init(SDL_Renderer* renderer) {
    r_ = renderer;
    const int scale = 2, gw = 5, gh = 7;
    for (int i = 0; i < (int)strlen(kCharset); ++i) {
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
            0, gw * scale, gh * scale, 32, SDL_PIXELFORMAT_RGBA32);
        if (!surf) continue;
        SDL_LockSurface(surf);
        Uint32* px = (Uint32*)surf->pixels;
        for (int row = 0; row < gh; ++row) {
            for (int col = 0; col < gw; ++col) {
                if (kFont[i][row] & (0x10 >> col)) {
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx)
                            px[(row * scale + dy) * surf->w + col * scale + dx] = 0xFFFFFFFF;
                }
            }
        }
        SDL_UnlockSurface(surf);
        SDL_Texture* tex = SDL_CreateTextureFromSurface(r_, surf);
        SDL_FreeSurface(surf);
        glyphs_.push_back(tex);
    }
}

SDL_Texture* OSD::glyphFor(char c) {
    const char* p = strchr(kCharset, c);
    if (!p) return nullptr;
    size_t idx = (size_t)(p - kCharset);
    return idx < glyphs_.size() ? glyphs_[idx] : nullptr;
}

void OSD::drawText(int x, int y, const std::string& text, int scale) {
    for (char c : text) {
        SDL_Texture* tex = glyphFor(c);
        if (!tex) continue;
        SDL_Rect dst{ x, y, 5 * scale, 7 * scale };
        SDL_RenderCopy(r_, tex, nullptr, &dst);
        x += 5 * scale + 1;
    }
}

void OSD::draw(const std::string& timeText, double pos, double dur, float vol,
               bool volVisible, bool paused, bool hasMedia) {
    if (!r_) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(r_, &w, &h);
    SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);

    if (hasMedia && dur > 0) {
        const int margin = 14, barH = 4;
        int y = h - barH - 8;
        SDL_SetRenderDrawColor(r_, 0, 0, 0, 140);
        SDL_Rect bg{ margin, y, w - margin * 2, barH };
        SDL_RenderFillRect(r_, &bg);
        double ratio = pos / dur;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        SDL_SetRenderDrawColor(r_, 46, 204, 113, 255);
        SDL_Rect fg{ margin, y, (int)((w - margin * 2) * ratio), barH };
        SDL_RenderFillRect(r_, &fg);
    }

    if (hasMedia && !timeText.empty()) {
        int tw = (int)timeText.size() * (5 * 2 + 1) - 1;
        drawText(w - tw - 14, h - 8 - 4 - 7 * 2 - 10, timeText, 2);
    }

    if (volVisible) {
        std::string vtext = "音量 ";
        vtext += std::to_string((int)(vol * 100.0f + 0.5f));
        vtext += '%';
        int tw = (int)vtext.size() * (5 * 2 + 1) - 1;
        drawText(w / 2 - tw / 2, h / 2 - 48, vtext, 2);
        SDL_SetRenderDrawColor(r_, 255, 255, 255, 90);
        SDL_Rect barBg{ w / 2 - 60, h / 2 - 22, 120, 6 };
        SDL_RenderFillRect(r_, &barBg);
        SDL_SetRenderDrawColor(r_, 46, 204, 113, 255);
        int bw = (int)(120.0f * vol);
        if (bw > 0) {
            SDL_Rect barFg{ w / 2 - 60, h / 2 - 22, bw, 6 };
            SDL_RenderFillRect(r_, &barFg);
        }
    }

    if (paused && hasMedia) {
        SDL_SetRenderDrawColor(r_, 255, 255, 255, 220);
        SDL_Rect b1{ w / 2 - 18, h / 2 - 16, 12, 32 };
        SDL_RenderFillRect(r_, &b1);
        SDL_Rect b2{ w / 2 + 6, h / 2 - 16, 12, 32 };
        SDL_RenderFillRect(r_, &b2);
    }
}