#include "ui/playlist_panel.h"
#include "core/playlist.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <windows.h>

static const int kItemH = 36;
static const int kHeaderH = 32;
static const int kEdgeW = 8;
static const int kResizeW = 4;
static const SDL_Color kBgColor{ 25, 25, 25, 255 };
static const SDL_Color kHeaderBg{ 30, 30, 30, 255 };
static const SDL_Color kItemHover{ 50, 50, 50, 255 };
static const SDL_Color kItemActive{ 77, 144, 255, 40 };
static const SDL_Color kTextColor{ 200, 200, 200, 255 };
static const SDL_Color kActiveTextColor{ 77, 144, 255, 255 };
static const SDL_Color kSeparator{ 40, 40, 40, 255 };

static std::string getExeDir() {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    auto pos = s.find_last_of("\\/");
    return pos != std::string::npos ? s.substr(0, pos) : ".";
}

void PlaylistPanel::init(SDL_Renderer* renderer) {
    renderer_ = renderer;
    loadFormatIcons();
    textCache_.init(renderer);
}

void PlaylistPanel::shutdown() {
    for (auto& [k, v] : iconCache_) {
        if (v.tex) SDL_DestroyTexture(v.tex);
    }
    iconCache_.clear();
    textCache_.shutdown();
}

void PlaylistPanel::toggle() {
    open_ = !open_;
    if (!open_) {
        resizing_ = false;
    }
}

int PlaylistPanel::width() const {
    return open_ ? baseWidth_ : 0;
}

void PlaylistPanel::loadFormatIcons() {
    std::string dir = getExeDir() + "/assets/icons/formats";
    if (!std::filesystem::exists(dir)) return;

    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".png") continue;
        std::string ext = entry.path().stem().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        SDL_Surface* surf = IMG_Load(entry.path().string().c_str());
        if (!surf) continue;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
        if (tex) {
            iconCache_[ext] = { tex, surf->w, surf->h };
        }
        SDL_FreeSurface(surf);
    }
}

