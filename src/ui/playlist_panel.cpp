#include "ui/playlist_panel.h"
#include "core/playlist.h"
#include "core/thumbnail_extractor.h"
#include "ui/svgicon.h"
#include "core/logger.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <windows.h>

// M32f: 卡片化尺寸（对照效果图 .pl-*）
static const int kItemH = 70;      // 缩略图56 + 上下留白
static const int kHeaderH = 44;
static const int kEdgeW = 8;
static const int kResizeW = 0;     // 效果图无拖拽调宽，固定 320

// 本地圆角矩形（顶点扇形，与 video_renderer 同思路）
static void fillRR(SDL_Renderer* r, int x, int y, int w, int h, int rad,
                   Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (w <= 0 || h <= 0 || ca == 0) return;
    rad = std::min(rad, std::min(w, h) / 2);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_Color c{ cr, cg, cb, ca };
    std::vector<SDL_Vertex> v;
    SDL_Rect body{ x + rad, y, w - rad * 2, h };
    auto quad = [&](float ax, float ay, float bx, float by) {
        v.push_back({ {ax,ay}, c,{} }); v.push_back({ {bx,ay}, c,{} });
        v.push_back({ {bx,by}, c,{} }); v.push_back({ {ax,ay}, c,{} });
        v.push_back({ {bx,by}, c,{} }); v.push_back({ {ax,by}, c,{} });
    };
    quad((float)body.x, (float)y, (float)(body.x + body.w), (float)(y + h));
    const int STEPS = 6;
    auto corner = [&](int cx, int cy, int sx, int sy, float a0) {
        for (int i = 0; i < STEPS; ++i) {
            float t0 = a0 + i * 1.5707963f / STEPS, t1 = a0 + (i + 1) * 1.5707963f / STEPS;
            SDL_Vertex center{ { (float)cx,(float)cy }, c,{} };
            SDL_Vertex p0{ { cx + sx * rad * std::cos(t0), cy + sy * rad * std::sin(t0) }, c,{} };
            SDL_Vertex p1{ { cx + sx * rad * std::cos(t1), cy + sy * rad * std::sin(t1) }, c,{} };
            v.push_back(center); v.push_back(p0); v.push_back(p1);
        }
    };
    corner(x + rad, y + rad, -1, -1, 3.14159265f);
    corner(x + w - rad, y + rad, 1, -1, 4.71238898f);
    corner(x + w - rad, y + h - rad, 1, 1, 0.f);
    corner(x + rad, y + h - rad, -1, 1, 1.5707963f);
    SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), nullptr, 0);
}
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
    stopWorker();
    clearThumbnailCache();
    for (auto& [k, v] : iconCache_) {
        if (v.tex) SDL_DestroyTexture(v.tex);
    }
    iconCache_.clear();
    textCache_.shutdown();
}

void PlaylistPanel::toggle() {
    bool wasOpen = open_;
    open_ = !open_;
    if (!open_) {
        resizing_ = false;
        shrinkPending_ = true;
        stopWorker();
    } else {
        lastScrollTick_ = SDL_GetTicks();  // 打开时立即显示滚动条
        scrollbarAlpha_ = 0.0f;
    }
    if (wasOpen) LOG_INFO("UI", "playlist closing anim -> window shrink pending");
}

bool PlaylistPanel::shrinkReady() const {
    return shrinkPending_ && !open_;
}

void PlaylistPanel::consumeShrink() {
    shrinkPending_ = false;
}

bool PlaylistPanel::consumeToggleRequest() {
    if (!toggleRequested_) return false;
    toggleRequested_ = false;
    return true;
}

int PlaylistPanel::width() const {
    return (open_ || shrinkPending_) ? baseWidth_ : 0;
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

// M33: 后台缩略图提取
static double thumbSeekTime(const std::string& path) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
        return 2.0;
    double dur = 0;
    if (fmt->duration != AV_NOPTS_VALUE)
        dur = (double)fmt->duration / AV_TIME_BASE;
    avformat_close_input(&fmt);
    if (dur <= 0) return 2.0;
    return dur * 0.10;
}

