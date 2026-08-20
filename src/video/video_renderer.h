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
    bool draggingVolume = false;
    const char* subtitle = nullptr;
    std::function<void()> onPlayPause;
    std::function<void()> onToggleFullscreen;
    std::function<bool(double)> onSeekTo;
    std::function<void()> onVolumeUp;
    std::function<void()> onNextTrack;
    std::function<void()> onPrevTrack;
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

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int fw_ = 0;
    int fh_ = 0;
    Uint32 lastMouseMove_ = 0;
    bool controlsVisible_ = true;
    int mouseX_ = -1;
    int mouseY_ = -1;
    int controlsAlpha_ = 255;
    void drawControls(const RenderStats& stats);
    void drawSubtitle(const RenderStats& stats);
    void destroySubtitleTexture();
    void* subtitleTexture_ = nullptr;  // SDL_Texture*
    int subTexW_ = 0;
    int subTexH_ = 0;
    std::string subtitleCache_;
};