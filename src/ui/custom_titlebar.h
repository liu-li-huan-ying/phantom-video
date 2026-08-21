#pragma once
#include <windows.h>
#include <SDL.h>
#include <string>

class CustomTitlebar {
public:
    void init(HWND hwnd);
    void shutdown();
    void setTitle(const char* title);

    // SDL 绘制标题栏（每帧调用，替代 GDI WM_PAINT）
    void draw(SDL_Renderer* renderer);

    // WndProc hook 相关
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static const int height = 32;  // 标题栏高度

private:
    HWND hwnd_ = nullptr;
    WNDPROC oldWndProc_ = nullptr;
    std::string title_;
    int hoverBtn_ = -1;  // -1=无, 0=最小化, 1=最大化, 2=关闭

    int hitTest(int x, int y);
};
