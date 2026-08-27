#pragma once
#include <string>
#include <unordered_map>
#include <SDL.h>

class GdiTextCache {
public:
    void init(SDL_Renderer* renderer);
    void shutdown();
    void drawText(int x, int y, const std::string& utf8, int ptSize = 12,
                  int r = 200, int g = 200, int b = 200, int a = 255);
    int  measureText(const std::string& utf8, int ptSize);   // 像素宽(不绘制)

private:
    SDL_Texture* renderText(const std::string& utf8, int ptSize,
                            int r, int g, int b, int& outW, int& outH);

    SDL_Renderer* renderer_ = nullptr;
    struct CacheEntry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    std::unordered_map<std::string, CacheEntry> cache_;   // key = "text|pt|r,g,b"
};