std::string PlaylistPanel::extLower(const std::string& path) const {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

SDL_Texture* PlaylistPanel::iconForFile(const std::string& path) const {
    std::string ext = extLower(path);
    auto it = iconCache_.find(ext);
    if (it != iconCache_.end()) return it->second.tex;
    return nullptr;
}

void PlaylistPanel::draw(int currentIndex, int winW, int winH) {
    const int titleH = 32;
    const int barH = 64;
    const int progH = 9;  // 进度条区域高度
    int barY = winH - progH - 4 - barH;  // 控件栏顶部

    // 动画更新
    if (open_ && openAnim_ < 1.0f)
        openAnim_ = std::min(1.0f, openAnim_ + 0.10f);
    else if (!open_ && openAnim_ > 0.0f)
        openAnim_ = std::max(0.0f, openAnim_ - 0.10f);

    // 切换按钮：始终绘制在窗口右边缘（全高，方便点击）
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_,
        toggleHover_ ? 100 : 50, toggleHover_ ? 100 : 50,
        toggleHover_ ? 100 : 50, toggleHover_ ? 180 : 100);
    SDL_Rect toggleBtn{ winW - kEdgeW, 0, kEdgeW, winH };
    SDL_RenderFillRect(renderer_, &toggleBtn);

    // 按钮上的箭头指示
    int cx = winW - kEdgeW / 2;
    int cy = (titleH + barY) / 2;  // 在面板区域内居中
    SDL_SetRenderDrawColor(renderer_, 200, 200, 200, toggleHover_ ? 220 : 140);
    for (int i = -3; i <= 3; ++i) {
        int dx = open_ ? -1 : 1;
        SDL_RenderDrawPoint(renderer_, cx + dx * abs(i), cy + i);
    }

    if (openAnim_ < 0.01f) return;

    // 面板区域（仅在标题栏下方和控件上方之间）
    int pw = (int)(baseWidth_ * openAnim_);
    int panelX = winW - pw;
    int panelTop = titleH;
    int panelH = barY - titleH;  // 高度 = 控件栏顶部 - 标题栏底部
    int itemsY = panelTop + kHeaderH;  // 头部下方开始显示列表项
    int visibleH = panelH - kHeaderH;  // 可滚动区域
    int totalItems = playlist_ ? (int)playlist_->size() : 0;
    int contentH = totalItems * kItemH;
    int maxScroll = std::max(0, contentH - visibleH);
    scrollOffset_ = std::min(scrollOffset_, maxScroll);

    // 面板背景（仅覆盖 panelTop 到 barY）
    SDL_Rect panelBg{ panelX, panelTop, pw, panelH };
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, kBgColor.r, kBgColor.g, kBgColor.b, kBgColor.a);
    SDL_RenderFillRect(renderer_, &panelBg);

    // 左边缘分隔线
    SDL_SetRenderDrawColor(renderer_, 55, 55, 55, 255);
    SDL_RenderDrawLine(renderer_, panelX, panelTop, panelX, barY);

    // Header 背景
    SDL_Rect headerBg{ panelX, panelTop, pw, kHeaderH };
    SDL_SetRenderDrawColor(renderer_, kHeaderBg.r, kHeaderBg.g, kHeaderBg.b, kHeaderBg.a);
    SDL_RenderFillRect(renderer_, &headerBg);

    // Header 下分隔线
    SDL_SetRenderDrawColor(renderer_, kSeparator.r, kSeparator.g, kSeparator.b, kSeparator.a);
    SDL_RenderDrawLine(renderer_, panelX, panelTop + kHeaderH - 1, panelX + pw, panelTop + kHeaderH - 1);

    // Header 文字
    std::string headerText = "播放列表";
    if (playlist_ && playlist_->size() > 0) {
        headerText += " (" + std::to_string(playlist_->size()) + ")";
    }
    textCache_.drawText(panelX + 12, panelTop + 8, headerText, 13, 200, 200, 200);

    // Clip to items area
    SDL_Rect clipRect{ panelX, itemsY, pw, visibleH };
    SDL_RenderSetClipRect(renderer_, &clipRect);

    // 绘制列表项
    for (int i = 0; i < totalItems; ++i) {
        int y = itemsY + i * kItemH - scrollOffset_;
        if (y + kItemH < itemsY - kItemH || y > itemsY + visibleH + kItemH) continue;

        bool isActive = (i == currentIndex);
        bool isHover = (i == hoverIndex_);
        std::string filename = std::filesystem::path(playlist_->fileAt(i)).filename().string();
        drawItem(panelX, y, i, filename, isActive, isHover, pw);
    }

    SDL_RenderSetClipRect(renderer_, nullptr);

    // 滚动条
    if (contentH > visibleH) {
        int sbH = std::max(30, (int)((float)visibleH / contentH * visibleH));
        int sbY = itemsY + (int)((float)scrollOffset_ / contentH * visibleH);
        SDL_Rect sb{ panelX + pw - 3, sbY, 2, sbH };
        SDL_SetRenderDrawColor(renderer_, 80, 80, 80, 120);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(renderer_, &sb);
    }

    // 拖拽调整宽度手柄（面板左边缘）
    if (open_) {
        SDL_Rect resizeHandle{ panelX, panelTop, kResizeW, panelH };
        SDL_SetRenderDrawColor(renderer_, 70, 70, 70, 180);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(renderer_, &resizeHandle);
    }
}

void PlaylistPanel::drawItem(int baseX, int y, int index, const std::string& filename,
                              bool isActive, bool isHover, int panelW) {
    SDL_Rect itemBg{ baseX, y, panelW, kItemH };

    // Background
    if (isActive) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, kItemActive.r, kItemActive.g, kItemActive.b, kItemActive.a);
        SDL_RenderFillRect(renderer_, &itemBg);
    } else if (isHover) {
        SDL_SetRenderDrawColor(renderer_, kItemHover.r, kItemHover.g, kItemHover.b, kItemHover.a);
        SDL_RenderFillRect(renderer_, &itemBg);
    }

    // Format icon
    SDL_Texture* icon = iconForFile(filename);
    int iconX = baseX + 8;
    int iconY = y + (kItemH - 24) / 2;
    if (icon) {
        SDL_Rect dst{ iconX, iconY, 24, 24 };
        SDL_RenderCopy(renderer_, icon, nullptr, &dst);
    } else {
        SDL_Rect fallback{ iconX, iconY, 24, 24 };
        SDL_SetRenderDrawColor(renderer_, 80, 80, 80, 180);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(renderer_, &fallback);
    }

    // Index number
    int textX = baseX + 36;
    std::string numStr = std::to_string(index + 1) + ". ";
    int cr = isActive ? 77 : 180;
    int cg = isActive ? 144 : 180;
    int cb = isActive ? 255 : 180;
    textCache_.drawText(textX, y + 9, numStr, 11, cr, cg, cb);

    // Filename
    int nameX = textX + 30;
    int nameW = panelW - (nameX - baseX) - 8;
    if (nameW > 0) {
        textCache_.drawText(nameX, y + 9, filename, 11,
                           isActive ? 77 : 200,
                           isActive ? 144 : 200,
                           isActive ? 255 : 200);
    }
}

