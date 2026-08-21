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
    int width() const;
    void setPlaylist(const Playlist* pl) { playlist_ = pl; }

    void draw(int currentIndex, int winH);

    bool handleMouseMove(int mx, int my, int winH);
    bool handleMouseDown(int mx, int my, int winH);
    bool handleMouseUp(int mx, int my);
    bool handleMouseWheel(int dy, int winH);

    int clickedIndex() const { return clickedIdx_; }
    void clearClick() { clickedIdx_ = -1; }

private:
    void loadFormatIcons();
    SDL_Texture* iconForFile(const std::string& path) const;
    std::string extLower(const std::string& path) const;
    void drawItem(int y, int index, const std::string& filename,
                  bool isActive, bool isHover, int winH);

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
    int clickedIdx_ = -1;
    bool dragStarted_ = false;

    const Playlist* playlist_ = nullptr;

    struct IconEntry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    std::unordered_map<std::string, IconEntry> iconCache_;
    GdiTextCache textCache_;
};