void PlaylistPanel::startWorker() {
    if (worker_.running.load()) return;
    worker_.running.store(true);
    worker_.cancelled.store(false);
    worker_.nextIdx.store(0);
    worker_.thread = std::thread(&PlaylistPanel::workerFunc, this);
}

void PlaylistPanel::stopWorker() {
    if (!worker_.running.load()) return;
    worker_.running.store(false);
    worker_.cancelled.store(true);
    if (worker_.thread.joinable()) worker_.thread.join();
}

void PlaylistPanel::workerFunc() {
    ThumbnailExtractor extractor;
    std::string lastPath;

    while (worker_.running.load()) {
        if (worker_.cancelled.load()) {
            extractor.close();
            lastPath.clear();
            worker_.cancelled.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int cur = worker_.nextIdx.load();
        std::vector<std::string> paths;
        std::vector<int> indices;
        std::unordered_set<int> cached;
        {
            std::lock_guard<std::mutex> lock(worker_.mutex);
            paths = worker_.paths;
            indices = worker_.indices;
            cached = worker_.cachedIndices;
        }

        if (cur < 0 || cur >= (int)paths.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        int listIdx = (cur < (int)indices.size()) ? indices[cur] : cur;

        // 跳过已缓存项
        if (cached.count(listIdx)) {
            worker_.nextIdx.store(cur + 1);
            continue;
        }

        const std::string& path = paths[cur];
        if (path.empty()) {
            worker_.nextIdx.store(cur + 1);
            continue;
        }

        if (worker_.cancelled.load()) continue;

        if (path != lastPath) {
            extractor.close();
            if (!extractor.open(path)) {
                LOG_WARN("THUMB", "worker: open fail idx=%d path=%s", cur, path.c_str());
                worker_.nextIdx.store(cur + 1);
                lastPath = path;
                continue;
            }
            lastPath = path;
        }

        if (worker_.cancelled.load()) continue;

        double seekSec = thumbSeekTime(path);
        uint8_t* pixels = nullptr;
        int w = 0, h = 0;
        if (extractor.getFrame(seekSec, &pixels, w, h) && pixels) {
            std::lock_guard<std::mutex> lock(worker_.mutex);
            if (worker_.pendingPixels) av_free(worker_.pendingPixels);
            worker_.pendingPixels = pixels;
            worker_.pendingW = w;
            worker_.pendingH = h;
            worker_.pendingTargetIdx = listIdx;
            worker_.ready = true;
        }
        worker_.nextIdx.store(cur + 1);
    }
    extractor.close();
}

void PlaylistPanel::requestVisibleRange(const std::vector<std::string>& paths,
                                        const std::vector<int>& indices) {
    worker_.cancelled.store(true);
    {
        std::lock_guard<std::mutex> lock(worker_.mutex);
        worker_.paths = paths;
        worker_.indices = indices;
    }
    worker_.nextIdx.store(0);
    worker_.cancelled.store(false);
}

void PlaylistPanel::consumeReadyTexture() {
    std::lock_guard<std::mutex> lock(worker_.mutex);
    if (!worker_.ready || !worker_.pendingPixels) return;

    int targetIdx = worker_.pendingTargetIdx;
    if (targetIdx >= 0 && thumbTextures_.count(targetIdx) == 0) {
        SDL_Texture* tex = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGB24,
            SDL_TEXTUREACCESS_STREAMING,
            worker_.pendingW, worker_.pendingH);
        if (tex) {
            void* tbits = nullptr;
            int pitch = 0;
            if (SDL_LockTexture(tex, nullptr, &tbits, &pitch) == 0) {
                for (int row = 0; row < worker_.pendingH; ++row)
                    memcpy((Uint8*)tbits + row * pitch,
                           worker_.pendingPixels + row * worker_.pendingW * 3,
                           worker_.pendingW * 3);
                SDL_UnlockTexture(tex);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                thumbTextures_[targetIdx] = tex;
                worker_.cachedIndices.insert(targetIdx);
                thumbAccess_[targetIdx] = SDL_GetTicks();
            } else {
                SDL_DestroyTexture(tex);
            }
        }
    }
    av_free(worker_.pendingPixels);
    worker_.pendingPixels = nullptr;
    worker_.ready = false;
}

void PlaylistPanel::clearThumbnailCache() {
    for (auto& [k, v] : thumbTextures_)
        if (v) SDL_DestroyTexture(v);
    thumbTextures_.clear();
    thumbAccess_.clear();
    {
        std::lock_guard<std::mutex> lock(worker_.mutex);
        worker_.cachedIndices.clear();
    }
    worker_.nextIdx.store(0);
}

void PlaylistPanel::evictOldThumbnails() {
    uint32_t now = SDL_GetTicks();
    std::vector<int> toEvict;
    for (auto& [idx, tex] : thumbTextures_) {
        auto it = thumbAccess_.find(idx);
        uint32_t lastAccess = (it != thumbAccess_.end()) ? it->second : 0;
        if (now - lastAccess > 30000) {  // 30 秒未访问
            toEvict.push_back(idx);
        }
    }
    for (int idx : toEvict) {
        auto t = thumbTextures_.find(idx);
        if (t != thumbTextures_.end()) {
            if (t->second) SDL_DestroyTexture(t->second);
            thumbTextures_.erase(t);
        }
        thumbAccess_.erase(idx);
        {
            std::lock_guard<std::mutex> lock(worker_.mutex);
            worker_.cachedIndices.erase(idx);
        }
    }
}

void PlaylistPanel::draw(int currentIndex, int winW, int winH) {
    // M32f.6: 全高面板（无底部空白）；动画更新
    // M32g: 无动画 —— 立即出现/消失
    openAnim_ = open_ ? 1.0f : 0.0f;

    closeHover_ = false;

    // M32g: 右缘条开关已移除（顶栏 ☰ 即列表开关）

    if (openAnim_ < 0.01f) return;

    consumeReadyTexture();
    evictOldThumbnails();

    int pw = (int)(baseWidth_ * openAnim_);
    // M32f.9: 收起动画时整体向右滑出（窗口尚未缩回，超出部分被窗口裁剪）
    int panelX = winW - pw;
    int panelTop = 0;
    int panelH = winH;
    int itemsY = panelTop + kHeaderH;
    int visibleH = panelH - kHeaderH;
    int totalItems = playlist_ ? (int)playlist_->size() : 0;
    int contentH = totalItems * kItemH;
    int maxScroll = std::max(0, contentH - visibleH);
    scrollOffset_ = std::min(scrollOffset_, maxScroll);

    // 面板背景
    SDL_Rect panelBg{ panelX, panelTop, pw, panelH };
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, kBgColor.r, kBgColor.g, kBgColor.b, kBgColor.a);
    SDL_RenderFillRect(renderer_, &panelBg);

    // 左边缘分隔线
    SDL_SetRenderDrawColor(renderer_, 55, 55, 55, 255);
    SDL_RenderDrawLine(renderer_, panelX, panelTop, panelX, winH);

    // ---- 列表区（先画条目，最后画头部保证头部在最上层）----
    // Clip to items area
    listClip_ = SDL_Rect{ panelX, itemsY, pw, visibleH };
    SDL_RenderSetClipRect(renderer_, &listClip_);

    for (int i = 0; i < totalItems; ++i) {
        int y = itemsY + i * kItemH - scrollOffset_;
        if (y + kItemH < itemsY - kItemH || y > itemsY + visibleH + kItemH) continue;
        bool isActive = (i == currentIndex);
        bool isHover = (i == hoverIndex_);
        std::string filename = std::filesystem::path(playlist_->fileAt(i)).filename().string();
        drawItem(panelX, y, i, filename, isActive, isHover, pw);
    }
    // M33: 请求可见区域缩略图
    if (playlist_ && playlist_->size() > 0) {
        int visStart = std::max(0, -scrollOffset_ / kItemH);
        int visEnd = std::min(totalItems, (visibleH + scrollOffset_) / kItemH + 1);
        // 上下各预留 4 个缓冲项（滚动时已有缩略图）
        const int kBuf = 4;
        int rangeStart = std::max(0, visStart - kBuf);
        int rangeEnd = std::min(totalItems, visEnd + kBuf);
        if (rangeStart < rangeEnd) {
            startWorker();
            std::vector<std::string> visPaths;
            std::vector<int> visIndices;
            for (int i = rangeStart; i < rangeEnd; ++i) {
                visPaths.push_back(playlist_->fileAt(i));
                visIndices.push_back(i);
            }
            requestVisibleRange(visPaths, visIndices);
        }
    }
    SDL_RenderSetClipRect(renderer_, nullptr);

    // ---- Header（最后绘制，永远盖在条目之上）----
    SDL_Rect headerBg{ panelX, panelTop, pw, kHeaderH };
    SDL_SetRenderDrawColor(renderer_, kHeaderBg.r, kHeaderBg.g, kHeaderBg.b, kHeaderBg.a);
    SDL_RenderFillRect(renderer_, &headerBg);
    SDL_SetRenderDrawColor(renderer_, kSeparator.r, kSeparator.g, kSeparator.b, kSeparator.a);
    SDL_RenderDrawLine(renderer_, panelX, panelTop + kHeaderH - 1, panelX + pw, panelTop + kHeaderH - 1);

    std::string headerText = "播放列表";
    if (playlist_ && playlist_->size() > 0)
        headerText += " (" + std::to_string(playlist_->size()) + ")";
    textCache_.drawText(panelX + 14, panelTop + 13, headerText, 13, 255, 255, 255);
    {
        int bx = panelX + pw - 14 - 28, by = panelTop + 8;
        closeRect_ = SDL_Rect{ bx, by, 28, 28 };
        bool closeHover = (mx_ >= bx && mx_ < bx + 28 && my_ >= by && my_ < by + 28);
        if (closeHover)
            fillRR(renderer_, bx, by, 28, 28, 7, 255, 255, 255, 20);
        svgicon::draw(renderer_, "close", bx + 14, by + 14, 16,
                      closeHover ? 255 : 212, closeHover ? 255 : 212,
                      closeHover ? 255 : 216, 220);
    }

    // 滚动条（头部之后绘制，确保在最上层）
    scrollbarRect_ = { 0, 0, 0, 0 };
    if (contentH > visibleH && totalItems > 0) {
        bool nearEdge = (mx_ >= panelX + pw - 40);
        float targetAlpha = (nearEdge || scrollbarDragging_ ||
                            SDL_GetTicks() - lastScrollTick_ < 2000) ? 1.0f : 0.0f;
        if (scrollbarAlpha_ < targetAlpha)
            scrollbarAlpha_ = std::min(1.0f, scrollbarAlpha_ + 0.10f);
        else if (scrollbarAlpha_ > targetAlpha)
            scrollbarAlpha_ = std::max(0.0f, scrollbarAlpha_ - 0.04f);

        if (scrollbarAlpha_ > 0.01f) {
            int sbH = std::max(30, (int)((float)visibleH / contentH * visibleH));
            int sbY = itemsY + (int)((float)scrollOffset_ / contentH * visibleH);
            int sbW = 6;
            Uint8 alpha = (Uint8)(200 * scrollbarAlpha_);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, alpha);
            SDL_Rect sbRect{ panelX + pw - sbW - 6, sbY, sbW, sbH };
            SDL_RenderFillRect(renderer_, &sbRect);
            scrollbarRect_ = sbRect;
        }
    }

    // 拖拽调整宽度手柄（面板左边缘）—— kResizeW=0 时不可见
    if (open_) {
        SDL_Rect resizeHandle{ panelX, panelTop, kResizeW, panelH };
        SDL_SetRenderDrawColor(renderer_, 70, 70, 70, 180);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(renderer_, &resizeHandle);
    }
}

