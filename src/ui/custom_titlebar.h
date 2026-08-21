#pragma once
#include <windows.h>
#include <SDL.h>
#include <string>

class CustomTitlebar {
public:
    void init(HWND hwnd);
    void shutdown();
    void setTitle(const char* title);

    // SDL 绘制标题栏（每帧调用）
    void draw(SDL_Renderer* renderer);
    // 更新滚动动画（每帧调用）
    void updateScroll();

    // WndProc hook 相关
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static const int height = 32;  // 标题栏高度

private:
    HWND hwnd_ = nullptr;
    WNDPROC oldWndProc_ = nullptr;
    std::string title_;
    int hoverBtn_ = -1;  // -1=无, 0=最小化, 1=最大化, 2=关闭
    // 滚动字幕状态
    float scrollOffset_ = 0.0f;  // 当前滚动偏移（像素）
    int scrollDir_ = 1;          // 1=向左, -1=向右
    Uint32 lastScrollTick_ = 0;  // 上次更新时间
    bool scrollPaused_ = false;  // 暂停（到达端点时）
    Uint32 pauseStart_ = 0;      // 暂停开始时间

    int hitTest(int x, int y);
};
