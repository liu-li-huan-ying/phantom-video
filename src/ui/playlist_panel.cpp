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

void PlaylistPanel::draw(int currentIndex, int winH) {
    if (openAnim_ < 0.01f && !open_) return;

    if (open_ && openAnim_ < 1.0f)
        openAnim_ = std::min(1.0f, openAnim_ + 0.08f);
    else if (!open_ && openAnim_ > 0.0f)
        openAnim_ = std::max(0.0f, openAnim_ - 0.08f);

    if (openAnim_ < 0.01f) return;

    int w = (int)(baseWidth_ * openAnim_);
    int panelX = (int)(960 * openAnim_); // placeholder, will be overridden by caller via clip
    int itemsY = kHeaderH;
    int visibleH = winH - itemsY;
    int totalItems = playlist_ ? (int)playlist_->size() : 0;
    int contentH = totalItems * kItemH;
    int maxScroll = std::max(0, contentH - visibleH);
    scrollOffset_ = std::min(scrollOffset_, maxScroll);

    // Panel background (full height, right edge)
    SDL_Rect panelBg{ 0, 0, w, winH };
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, kBgColor.r, kBgColor.g, kBgColor.b, kBgColor.a);
    SDL_RenderFillRect(renderer_, &panelBg);

    // Header
    SDL_Rect headerBg{ 0, 0, w, kHeaderH };
    SDL_SetRenderDrawColor(renderer_, kHeaderBg.r, kHeaderBg.g, kHeaderBg.b, kHeaderBg.a);
    SDL_RenderFillRect(renderer_, &headerBg);

    // Separator line below header
    SDL_SetRenderDrawColor(renderer_, kSeparator.r, kSeparator.g, kSeparator.b, kSeparator.a);
    SDL_RenderDrawLine(renderer_, 0, kHeaderH - 1, w, kHeaderH - 1);

    // Header text: "播放列表 (N)"
    std::string headerText = "播放列表";
    if (playlist_ && playlist_->size() > 0) {
        headerText += " (" + std::to_string(playlist_->size()) + ")";
    }
    textCache_.drawText(12, 8, headerText, 13, 200, 200, 200);

    // Clip to items area
    SDL_Rect clipRect{ 0, itemsY, w, visibleH };
    SDL_RenderSetClipRect(renderer_, &clipRect);

    // Draw items
    for (int i = 0; i < totalItems; ++i) {
        int y = itemsY + i * kItemH - scrollOffset_;
        if (y + kItemH < itemsY - kItemH || y > itemsY + visibleH + kItemH) continue;

        bool isActive = (i == currentIndex);
        bool isHover = (i == hoverIndex_);
        std::string filename = std::filesystem::path(playlist_->fileAt(i)).filename().string();
        drawItem(y, i, filename, isActive, isHover, winH);
    }

    SDL_RenderSetClipRect(renderer_, nullptr);

    // Scrollbar
    if (contentH > visibleH) {
        int sbH = std::max(30, (int)((float)visibleH / contentH * visibleH));
        int sbY = itemsY + (int)((float)scrollOffset_ / contentH * visibleH);
        SDL_Rect sb{ w - 3, sbY, 2, sbH };
        SDL_SetRenderDrawColor(renderer_, 80, 80, 80, 120);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(renderer_, &sb);
    }

    // Toggle button (right edge vertical strip)
    SDL_Rect toggleBtn{ w - kEdgeW, 0, kEdgeW, winH };
    SDL_SetRenderDrawColor(renderer_,
        toggleHover_ ? 80 : 40, toggleHover_ ? 80 : 40,
        toggleHover_ ? 80 : 40, toggleHover_ ? 120 : 60);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRect(renderer_, &toggleBtn);

    // Resize handle (left edge)
    if (open_) {
        SDL_Rect resizeHandle{ 0, 0, kResizeW, winH };
        SDL_SetRenderDrawColor(renderer_, 60, 60, 60, 0);
        SDL_RenderFillRect(renderer_, &resizeHandle);
    }
}

void PlaylistPanel::drawItem(int y, int index, const std::string& filename,
                              bool isActive, bool isHover, int winH) {
    int w = baseWidth_;
    SDL_Rect itemBg{ 0, y, w, kItemH };

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
    int iconX = 8;
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
    int textX = 36;
    std::string numStr = std::to_string(index + 1) + ". ";
    int cr = isActive ? 77 : 180;
    int cg = isActive ? 144 : 180;
    int cb = isActive ? 255 : 180;
    textCache_.drawText(textX, y + 9, numStr, 11, cr, cg, cb);

    // Filename (截取显示区域内的部分)
    int nameX = textX + 30;
    int nameW = w - nameX - 8;
    if (nameW > 0) {
        textCache_.drawText(nameX, y + 9, filename, 11,
                           isActive ? 77 : 200,
                           isActive ? 144 : 200,
                           isActive ? 255 : 200);
    }
}

bool PlaylistPanel::handleMouseMove(int mx, int my, int winH) {
    int w = width();
    if (w == 0) return false;

    // Toggle button hover
    toggleHover_ = (mx >= w - kEdgeW && mx < w && my >= 0 && my < winH);

    // Resize handle
    if (open_ && mx >= 0 && mx < kResizeW && my >= 0 && my < winH) {
        SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE));
        if (resizing_) {
            int delta = resizeStartX_ - mx;
            baseWidth_ = std::max(minWidth_, std::min(maxWidth_, resizeStartW_ + delta));
        }
        return true;
    } else if (!resizing_) {
        SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
    }

    // Item hover
    if (mx >= 0 && mx < w && my >= kHeaderH) {
        int idx = (my - kHeaderH + scrollOffset_) / kItemH;
        if (playlist_ && idx >= 0 && idx < (int)playlist_->size()) {
            hoverIndex_ = idx;
        } else {
            hoverIndex_ = -1;
        }
    } else {
        hoverIndex_ = -1;
    }

    return mx < w;
}

bool PlaylistPanel::handleMouseDown(int mx, int my, int winH) {
    int w = width();
    if (w == 0) return false;

    // Toggle button click
    if (mx >= w - kEdgeW && mx < w && my >= 0 && my < winH) {
        toggle();
        return true;
    }

    // Resize handle
    if (open_ && mx >= 0 && mx < kResizeW && my >= 0 && my < winH) {
        resizing_ = true;
        resizeStartX_ = mx;
        resizeStartW_ = baseWidth_;
        return true;
    }

    // Item click
    if (mx >= 0 && mx < w && my >= kHeaderH) {
        int idx = (my - kHeaderH + scrollOffset_) / kItemH;
        if (playlist_ && idx >= 0 && idx < (int)playlist_->size()) {
            clickedIdx_ = idx;
        }
        return true;
    }

    return mx < w;
}

bool PlaylistPanel::handleMouseUp(int mx, int my) {
    (void)mx; (void)my;
    if (resizing_) {
        resizing_ = false;
        return true;
    }
    return false;
}

bool PlaylistPanel::handleMouseWheel(int dy, int winH) {
    int w = width();
    if (w == 0) return false;
    if (!playlist_) return false;

    int itemsY = kHeaderH;
    int visibleH = winH - itemsY;
    int totalItems = (int)playlist_->size();
    int contentH = totalItems * kItemH;
    int maxScroll = std::max(0, contentH - visibleH);

    scrollOffset_ -= dy * 20;
    scrollOffset_ = std::max(0, std::min(maxScroll, scrollOffset_));
    return true;
}
