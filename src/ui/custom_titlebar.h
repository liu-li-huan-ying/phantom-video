#pragma once
#include <windows.h>
#include <string>
#include <functional>

class CustomTitlebar {
public:
    void init(HWND hwnd);
    void shutdown();
    void setTitle(const char* title);
    void draw();  // GDI 绘制标题栏

    // WndProc hook 相关
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND hwnd_ = nullptr;
    WNDPROC oldWndProc_ = nullptr;
    std::string title_;
    int hoverBtn_ = -1;  // -1=无, 0=最小化, 1=最大化, 2=关闭

    int hitTest(int x, int y);
    void drawButton(int btnIndex, bool hover);
};
