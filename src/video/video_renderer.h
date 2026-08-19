#pragma once
#include <SDL.h>

#include "core/types.h"

class VideoRenderer {
public:
    ~VideoRenderer();
    bool init(SDL_Window* window);
    void shutdown();
    void render(const AVFrame* frame);
    void clear();
    SDL_Renderer* renderer() const { return renderer_; }
    int frameWidth() const { return fw_; }
    int frameHeight() const { return fh_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int fw_ = 0;
    int fh_ = 0;
};