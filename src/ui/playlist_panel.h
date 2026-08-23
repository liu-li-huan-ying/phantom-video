#pragma once
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL.h>
#include <SDL_image.h>

#include "ui/gdi_text.h"

class Playlist;

class PlaylistPanel {
public:
    void init(SDL_Renderer* renderer);
    void shutdown();
    void toggle();
    bool isOpen() const { return open_; }
    // M32f.2: 面板内部不再自行开合，仅发出请求；窗口尺寸变化由主循环统一处理
    bool consumeToggleRequest();
    int width() const;
    void setPlaylist(const Playlist* pl) { playlist_ = pl; }

    void draw(int currentIndex, int winW, int winH);

    bool handleMouseMove(int mx, int my, int winW, int winH);
    bool handleMouseDown(int mx, int my, int winW, int winH);
    bool handleMouseUp(int mx, int my);
    bool handleMouseWheel(int dy, int winW, int winH);

    int clickedIndex() const { return clickedIdx_; }
    void clearClick() { clickedIdx_ = -1; }

private:
    void loadFormatIcons();
    SDL_Texture* iconForFile(const std::string& path) const;
    std::string extLower(const std::string& path) const;
    void drawItem(int baseX, int y, int index, const std::string& filename,
                  bool isActive, bool isHover, int panelW);

    SDL_Renderer* renderer_ = nullptr;
    bool open_ = false;
    float openAnim_ = 0.0f;
    int baseWidth_ = 200;
    int minWidth_ = 160;
    int maxWidth_ = 280;
    bool resizing_ = false;
    int resizeStartX_ = 0;
    int resizeStartW_ = 0;
    int scrollOffset_ = 0;
    int hoverIndex_ = -1;
    bool toggleHover_ = false;
    bool closeHover_ = false;
    int mx_ = 0, my_ = 0;  // 鼠标位置（供 draw 关闭按钮 hover 检测）
    int clickedIdx_ = -1;
    bool dragStarted_ = false;
    bool toggleRequested_ = false;  // M32f.2: 内部开合请求（主循环消费）

    const Playlist* playlist_ = nullptr;

    struct IconEntry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    std::unordered_map<std::string, IconEntry> iconCache_;
    GdiTextCache textCache_;
};