void PlaylistPanel::drawItem(int baseX, int y, int index, const std::string& filename,
                              bool isActive, bool isHover, int panelW) {
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    // M32f: 卡片容器（.pl-item padding7 r9）
    const int pad = 7;
    if (isActive)
        fillRR(renderer_, baseX + 2, y, panelW - 4, kItemH - 2, 9, 37, 99, 235, 46);
    else if (isHover)
        fillRR(renderer_, baseX + 2, y, panelW - 4, kItemH - 2, 9, 255, 255, 255, 15);

    // 缩略图（100×56）：有缓存时显示真实帧，否则显示占位图
    const int thW = 100, thH = 56;
    int tx = baseX + pad, ty = y + (kItemH - 2 - thH) / 2;
    SDL_Rect clip{ tx, ty, thW, thH };
    SDL_RenderSetClipRect(renderer_, &clip);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    auto thumbIt = thumbTextures_.find(index);
    if (thumbIt != thumbTextures_.end() && thumbIt->second) {
        SDL_Rect dst{ tx, ty, thW, thH };
        SDL_RenderCopy(renderer_, thumbIt->second, nullptr, &dst);
        thumbAccess_[index] = SDL_GetTicks();  // LRU: 更新访问时间
    } else {
        SDL_SetRenderDrawColor(renderer_, 0x3c, 0x3c, 0x48, 255);
        SDL_Rect topHalf{ tx, ty, thW, thH / 2 };
        SDL_RenderFillRect(renderer_, &topHalf);
        SDL_SetRenderDrawColor(renderer_, 0x2a, 0x2a, 0x34, 255);
        SDL_Rect botHalf{ tx, ty + thH / 2, thW, thH - thH / 2 };
        SDL_RenderFillRect(renderer_, &botHalf);
        svgicon::draw(renderer_, "play", tx + thW / 2 + 1, ty + thH / 2, 22,
                      255, 255, 255, isActive ? 170 : 110);
    }
    SDL_RenderSetClipRect(renderer_, &listClip_);

    // 元数据区
    int mx = tx + thW + 10;
    int mw = baseX + panelW - pad * 2 - (mx - baseX);
    std::string ext;
    size_t dot = filename.find_last_of('.');
    if (dot != std::string::npos) ext = filename.substr(dot + 1);
    for (auto& ch : ext) ch = (char)::toupper((unsigned char)ch);

    textCache_.drawText(mx, y + 8, filename.substr(0, dot == std::string::npos ? filename.size() : dot), 12,
                        isActive ? 0xbf : 255, isActive ? 0xd6 : 255, isActive ? 0xff : 255);
    textCache_.drawText(mx, y + 28, std::string("视频 · ") + (ext.empty() ? "VIDEO" : ext), 11,
                        0xa1, 0xa1, 0xa6);
    if (isActive) textCache_.drawText(mx, y + 47, "正在播放", 10, 0x3b, 0x82, 0xf6);
    else          textCache_.drawText(mx, y + 47, "未播放", 10, 0x3f, 0x3f, 0x46);
}

