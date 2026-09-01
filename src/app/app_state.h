#pragma once

// Centralized application state declarations for cross-module access.
// All global variables used by multiple modules are declared extern here
// and defined once in main.cpp.

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/mpv_backend.h"
#include "ui/gdi_text.h"

// ---- UI state (all overlay/WndProc/rendering state) ----
struct UiState {
    bool   visible   = true;
    Uint32 hideAt    = 3000;
    int    mouseX    = -1;
    int    mouseY    = -1;
    int    winW      = 960;
    int    winH      = 540;
    bool   fullscreen = false;

    // seekbar
    bool   seekbarHover  = false;
    bool   seekingDrag   = false;
    double seekTarget    = 0.0;

    // topbar icon hover (-1=none, 0=close, 1=maximize, ...)
    int    topbarHover   = -1;

    // welcome page hit rects (rebuilt each frame during rendering)
    SDL_Rect heroFileBtn   = {};
    SDL_Rect heroFolderBtn = {};
    std::vector<std::pair<std::string, SDL_Rect>> continueHits;
    std::vector<std::pair<int, SDL_Rect>>         gridHits;
    float introAlpha = 1.0f;

    // speed popup
    bool   speedMenuOpen = false;

    // quality popup
    bool   qualityMenuOpen = false;
    int    qualityPreset = 1;

    // EQ popup
    bool   eqMenuOpen = false;
    int    eqDraggingBand = -1;

    // subtitle/audio track popup
    bool subMenuOpen = false;
    bool audioMenuOpen = false;
    bool chapterMenuOpen = false;

    // volume slider
    bool   volumeSliderOpen = false;
    bool   volumeDragging   = false;
    bool   volumeSliderHover = false;

    // toast
    bool   toastActive = false;
    Uint32 toastStart  = 0;
    char   toastMsg[128] = {};

    // settings panel
    bool   settingsOpen = false;

    // playlist panel
    bool   playlistOpen = false;
    int    playlistTargetW = 0;
    int    playlistAnimW = 0;
    int    playlistScroll = 0;
    int    totalW = 960;

    // single-click pause delay (vs double-click fullscreen)
    bool   pendingPause = false;

    // control bar fade (alpha 0..1)
    float  ctrlAlpha = 1.0f;

    // volume slider hover collapse
    Uint32 volHoverAt = 0;

    // playlist drag reorder
    int    plDragFrom = -1;
    int    plDownY = 0;
    bool   plDragging = false;
    int    plDragY = 0;

    // playlist scrollbar
    bool   sbHover = false;
    bool   sbDragging = false;
    int    sbGrabOff = 0;
    int    sbTrackX = -1, sbTrackY = 0, sbTrackW = 0, sbTrackH = 0;
    int    sbBarY = 0, sbBarH = 0;
    SDL_Rect plCloseRect = {};

    // PIP / mini mode
    bool   miniMode  = false;
    RECT   savedRect  = {};
    DWORD  savedStyle = 0;

    // OSD info overlay
    bool   osdActive = false;
    Uint32 osdStart  = 0;

    // Shortcuts help overlay
    bool   shortcutsOpen = false;

    // Image adjustments panel
    bool   imageMenuOpen = false;
    int    imageDraggingSlider = -1;  // -1=none, 0=brightness 1=contrast 2=saturation 3=gamma
};

// ---- extern globals (defined in main.cpp) ----
extern HWND              g_parentHwnd;
extern HWND              g_mpvHwnd;
extern HWND              g_overlayHwnd;
extern std::atomic<bool> g_dirty;
extern MpvBackend*       g_mpv;
extern SDL_Window*       g_sdlWin;
extern SDL_Renderer*     g_sdlRdr;
extern GdiTextCache      g_text;
extern UiState           g_ui;
extern AppConfig         g_cfg;
extern const char*       PHANTOM_VERSION;
extern float             g_dpi;
extern float             g_uiBase;
extern std::vector<std::string> g_playlist;

// ---- scaling helpers (inline, depend on g_dpi/g_uiBase) ----
inline int U(int v) {
    float s = g_dpi * g_uiBase;
    return std::max(1, (int)(v * s + 0.5f));
}

inline int Tpt(int pt) {
    return std::max(8, (int)(pt * g_uiBase + 0.5f));
}

// ---- i18n bilingual string table (0=Chinese 1=English) ----
// T() is defined in main.cpp, reads g_cfg.lang
const char* T(const char* zh, const char* en);

