#pragma once
#include <SDL.h>
#include <functional>
#include <string>
#include "core/types.h"
#include "ui/gdi_text.h"
#include "subtitle/ass_renderer.h"

struct RenderStats {
    bool playing = true;
    bool paused = false;
    double clock = 0.0;
    double uiClock = 0.0;  // UI 进度条安全时钟（seek/切倍速期间冻结）
    double duration = 0.0;
    float volume = 1.0f;
    bool muted = false;
    bool fullscreen = false;
    float speed = 1.0f;
    int playMode = 1;  // PlayMode: 0=Single 1=Loop 2=Shuffle
    bool draggingVolume = false;
    const char* subtitle = nullptr;
    const char* rawSubtitle = nullptr;  // 原始 ASS Dialogue 行（含 override tags）
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

// 统一控件布局：M32b 两行结构（进度条 / 传输行 / 功能行）
struct ControlLayout {
    int top = 0;       // 控制栏渐变起始 Y
    // 行1：传输钮（prev/next 34，play 42）
    int row1Y = 0;
    int prevX = 0, playX = 0, nextX = 0, timeX = 0, timeY = 0;
    // 行2：文本/图标钮
    int row2Y = 0;
    int subX = 0, spdX = 0, qualX = 0, volBxX = 0, volSlX = 0, volSlW = 70;
    int setX = 0, fs2X = 0;
    // 进度条命中带（兼容旧字段名）
    int progX = 0, progY = 0, progW = 0;
    // legacy 兼容别名（旧代码引用）
    int barY = 0, btnY = 0, btnSize = 42, gap = 8;
    int modeX = 0, speedX = 0, volX = 0, fsX = 0;
    static ControlLayout compute(int winW, int winH, int panelWidth = 0);
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
    void setPanelWidth(int w) { panelWidth_ = w; }
    int panelWidth() const { return panelWidth_; }
    static SDL_Rect speedMenuItemRect(const ControlLayout& lay, int index);

    // M15: 缩略图预览
    void setThumbnail(SDL_Texture* tex, int w, int h, double timeSec);

    // M31: 轻量 ASS 渲染
    void setAssContent(const std::string& assContent);
    void clearStyledSubtitle();
    SDL_Renderer* renderer() { return renderer_; }
    GdiTextCache gdi_;

    // M16: 暂停叠加图标
    enum class PauseIcon { None, Play, Pause };
    void showPauseOverlay(PauseIcon icon);
    bool isPauseOverlayVisible() const { return pauseOverlayAlpha_ > 0; }
    void showSeekingOverlay();   // M18: 显示 "Seeking..."
    void hideSeekingOverlay();   // M18: 隐藏
    bool isSeekingOverlayVisible() const { return seekingAlpha_ > 0; }

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
    void drawControls(const RenderStats& stats);
    void drawSubtitle(const RenderStats& stats);
    void drawToast();
    void drawBackground();  // M14-C: 深色主题圆角背景
    void drawPauseOverlay(const RenderStats& stats);  // M16: 暂停叠加图标
    void drawSeekingOverlay(const RenderStats& stats);  // M18: Seeking 指示器
    void destroySubtitleTexture();
    void* subtitleTexture_ = nullptr;  // SDL_Texture*
    int subTexW_ = 0;
    int subTexH_ = 0;
    std::string subtitleCache_;
    std::string toastText_;
    Uint32 toastUntil_ = 0;
    bool speedMenuOpen_ = false;
    SDL_Texture* iconTex_[12] = {};
    void ensureIcon(Icon icon);
    SDL_Texture* iconTexture(Icon icon);
    // M15: 缩略图预览
    SDL_Texture* thumbTex_ = nullptr;
    int thumbW_ = 0, thumbH_ = 0;
    double thumbTime_ = -1.0;
    int panelWidth_ = 0;  // M16: 播放列表面板宽度
    // M16: 暂停叠加图标
    int pauseOverlayAlpha_ = 0;   // 0~255 淡入淡出
    Uint32 pauseOverlayUntil_ = 0; // 显示截止时间
    PauseIcon pauseOverlayIcon_ = PauseIcon::None;
    int seekingAlpha_ = 0;         // M18: Seeking 指示器 alpha

    // M30a: 动画状态
    float animControlsAlpha_ = 1.0f;    // 控件 alpha (0~1 浮点)
    float animControlsFrom_ = 1.0f;     // 动画起始值
    float animControlsTo_ = 1.0f;       // 动画目标值
    Uint32 animControlsStart_ = 0;      // 动画起始时间 (SDL_GetTicks)
    static constexpr int kControlsFadeMs = 200; // 过渡时长

    float animTrackH_ = 4.0f;           // 进度条高度 (4~10)
    float animTrackFrom_ = 4.0f;
    float animTrackTo_ = 4.0f;
    Uint32 animTrackStart_ = 0;
    static constexpr int kTrackExpandMs = 180;

    float animThumbScale_ = 1.0f;       // thumb 缩放 (1.0~1.15)
    float animThumbFrom_ = 1.0f;
    float animThumbTo_ = 1.0f;
    Uint32 animThumbStart_ = 0;
    static constexpr int kThumbHoverMs = 160;

    bool wasProgHover_ = false;         // 上一帧进度条 hover 状态
    bool wasControlsHover_ = false;     // 上一帧控件区 hover 状态

    // M31: 轻量 ASS 渲染
    ASSRenderer assRenderer_;
    RenderedSubtitle styledSub_;        // 当前渲染的样式字幕
    std::string styledSubCache_;        // 样式字幕缓存（原始 ASS 行）
    bool assRendererInit_ = false;
};