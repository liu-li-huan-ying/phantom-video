#pragma once
#include <SDL.h>
#include <functional>
#include <string>
#include "core/types.h"

struct RenderStats {
    bool playing = true;
    bool paused = false;
    double clock = 0.0;
    double duration = 0.0;
    float volume = 1.0f;
    bool muted = false;
    bool fullscreen = false;
    float speed = 1.0f;
    int playMode = 1;  // PlayMode: 0=Single 1=Loop 2=Shuffle
    bool draggingVolume = false;
    const char* subtitle = nullptr;
    std::function<void()> onPlayPause;
    std::function<void()> onToggleFullscreen;
    std::function<bool(double)> onSeekTo;
    std::function<void()> onVolumeUp;
    std::function<void()> onNextTrack;
    std::function<void()> onPrevTrack;
    std::function<void()> onCycleMode;
    std::function<void()> onCycleSpeed;
};

// 控件图标（Material Icons 贴图，加载失败时回退矢量绘制）
enum class Icon { Play, Pause, Prev, Next, Volume, Mute, Fullscreen, ExitFullscreen,
                  Single, Loop, Shuffle };

// 统一控件布局：进度条贴底独立全宽，按钮行在其上方
struct ControlLayout {
    int barY = 0;      // 按钮行上边缘
    int btnY = 0;      // 按钮上边缘
    int btnSize = 40;
    int gap = 12;
    int prevX = 0, playX = 0, nextX = 0, modeX = 0, speedX = 0, volX = 0, fsX = 0;
    int progX = 0, progY = 0, progW = 0;  // 进度条（贴底全宽）
    static ControlLayout compute(int winW, int winH);
};

class VideoRenderer {
public:
    ~VideoRenderer();
    bool init(SDL_Window* window);
    void shutdown();
    void render(const AVFrame* frame, const RenderStats& stats);
    void clear();
    SDL_Renderer* renderer() const { return renderer_; }
    int frameWidth() const { return fw_; }
    int frameHeight() const { return fh_; }
    void onMouseMove(int x, int y);
    void onMouseClick(int x, int y, const RenderStats& stats);
    bool isPointInRect(int px, int py, const SDL_Rect& rect);
    bool controlsVisible() const { return controlsVisible_; }
    void showControls();
    void showToast(const char* text);
    void toggleSpeedMenu();
    bool speedMenuOpen() const { return speedMenuOpen_; }
    void setSpeedMenuOpen(bool b) { speedMenuOpen_ = b; }
    // 倍速菜单项矩形（供 main.cpp 命中检测）
    static SDL_Rect speedMenuItemRect(const ControlLayout& lay, int index);

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int fw_ = 0;
    int fh_ = 0;
    int pixFmt_ = 0;  // AVPixelFormat of current texture
    void* swsCtx_ = nullptr;     // SwsContext* for format conversion fallback
    void* convFrame_ = nullptr;  // AVFrame* converted to YUV420P
    int convW_ = 0;
    int convH_ = 0;
    int convSrcFmt_ = 0;
    Uint32 lastMouseMove_ = 0;
    bool controlsVisible_ = true;
    int mouseX_ = -1;
    int mouseY_ = -1;
    int controlsAlpha_ = 255;
    void drawControls(const RenderStats& stats);
    void drawSubtitle(const RenderStats& stats);
    void drawToast();
    void destroySubtitleTexture();
    void* subtitleTexture_ = nullptr;  // SDL_Texture*
    int subTexW_ = 0;
    int subTexH_ = 0;
    std::string subtitleCache_;
    std::string toastText_;
    Uint32 toastUntil_ = 0;
    bool speedMenuOpen_ = false;
    SDL_Texture* iconTex_[12] = {};  // Icon enum -> PNG texture (loaded lazily)
    void ensureIcon(Icon icon);
    SDL_Texture* iconTexture(Icon icon);
};