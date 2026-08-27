#include "ui/gdi_text.h"
#include <windows.h>
#include <cstring>
#include <vector>

void GdiTextCache::init(SDL_Renderer* renderer) {
    renderer_ = renderer;
}

void GdiTextCache::shutdown() {
    for (auto& kv : cache_) {
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    }
    cache_.clear();
}

SDL_Texture* GdiTextCache::renderText(const std::string& utf8, int ptSize,
                                       int r, int g, int b, int& outW, int& outH) {
    if (!renderer_ || utf8.empty()) return nullptr;

    // UTF-8 → UTF-16
    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (wLen <= 0) return nullptr;
    std::vector<wchar_t> wbuf(wLen + 1);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wbuf.data(), wLen);
    wbuf[wLen] = 0;

    HDC hdc = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(hdc);
    HFONT hFont = CreateFontW(
        -MulDiv(ptSize, GetDeviceCaps(hdc, LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(memDC, hFont);

    RECT rc{ 0, 0, 0, 0 };
    DrawTextW(memDC, wbuf.data(), -1, &rc, DT_CALCRECT | DT_LEFT | DT_SINGLELINE);
    outW = rc.right;
    outH = rc.bottom;
    if (outW <= 0 || outH <= 0) {
        SelectObject(memDC, oldFont);
        DeleteObject(hFont);
        DeleteDC(memDC);
        ReleaseDC(nullptr, hdc);
        return nullptr;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = outW;
    bi.bmiHeader.biHeight = -outH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(memDC, hBmp);

    SetBkMode(memDC, TRANSPARENT);
    // 恒用白色渲染: 亮度=覆盖率, 输出时再替换为请求色 (luma-alpha 标准技法)
    SetTextColor(memDC, RGB(255, 255, 255));
    DrawTextW(memDC, wbuf.data(), -1, &rc, DT_LEFT | DT_SINGLELINE);

    // 转 SDL_Texture
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
        0, outW, outH, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surf) {
        SDL_LockSurface(surf);
        Uint32* dst = (Uint32*)surf->pixels;
        Uint32* src = (Uint32*)bits;
        // 亮度即 alpha, 颜色统一为请求色 — 抗锯齿平滑, 无二值描边锯齿
        Uint8 cr = (Uint8)r, cg = (Uint8)g, cb = (Uint8)b;
        for (int y = 0; y < outH; ++y) {
            for (int x = 0; x < outW; ++x) {
                Uint32 px = src[y * outW + x];
                Uint8 sr = (px >> 16) & 0xFF;
                Uint8 sg = (px >> 8) & 0xFF;
                Uint8 sb = px & 0xFF;
                Uint8 a = sr; if (sg > a) a = sg; if (sb > a) a = sb;
                dst[y * surf->w + x] = ((Uint32)a << 24) | ((Uint32)cr << 16) |
                                       ((Uint32)cg << 8) | cb;
            }
        }
        SDL_UnlockSurface(surf);
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(hBmp);
    SelectObject(memDC, oldFont);
    DeleteObject(hFont);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdc);

    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    SDL_FreeSurface(surf);
    return tex;
}

int GdiTextCache::measureText(const std::string& utf8, int ptSize) {
    if (utf8.empty()) return 0;
    // Toast 等低频调用: 直接测量, 不走纹理缓存
    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (wLen <= 0) return 0;
    std::vector<wchar_t> wbuf(wLen + 1);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wbuf.data(), wLen);
    wbuf[wLen] = 0;

    HDC hdc = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(hdc);
    HFONT hFont = CreateFontW(
        -MulDiv(ptSize, GetDeviceCaps(hdc, LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(memDC, hFont);
    RECT rc{ 0, 0, 0, 0 };
    DrawTextW(memDC, wbuf.data(), -1, &rc, DT_CALCRECT | DT_LEFT | DT_SINGLELINE);
    int w = rc.right;
    SelectObject(memDC, oldFont);
    DeleteObject(hFont);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdc);
    return w;
}

void GdiTextCache::drawText(int x, int y, const std::string& utf8, int ptSize,
                             int r, int g, int b, int a) {
    // 查缓存（哈希表 O(1)；旧实现 vector 线性扫描每帧数千次字符串比较）
    std::string key = utf8 + "|" + std::to_string(ptSize) + "|" +
                      std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        SDL_Rect dst{ x, y, it->second.w, it->second.h };
        if (a < 255) SDL_SetTextureAlphaMod(it->second.tex, (Uint8)a);
        SDL_RenderCopy(renderer_, it->second.tex, nullptr, &dst);
        if (a < 255) SDL_SetTextureAlphaMod(it->second.tex, 255);   // 复位, 缓存共享
        return;
    }

    // 渲染新文本
    int w = 0, h = 0;
    SDL_Texture* tex = renderText(utf8, ptSize, r, g, b, w, h);
    if (!tex) return;

    cache_[key] = { tex, w, h };
    // 超限：仅保留刚插入的条目，其余整体释放（UI 实际条目远低于阈值，极少触发）
    if (cache_.size() > 400) {
        CacheEntry keep = cache_[key];
        for (auto& kv : cache_)
            if (kv.second.tex != keep.tex) SDL_DestroyTexture(kv.second.tex);
        cache_.clear();
        cache_[key] = keep;
    }

    SDL_Rect dst{ x, y, w, h };
    if (a < 255) SDL_SetTextureAlphaMod(tex, (Uint8)a);
    SDL_RenderCopy(renderer_, tex, nullptr, &dst);
    if (a < 255) SDL_SetTextureAlphaMod(tex, 255);
}