bool PlaylistPanel::handleMouseMove(int mx, int my, int winW, int winH) {
    int w = width();
    const int titleH = 32;
    const int barH = 64;
    const int progH = 9;
    int barY = winH - progH - 4 - barH;
    int panelTop = titleH;
    int panelBot = barY;

    // 切换按钮 hover（始终在窗口右边缘）
    toggleHover_ = (mx >= winW - kEdgeW && mx < winW && my >= 0 && my < winH);

    if (w == 0) return false;
    int panelX = winW - w;

    // 拖拽调整宽度手柄
    if (open_ && mx >= panelX && mx < panelX + kResizeW && my >= panelTop && my < panelBot) {
        SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE));
        if (resizing_) {
            int delta = resizeStartX_ - mx;
            baseWidth_ = std::max(minWidth_, std::min(maxWidth_, resizeStartW_ + delta));
        }
        return true;
    } else if (!resizing_) {
        SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
    }

    // 列表项 hover
    if (mx >= panelX && mx < panelX + w && my >= panelTop + kHeaderH && my < panelBot) {
        int idx = (my - panelTop - kHeaderH + scrollOffset_) / kItemH;
        if (playlist_ && idx >= 0 && idx < (int)playlist_->size()) {
            hoverIndex_ = idx;
        } else {
            hoverIndex_ = -1;
        }
    } else {
        hoverIndex_ = -1;
    }

    return mx >= panelX && my >= panelTop && my < panelBot;
}

bool PlaylistPanel::handleMouseDown(int mx, int my, int winW, int winH) {
    int w = width();
    const int titleH = 32;
    const int barH = 64;
    const int progH = 9;
    int barY = winH - progH - 4 - barH;
    int panelTop = titleH;
    int panelBot = barY;

    // 切换按钮点击
    if (mx >= winW - kEdgeW && mx < winW && my >= 0 && my < winH) {
        toggle();
        return true;
    }

    if (w == 0) return false;
    int panelX = winW - w;

    // 拖拽调整宽度
    if (open_ && mx >= panelX && mx < panelX + kResizeW && my >= panelTop && my < panelBot) {
        resizing_ = true;
        resizeStartX_ = mx;
        resizeStartW_ = baseWidth_;
        return true;
    }

    // 列表项点击
    if (mx >= panelX && mx < panelX + w && my >= panelTop + kHeaderH && my < panelBot) {
        int idx = (my - panelTop - kHeaderH + scrollOffset_) / kItemH;
        if (playlist_ && idx >= 0 && idx < (int)playlist_->size()) {
            clickedIdx_ = idx;
        }
        return true;
    }

    return mx >= panelX && my >= panelTop && my < panelBot;
}

bool PlaylistPanel::handleMouseUp(int mx, int my) {
    (void)mx; (void)my;
    if (resizing_) {
        resizing_ = false;
        return true;
    }
    return false;
}

bool PlaylistPanel::handleMouseWheel(int dy, int winW, int winH) {
    int w = width();
    if (w == 0) return false;
    if (!playlist_) return false;

    const int titleH = 32;
    const int barH = 64;
    const int progH = 9;
    int barY = winH - progH - 4 - barH;
    int panelTop = titleH;
    int panelBot = barY;
    int visibleH = panelBot - panelTop - kHeaderH;
    int totalItems = (int)playlist_->size();
    int contentH = totalItems * kItemH;
    int maxScroll = std::max(0, contentH - visibleH);

    scrollOffset_ -= dy * 20;
    scrollOffset_ = std::max(0, std::min(maxScroll, scrollOffset_));
    return true;
}