namespace i18n {
    inline const char* subtitles()  { return T("字幕", "Subtitles"); }
    inline const char* audioTrack() { return T("音轨", "Audio"); }
    inline const char* chapName()   { return T("章节", "Chapters"); }
    inline const char* speed()      { return T("倍速", "Speed"); }
    inline const char* quality()    { return T("画质", "Quality"); }
    inline const char* settings()   { return T("设置", "Settings"); }
    inline const char* settingsTitle()  { return T("设置", "Settings"); }
    inline const char* language()       { return T("语言", "Language"); }
    inline const char* chinese()        { return T("中文", "Chinese"); }
    inline const char* english()        { return T("English", "English"); }
    inline const char* hwDecode()       { return T("硬件解码", "Hardware Decode"); }
    inline const char* volNorm()        { return T("音量标准化", "Volume Normalization"); }
    inline const char* subAutoLoad()    { return T("字幕自动加载", "Subtitle Auto-Load"); }
    inline const char* thumbCache()     { return T("缩略图缓存", "Thumbnail Cache"); }
    inline const char* resume()         { return T("续播记忆", "Resume Playback"); }
    inline const char* nightMode()      { return T("夜间模式", "Night Mode"); }
    inline const char* exclusiveAudio() { return T("独占音频", "Exclusive Audio"); }
    inline const char* motionInterp()   { return T("运动插值", "Motion Interpolation"); }
    inline const char* vsInterp()       { return T("VS 插帧", "VS Frame Interp"); }
    inline const char* vsSuperRes()     { return T("VS 超分", "VS Super Res"); }
    inline const char* hiQScaling()     { return T("高质量缩放", "HQ Scaling"); }
    inline const char* playbackMode()   { return T("播放模式", "Playback Mode"); }
    inline const char* modeSingle()     { return T("单曲", "Single"); }
    inline const char* modeLoop()       { return T("循环", "Loop"); }
    inline const char* modeShuffle()    { return T("随机", "Shuffle"); }
    inline const char* playlist()       { return T("播放列表", "Playlist"); }
    inline const char* playing()        { return T("正在播放", "Playing"); }
    inline const char* played()         { return T("已播放", "Played"); }
    inline const char* unplayed()       { return T("未播放", "Unplayed"); }
    inline const char* emptyPlaylist()  { return T("无文件", "No files"); }
    inline const char* dropHint()       { return T("拖入视频文件", "Drop video here"); }
    inline const char* ctrlOHint()      { return T("或按 Ctrl+O", "or press Ctrl+O"); }
    inline const char* ctrlUHint()      { return T("或按 Ctrl+U 打开 URL", "or press Ctrl+U for URL"); }
    inline const char* equalizer()      { return T("均衡器", "Equalizer"); }
    inline const char* reset()          { return T("重置", "Reset"); }
    inline const char* muted()          { return T("已静音", "Muted"); }
    inline const char* unmuted()        { return T("已取消静音", "Unmuted"); }
    inline const char* subtitlesOn()    { return T("字幕已开启", "Subtitles ON"); }
    inline const char* subtitlesOff()   { return T("字幕已关闭", "Subtitles OFF"); }
    inline const char* loopASet()       { return T("已设置 A 点", "Loop A set"); }
    inline const char* loopActive()     { return T("AB 循环中", "AB loop active"); }
    inline const char* loopCleared()    { return T("循环已清除", "Loop cleared"); }
    inline const char* singleTrack()    { return T("单音轨", "Single audio track"); }
    inline const char* playlistReordered() { return T("列表已重排", "Playlist reordered"); }
    inline const char* failedOpen()     { return T("打开失败", "Failed to open file"); }
    inline const char* eqReset()        { return T("EQ 已重置", "EQ reset"); }
    inline const char* eqOn()           { return T("已开启", "ON"); }
    inline const char* eqOff()          { return T("已关闭", "OFF"); }
    inline const char* presetFlat()     { return T("平坦", "Flat"); }
    inline const char* presetBass()     { return T("低音", "Bass"); }
    inline const char* presetTreble()   { return T("高音", "Treble"); }
    inline const char* presetVocal()    { return T("人声", "Vocal"); }
    inline const char* presetRock()     { return T("摇滚", "Rock"); }
    inline const char* noPrev()         { return T("无上一曲", "No previous track"); }
    inline const char* noNext()         { return T("无下一曲", "No next track"); }
    inline const char* screenshotSaved() { return T("截图已保存", "Screenshot saved"); }
    inline const char* screenshotFailed() { return T("截图失败", "Screenshot failed"); }
    inline const char* pipOn()          { return T("画中画已开启", "PIP ON"); }
    inline const char* pipOff()         { return T("画中画已关闭", "PIP OFF"); }
    inline const char* buffering()      { return T("缓冲中...", "Buffering..."); }
    inline const char* endOfTrack()     { return T("播放结束", "End of track"); }
    inline const char* resumedAt()      { return T("已续播", "Resumed at"); }
    inline const char* audioStereo()    { return T("立体声", "Stereo"); }
    inline const char* audio51()        { return T("5.1环绕", "5.1 Surround"); }
    inline const char* audio71()        { return T("7.1环绕", "7.1 Surround"); }
    inline const char* audioPassthrough() { return T("直通(Passthrough)", "Passthrough"); }
    inline const char* modeSingleT()   { return T("模式: 单曲", "Mode: Single"); }
    inline const char* modeLoopT()     { return T("模式: 循环", "Mode: Loop"); }
    inline const char* modeShuffleT()  { return T("模式: 随机", "Mode: Shuffle"); }
    inline const char* appName()       { return T("幻影视频", "Phantom Video"); }
    inline const char* tagline()       { return T("轻 · 快 · 纯粹的本地视频体验", "Light, fast, pure local video"); }
    inline const char* openFile()      { return T("打开文件", "Open File"); }
    inline const char* openFolder()    { return T("打开文件夹", "Open Folder"); }
    inline const char* dropAnywhere()  { return T("或直接拖拽视频到窗口", "or drop a video anywhere"); }
    inline const char* continueWatching() { return T("继续观看", "Continue Watching"); }
    inline const char* folderEmpty()   { return T("该文件夹没有视频文件", "No videos in this folder"); }
    inline const char* debandOff()     { return T("关闭", "Off"); }
    inline const char* debandLight()   { return T("轻", "Light"); }
    inline const char* debandMedium()  { return T("中", "Medium"); }
    inline const char* debandStrong()  { return T("强", "Strong"); }
    inline const char* subBottom()     { return T("底部", "Bottom"); }
    inline const char* subCenter()     { return T("居中", "Center"); }
    inline const char* subTop()        { return T("顶部", "Top"); }
    // 画面调节面板
    inline const char* image()         { return T("画面", "Image"); }
    inline const char* imageTitle()    { return T("画面调节", "Image Adjustments"); }
    inline const char* brightness()    { return T("亮度", "Brightness"); }
    inline const char* contrast()      { return T("对比度", "Contrast"); }
    inline const char* saturation()    { return T("饱和度", "Saturation"); }
    inline const char* gammaLabel()    { return T("Gamma", "Gamma"); }
    inline const char* deinterlaceLabel() { return T("去隔行", "Deinterlace"); }
    inline const char* toneMappingLabel() { return T("色调映射", "Tone Mapping"); }
    inline const char* gamutMappingLabel(){ return T("色域映射", "Gamut Mapping"); }
    inline const char* hdrPeakLabel()     { return T("HDR 峰值检测", "HDR Peak Detect"); }
    inline const char* tmAuto()        { return T("自动", "Auto"); }
    inline const char* tmClip()        { return T("裁剪", "Clip"); }
    inline const char* tmBT2390()      { return T("BT.2390", "BT.2390"); }
    inline const char* tmBT2446A()     { return T("BT.2446A", "BT.2446A"); }
    inline const char* tmST209410()    { return T("ST2094-10", "ST2094-10"); }
    inline const char* gmAuto()        { return T("自动", "Auto"); }
    inline const char* gmPerceptual()  { return T("感知", "Perceptual"); }
    inline const char* gmClip()        { return T("裁剪", "Clip"); }
    inline const char* gmRelative()    { return T("相对色度", "Relative"); }

