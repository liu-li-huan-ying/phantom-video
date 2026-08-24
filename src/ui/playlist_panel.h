#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
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
    bool shrinkReady() const;      // M32f.9: 收起动画结束后通知主循环缩窗
    void consumeShrink();
    int width() const;
    void setPlaylist(const Playlist* pl) { playlist_ = pl; }

    void draw(int currentIndex, int winW, int winH);

    bool handleMouseMove(int mx, int my, int winW, int winH);
    bool handleMouseDown(int mx, int my, int winW, int winH);
    bool handleMouseUp(int mx, int my);
    bool handleMouseWheel(int dy, int winW, int winH);

    int clickedIndex() const { return clickedIdx_; }
    void clearClick() { clickedIdx_ = -1; }

    void clearThumbnailCache();

private:
    void loadFormatIcons();
    SDL_Texture* iconForFile(const std::string& path) const;
    std::string extLower(const std::string& path) const;
    void drawItem(int baseX, int y, int index, const std::string& filename,
                  bool isActive, bool isHover, int panelW);

    SDL_Renderer* renderer_ = nullptr;
    bool open_ = false;
    float openAnim_ = 0.0f;
    int baseWidth_ = 430;  // M32f.5: 三分之二（640*2/3≈430）
    int minWidth_ = 430;
    int maxWidth_ = 430;
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
    bool toggleRequested_ = false;
    bool shrinkPending_ = false;   // M32f.9: 关闭动画期间保持宽度
    SDL_Rect closeRect_{ 0,0,0,0 };   // M32f.6: 实绘矩形（命中以此为准）
    SDL_Rect listClip_{ 0,0,0,0 };    // M32f.9: 列表区裁剪（drawItem 内恢复用）

    const Playlist* playlist_ = nullptr;

    struct IconEntry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    std::unordered_map<std::string, IconEntry> iconCache_;
    GdiTextCache textCache_;

    // M33: 后台缩略图提取
    struct ThumbWorker {
        std::thread thread;
        std::atomic<bool> running{ false };
        std::atomic<bool> cancelled{ false };
        std::atomic<int> itemCount{ 0 };
        std::mutex mutex;
        std::vector<std::string> paths;      // 可见项的文件路径快照
        std::atomic<int> nextIdx{ 0 };       // 下一个要提取的 paths[] 下标
        uint8_t* pendingPixels = nullptr;
        int pendingW = 0, pendingH = 0;
        int pendingTargetIdx = -1;           // 对应的播放列表索引
        bool ready = false;
    };
    ThumbWorker worker_;
    std::unordered_map<int, SDL_Texture*> thumbTextures_;

    void startWorker();
    void stopWorker();
    void workerFunc();
    void requestVisibleRange(const std::vector<std::string>& paths,
                             const std::vector<int>& indices);
    void consumeReadyTexture();
};
