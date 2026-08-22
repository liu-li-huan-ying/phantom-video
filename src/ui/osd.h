#pragma once
#include <string>
#include <vector>

#include <SDL.h>

class OSD {
public:
    ~OSD();
    void init(SDL_Renderer* renderer);
    void draw(const std::string& timeText, double pos, double dur, float vol,
              bool volVisible, bool paused, bool hasMedia);
    void drawInfoOverlay(int winW, int winH,
                         int vW, int vH, int vBitrate, float vFps, const char* vCodec,
                         int aRate, int aBitrate, const char* aCodec,
                         bool hw, double dur);
    void toggleInfo();
    bool isInfoVisible() const { return infoVisible_; }

private:
    void drawText(int x, int y, const std::string& text, int scale);
    SDL_Texture* glyphFor(char c);

    SDL_Renderer* r_ = nullptr;
    std::vector<SDL_Texture*> glyphs_;
    bool infoVisible_ = false;
    Uint32 infoToggleTime_ = 0;
};