    // OSD 信息面板标签
    inline const char* osdStream()     { return T("流", "stream"); }
    inline const char* osdContainer()  { return T("容器", "container"); }
    inline const char* osdDisplay()    { return T("显示器", "display"); }
    inline const char* osdVfFps()      { return T("滤镜帧率", "vf-fps"); }
    inline const char* osdVideo()      { return T("视频码率", "video"); }
    inline const char* osdAudio()      { return T("音频码率", "audio"); }
    inline const char* osdSpeed()      { return T("倍速", "speed"); }
    inline const char* osdVol()        { return T("音量", "vol"); }
    inline const char* osdMuted()      { return T("已静音", "MUTED"); }
    inline const char* osdPaused()     { return T("已暂停", "PAUSED"); }
    inline const char* osdHwdec()      { return T("硬件解码", "hwdec"); }
    inline const char* osdFallback()   { return T("降级", "fallback"); }
    inline const char* osdVfChain()    { return T("视频滤镜", "vf"); }
    inline const char* osdAfChain()    { return T("音频滤镜", "af"); }
    inline const char* osdVsOff()      { return T("关闭", "off"); }
    inline const char* osdVsInterp()   { return T("插帧", "interp"); }
    inline const char* osdVsSuperRes() { return T("超分", "superres"); }
    inline const char* osdVsBoth()     { return T("插帧+超分", "interp+superres"); }
    inline const char* osdPixelFmt()   { return T("像素格式", "pixfmt"); }
    inline const char* osdContainerFmt() { return T("容器格式", "container"); }
}

