#pragma once
#include <windows.h>
#include <SDL.h>
#include <string>

class CustomTitlebar {
public:
    void init(HWND hwnd);
    void shutdown();
    void setTitle(const char* title);
    void draw(SDL_Renderer* renderer);

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static const int height = 32;

private:
    HWND hwnd_ = nullptr;
    WNDPROC oldWndProc_ = nullptr;
    std::string title_;
    int hoverBtn_ = -1;

    // 滚动字幕
    float scrollOffset_ = 0.0f;
    int scrollDir_ = 1;
    Uint32 lastScrollTick_ = 0;
    bool scrollPaused_ = false;
    Uint32 pauseStart_ = 0;

    // GDI 文字纹理缓存
    std::string cachedText_;
    SDL_Texture* textTex_ = nullptr;
    int textTexW_ = 0, textTexH_ = 0;

    int hitTest(int x, int y);
    SDL_Texture* renderTextGDI(SDL_Renderer* renderer, const char* text, int fontSize,
                               Uint8 r, Uint8 g, Uint8 b, int& outW, int& outH);
};