bool PlaylistPanel::handleMouseMove(int mx, int my, int winW, int winH) {
    mx_ = mx; my_ = my;
    const int panelTop = 0;
    const int panelBot = winH;

    if (!open_) {
        toggleHover_ = false;
        scrollbarDragging_ = false;
        return false;
    }

    int w = width();
    if (w == 0) return false;
    int panelX = winW - w;

    // 滚动条拖动中
    if (scrollbarDragging_ && scrollbarRect_.h > 0) {
        int totalItems = playlist_ ? (int)playlist_->size() : 0;
        if (totalItems > 0) {
            int visibleH = panelBot - panelTop - kHeaderH;
            int contentH = totalItems * kItemH;
            int maxScroll = std::max(0, contentH - visibleH);
            float trackH = (float)(visibleH - scrollbarRect_.h);
            if (trackH > 0) {
                float ratio = (float)(my - scrollbarRect_.y - scrollbarDragOffset_) / trackH;
                if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
                scrollOffset_ = (int)(ratio * maxScroll);
            }
        }
        lastScrollTick_ = SDL_GetTicks();
        SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));
        return true;
    }

    // 滚动条 hover → 手型光标
    if (scrollbarRect_.w > 0 &&
        mx >= scrollbarRect_.x - 4 && mx < scrollbarRect_.x + scrollbarRect_.w + 4 &&
        my >= scrollbarRect_.y - 4 && my < scrollbarRect_.y + scrollbarRect_.h + 4) {
        SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));
        return true;
    }

    // 关闭按钮 hover
    if (open_ && mx >= closeRect_.x - 4 && mx < closeRect_.x + closeRect_.w + 4 &&
        my >= closeRect_.y - 4 && my < closeRect_.y + closeRect_.h + 4) {
        closeHover_ = true;
        SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));
        return true;
    } else {
        closeHover_ = false;
    }

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
    const int panelTop = 0;
    const int panelBot = winH;

    int w = width();

    if (w > 0) {
        int panelX = winW - w;
        // 关闭钮
        if (open_ &&
            mx >= closeRect_.x - 6 && mx < closeRect_.x + closeRect_.w + 6 &&
            my >= closeRect_.y - 6 && my < closeRect_.y + closeRect_.h + 6) {
            toggleRequested_ = true;
            LOG_INFO("UI", "playlist close button pressed -> toggle request");
            return true;
        }
        // 拖拽调整宽度
        if (open_ && mx >= panelX && mx < panelX + kResizeW && my >= panelTop && my < panelBot) {
            resizing_ = true;
            resizeStartX_ = mx;
            resizeStartW_ = baseWidth_;
            return true;
        }
        // 滚动条拖动
        if (open_ && scrollbarRect_.w > 0 &&
            mx >= scrollbarRect_.x - 4 && mx < scrollbarRect_.x + scrollbarRect_.w + 4 &&
            my >= scrollbarRect_.y - 4 && my < scrollbarRect_.y + scrollbarRect_.h + 4) {
            scrollbarDragging_ = true;
            scrollbarDragOffset_ = my - scrollbarRect_.y;
            lastScrollTick_ = SDL_GetTicks();
            return true;
        }
        // 滚动条轨道点击 → 跳页
        if (open_ && scrollbarRect_.w > 0 &&
            mx >= scrollbarRect_.x - 10 && mx < scrollbarRect_.x + scrollbarRect_.w + 10 &&
            my >= panelTop + kHeaderH && my < panelBot) {
            if (playlist_ && playlist_->size() > 0) {
                int totalItems = (int)playlist_->size();
                int visibleH = panelBot - panelTop - kHeaderH;
                int contentH = totalItems * kItemH;
                float ratio = (float)(my - panelTop - kHeaderH) / visibleH;
                scrollOffset_ = (int)(ratio * contentH) - visibleH / 2;
                scrollOffset_ = std::max(0, std::min(std::max(0, contentH - visibleH), scrollOffset_));
                lastScrollTick_ = SDL_GetTicks();
            }
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
        if (mx >= panelX && my >= panelTop && my < panelBot) return true;
    }

    return false;
}

bool PlaylistPanel::handleMouseUp(int mx, int my) {
    (void)mx; (void)my;
    if (scrollbarDragging_) {
        scrollbarDragging_ = false;
        return true;
    }
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

    const int panelTop = 0;   // M32f.6: 全高面板，无底部空白
    const int panelBot = winH;
    int visibleH = panelBot - panelTop - kHeaderH;
    int totalItems = (int)playlist_->size();
    int contentH = totalItems * kItemH;
    int maxScroll = std::max(0, contentH - visibleH);

    scrollOffset_ -= dy * 20;
    scrollOffset_ = std::max(0, std::min(maxScroll, scrollOffset_));
    lastScrollTick_ = SDL_GetTicks();
    return true;
}