// ---- format helpers ----
void formatTime(char* buf, size_t n, double sec);
std::wstring Utf8ToWide(const std::string& u8);
std::string  WideToUtf8(const std::wstring& ws);
std::string  fileNameOf(const std::string& utf8path);

// ---- constants (extracted from main.cpp for WndProc cross-module access) ----
extern const float SPEED_PRESETS[];
extern const int   SPEED_PRESET_COUNT;
extern const int   TIMER_SINGLECLICK;
extern const int   QUALITY_PRESET_COUNT;
extern const int   SET_ROW_COUNT;
extern const int   SB_MARGIN;

// ---- quality presets ----
struct QualityPreset {
    const char* name;
    const char* scale;
    const char* dscale;
    const char* cscale;
    int         deband;
    float       antiring;
};
extern const QualityPreset QUALITY_PRESETS[];

// ---- control-bar layout (shared by renderOverlay + parentProc) ----
struct Row1Layout {
    SDL_Rect prev, play, next;
    int timeX;
    SDL_Rect subBtn, audioBtn, chapterBtn, speedBtn, qualityBtn, abBtn, eqBtn, setBtn, fullBtn, imgBtn;
    int volIconCx;
    int volSliderX, volSliderW;
    int cy;
};

// ---- settings panel geometry ----
struct SettingsGeom {
    int panelX, panelY, panelW, panelH;
    int closeCx, closeCy, closeR;
    int swX, swW, swH;
    int rowY[12];
    int modeRowY;
    int chipY, chipH, chipW;
    int langRowY;
    int langSegX, langSegW, langSegH;
};

// ---- image panel geometry ----
struct ImagePanelGeom {
    int panelX, panelY, panelW, panelH;
    int closeCx, closeCy, closeR;
    int sliderY[4];       // brightness/contrast/saturation/gamma 行Y
    int switchY;          // 去隔行行Y
    int optY[3];          // toneMapping/gamutMapping/hdrPeak 行Y
    int sliderX, sliderW; // 滑条区左上角X和宽度
    int minusX, plusX;    // 减/加按钮X
    int valX;             // 数值显示X
};

// ---- seekbar geometry (inline, depend on g_ui/g_uiBase) ----
inline int sbTopY()    { return g_ui.winH - U(80); }
inline int sbTrackY()  { return sbTopY() + U(10); }
inline int sbLeftX()   { return U(16); }
inline int sbRightX()  { return g_ui.winW - U(16); }
inline int sbWidth()   { return sbRightX() - sbLeftX(); }
inline int curCtrlH()  { return U(80); }
inline int curTopH()   { return U(52); }

// ---- control-bar layout declaration ----
void layoutRow1(int w, int barTopY, bool volOpen, Row1Layout& L);

// ---- UI helper functions (cross-module) ----
void showToast(const char* msg);
const char* qualityLabel();
void raiseOverlayAbove();
void applyPlaylistWindow(HWND hwnd);
void toggleFullscreen(HWND hwnd);
void toggleMini(HWND hwnd);
void saveWindowPos(HWND hwnd);
void renderOverlay();

// ---- playlist functions (cross-module) ----
void clampPlaylistScroll();
void buildPlaylistAround(const std::string& file);
bool buildPlaylistFromFolder(const std::string& dirUtf8);
void playPath(const std::string& path, bool forceResume = false);
void playIndex(int idx, bool relative = false);
int  playlistIndexOf(const std::string& path);
void addToPlaylist(const std::string& file);
void removeFromPlaylist(int idx);

// ---- settings functions (cross-module) ----
void applySetting(const char* key, int value);
void applyQualityPreset(int idx);
SettingsGeom settingsGeom(int w, int h);

// ---- topbar/volume hit-test ----
int  hitTestTopbarIcon(int mx, int my, int winW);
bool inVolumeArea(int mx, int my);
