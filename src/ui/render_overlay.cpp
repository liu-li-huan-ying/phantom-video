#include "ui/render_overlay.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_syswm.h>

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

#include "app/app_state.h"
#include "ui/helpers.h"
#include "ui/theme.h"
#include "ui/primitives.h"
#include "ui/gradient.h"
#include "ui/ulw.h"
#include "ui/svgicon.h"
#include "ui/gdi_text.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/mpv_backend.h"
#include "core/thumbnail_extractor.h"

// ================================================================
// Thumb infrastructure (moved from main.cpp)
// ================================================================

std::mutex g_thumbMtx;
std::vector<std::string> g_thumbWant;                   // ��ǰ�ɼ�����ȡ����
std::map<std::string, ThumbRgb> g_thumbRgb;              // path -> RGB24(�� px=ʧ�ܱ��)
std::map<std::string, SDL_Texture*> g_thumbTex;
std::map<std::string, Uint32>       g_thumbAccess;          // ��Ⱦ�߳�ר��
std::atomic<bool> g_thumbQuit{false};
std::thread g_thumbThread;

// ---- ����ͼ���̻��棺exe/cache/thumbs/<fnv1a64>.bin = "VPT1"+w+h+RGB24 ----
std::string thumbCacheDir() {
    return exeDir() + "cache\\thumbs";
}

// ����ʱ�������� keepDays ��Ļ����ļ�
void thumbCacheCleanup(int keepDays) {
    if (keepDays <= 0) keepDays = 7;
    std::string dir = thumbCacheDir();
    WIN32_FIND_DATAA fd;
    std::string pattern = dir + "\\*.bin";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ulNow = {{now.dwLowDateTime, now.dwHighDateTime}};

    // �ռ������ļ�
    struct ThumbFile { std::string name; ULARGE_INTEGER mtime; };
    std::vector<ThumbFile> files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER ulFile = {{fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}};
        files.push_back({fd.cFileName, ulFile});
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    int removed = 0;
    // 1) ɾ�������ļ�
    for (auto& f : files) {
        long long ageDays = (long long)(ulNow.QuadPart - f.mtime.QuadPart) / (10000000LL * 86400);
        if (ageDays > keepDays) {
            DeleteFileA((dir + "\\" + f.name).c_str());
            f.name.clear();  // �����ɾ
            ++removed;
        }
    }
    // 2) �������� 300: ��ʱ��������̭��ɵ�
    static const int kMaxThumbs = 300;
    files.erase(std::remove_if(files.begin(), files.end(),
        [](const ThumbFile& f) { return f.name.empty(); }), files.end());
    if ((int)files.size() > kMaxThumbs) {
        std::sort(files.begin(), files.end(),
            [](const ThumbFile& a, const ThumbFile& b) {
                return a.mtime.QuadPart < b.mtime.QuadPart;
            });
        int excess = (int)files.size() - kMaxThumbs;
        for (int i = 0; i < excess; ++i) {
            DeleteFileA((dir + "\\" + files[i].name).c_str());
            ++removed;
        }
    }
    if (removed) LOG_INFO("MAIN", "thumb cache cleanup: removed %d files (%zu remaining)",
                          removed, files.size() - (files.size() > 0 ? 0 : 0));
}

static uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

static std::string thumbDiskPath(const std::string& path) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx.bin", (unsigned long long)fnv1a64(path));
    return thumbCacheDir() + "\\" + buf;
}

// ���з��� true ����� out���ļ�����ɾ��
static bool thumbDiskLoad(const std::string& path, ThumbRgb& out) {
    FILE* f = fopen(thumbDiskPath(path).c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    int w = 0, h = 0;
    bool ok = false;
    do {
        if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "VPT1", 4) != 0) break;
        if (fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1) break;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) break;
        size_t need = (size_t)w * h * 3;
        out.px.resize(need);
        if (fread(out.px.data(), 1, need, f) != need) { out.px.clear(); break; }
        out.w = w; out.h = h;
        ok = true;
    } while (false);
    fclose(f);
    if (!ok) DeleteFileA(thumbDiskPath(path).c_str());   // �𻵼�ɾ
    return ok;
}

static void thumbDiskSave(const std::string& path, const ThumbRgb& t) {
    if (t.px.empty()) return;
    FILE* f = fopen(thumbDiskPath(path).c_str(), "wb");
    if (!f) return;
    fwrite("VPT1", 1, 4, f);
    fwrite(&t.w, 4, 1, f);
    fwrite(&t.h, 4, 1, f);
    fwrite(t.px.data(), 1, t.px.size(), f);
    fclose(f);
}

void thumbWorkerMain() {
    // Run cache cleanup in background (non-blocking startup)
    thumbCacheCleanup(7);

    ThumbnailExtractor ex;
    while (!g_thumbQuit.load()) {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            for (auto& p : g_thumbWant) {
                if (!g_thumbRgb.count(p)) { path = p; break; }
            }
            if (!path.empty())
                g_thumbWant.erase(std::remove(g_thumbWant.begin(), g_thumbWant.end(), path),
                                  g_thumbWant.end());
        }
        if (path.empty()) { Sleep(150); continue; }

        ThumbRgb out;
        bool diskHit = false;
        if (g_cfg.thumbCache) {
            diskHit = thumbDiskLoad(path, out);
            if (diskHit) LOG_DBG("MAIN", "thumb disk hit %s", path.c_str());
        }
        if (!diskHit) {
            uint8_t* px = nullptr; int w = 0, h = 0;
            if (ex.open(path) && ex.getFrame(3.0, &px, w, h) && px && w > 0 && h > 0) {
                out.w = w; out.h = h;
                out.px.assign(px, px + (size_t)w * h * 3);
                ThumbnailExtractor::freePixels(px);
                if (g_cfg.thumbCache) thumbDiskSave(path, out);
                LOG_DBG("MAIN", "thumb ok %dx%d %s", w, h, path.c_str());
            } else {
                LOG_DBG("MAIN", "thumb fail %s", path.c_str());
            }
            ex.close();
        }
        {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            g_thumbRgb[path] = std::move(out);   // ʧ��Ҳ�ǿձ�ǣ����ⷴ������
        }
    }
}

// ��Ⱦ�̵߳��ã��Ѿ����� RGB ת������
static void uploadThumbs(SDL_Renderer* r) {
    std::lock_guard<std::mutex> lk(g_thumbMtx);
    for (auto it = g_thumbRgb.begin(); it != g_thumbRgb.end(); ) {
        auto& t = it->second;
        if (t.px.empty()) { ++it; continue; }                 // ʧ�ܱ������
        if (g_thumbTex.count(it->first)) { it = g_thumbRgb.erase(it); continue; }
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, t.w, t.h, 24, SDL_PIXELFORMAT_RGB24);
        if (surf) {
            SDL_LockSurface(surf);
            for (int y = 0; y < t.h; ++y)
                memcpy((uint8_t*)surf->pixels + y * surf->pitch,
                       t.px.data() + (size_t)y * t.w * 3, (size_t)t.w * 3);
            SDL_UnlockSurface(surf);
            SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
            SDL_FreeSurface(surf);
            if (tex) {
                g_thumbTex[it->first] = tex;
                g_thumbAccess[it->first] = SDL_GetTicks();
                it = g_thumbRgb.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// ================================================================
// Helper functions used only by renderOverlay (moved from main.cpp)
// ================================================================

static std::string mpvStr(const char* prop) {
    if (!g_mpv || !g_mpv->mpv()) return {};
    char* s = mpv_get_property_string(g_mpv->mpv(), prop);
    if (!s) return {};
    std::string r(s);
    mpv_free(s);
    return r;
}

static std::string formatBitrate(const std::string& bpsStr) {
    long long bps = std::atoll(bpsStr.c_str());
    if (bps <= 0) return "";
    char buf[32];
    if (bps >= 1000000) std::snprintf(buf, sizeof(buf), "%.1f Mbps", bps / 1000000.0);
    else                std::snprintf(buf, sizeof(buf), "%d kbps", (int)(bps / 1000));
    return buf;
}


// ================================================================
// Overlay rendering state and helpers (moved from main.cpp)
// ================================================================

void destroyGradCache();   // ������ drawGradientBar�������������棩

UlwCtx g_ulw;

void destroyOverlay() {
    ulwDestroy(g_ulw);
    g_text.shutdown();
    svgicon::shutdown();
    destroyGradCache();
    if (g_sdlRdr) { SDL_DestroyRenderer(g_sdlRdr); g_sdlRdr = nullptr; }
    if (g_sdlWin) { SDL_DestroyWindow(g_sdlWin);   g_sdlWin = nullptr; }  // ��ͬ HWND һ������
    g_overlayHwnd = nullptr;
}

// ---- dithered gradient helper ----
// �����������ػ��ƴ��� ~13 ��� FillRect/֡; ����Ϊ������ÿ֡һ�� RenderCopy��
// ͸����(alpha<��ֵ)д�� 0 ���� ��Ʒ�� colorkey ����, ��Ƶ�ճ���͸��
// P2-2: GradKey/GradCache/drawDitherDim/drawGradientBar ���� ui/gradient.h
GradCache g_gradCache;

void destroyGradCache() {
    g_gradCache.destroy();
}

// ---- rendering ----
// P2-3: UlwCtx/ulwDestroy/ulwResize ���� ui/ulw.h
SDL_Texture* g_ovTex = nullptr;   // UI ��������(�� alpha)
int g_ovTexW = 0, g_ovTexH = 0;

// M36: ��֡Բ�������б� (��Ⱦʱ���, overlayPresent ���Ѻ����)
std::vector<RoundMask> g_roundMasks;

static bool ovTexEnsure(int w, int h) {
    if (g_ovTex && g_ovTexW == w && g_ovTexH == h) return true;
    if (g_ovTex) { SDL_DestroyTexture(g_ovTex); g_ovTex = nullptr; }
    g_ovTex = SDL_CreateTexture(g_sdlRdr, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_TARGET, w, h);
    if (!g_ovTex) {
        LOG_ERROR("MAIN", "ovTex create: %s", SDL_GetError());
        return false;
    }
    g_ovTexW = w; g_ovTexH = h;
    return true;
}

static void overlayPresent() {
    if (!g_sdlRdr || !g_sdlWin || !g_overlayHwnd) return;
    int w = g_ui.totalW > 0 ? g_ui.totalW : g_ui.winW;
    int h = g_ui.winH > 0 ? g_ui.winH : 540;
    if (w <= 0 || h <= 0 || !g_ovTex) return;

    // 1. ������ ARGB �����ض� (�� alpha)
    if (SDL_SetRenderTarget(g_sdlRdr, g_ovTex) != 0) return;
    static std::vector<Uint32> px;
    px.resize((size_t)w * h);
    SDL_Rect rr = {0, 0, w, h};
    if (SDL_RenderReadPixels(g_sdlRdr, &rr, SDL_PIXELFORMAT_ARGB8888,
                             px.data(), w * 4) != 0) {
        LOG_ERROR("MAIN", "RenderReadPixels: %s", SDL_GetError());
        SDL_SetRenderTarget(g_sdlRdr, nullptr);
        return;
    }
    SDL_SetRenderTarget(g_sdlRdr, nullptr);

    // 1.5 M36: Բ������ͼ���� �� ����������ȫ͸��(����Ⱦ�޷��ü�����)
    for (const auto& m : g_roundMasks) {
        int r = std::min(m.r, std::min(m.w, m.h) / 2);
        if (r <= 0) continue;
        auto zeroIfOut = [&](int x, int y, int cx, int cy) {
            if (x < 0 || y < 0 || x >= w || y >= h) return;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r)
                px[(size_t)y * w + x] = 0;
        };
        for (int yy = 0; yy < r; ++yy) {
            int rowT = m.y + yy, rowB = m.y + m.h - 1 - yy;
            if (rowT < 0 || rowT >= h || rowB < 0 || rowB >= h) continue;
            int x0 = std::max(m.x, 0), x1 = std::min(m.x + m.w - 1, w - 1);
            for (int xx = x0; xx <= std::min(x0 + r, x1); ++xx) {
                zeroIfOut(xx, rowT, m.x + r, m.y + r);
                zeroIfOut(xx, rowB, m.x + r, m.y + m.h - 1 - r);
            }
            for (int xx = std::max(m.x + m.w - 1 - r, x0); xx <= x1; ++xx) {
                zeroIfOut(xx, rowT, m.x + m.w - 1 - r, m.y + r);
                zeroIfOut(xx, rowB, m.x + m.w - 1 - r, m.y + m.h - 1 - r);
            }
        }
    }
    g_roundMasks.clear();

    // 2. Ԥ�� alpha + ULW ���� (P2-3: ���� ui/ulw.h)
    ulwPresent(g_ulw, g_overlayHwnd, g_parentHwnd, px.data(), w, h);
}

// M36: ���ؿ���Լ���ĵ���ʡ�� (UTF-8 ��ȫ, ��������)
static std::string ellipsize(const std::string& s, int pt, int maxW) {
    if (s.empty() || g_text.measureText(s, Tpt(pt)) <= maxW) return s;
    std::string out = s;
    while (out.size() > 1) {
        size_t n = out.size();
        while (n > 0 && (out[n - 1] & 0xC0) == 0x80) --n;   // �������ֽ�
        if (n > 0) --n;                                      // ȥ��һ��ǰ���ֽ�
        out.resize(n);
        if (g_text.measureText(out + "...", Tpt(pt)) <= maxW) break;
    }
    return out + "...";
}

// M36: ����ͼ cover ���� + ע��Բ������; δ����ʱ��ռλ���
static void drawThumbCover(const std::string& path, SDL_Rect rc, int rad) {
    auto it = g_thumbTex.find(path);
    if (it == g_thumbTex.end() || !it->second) {
        SDL_SetRenderDrawColor(g_sdlRdr, ui::SURFACE1_R, ui::SURFACE1_G,
                               ui::SURFACE1_B, 255);
        SDL_RenderFillRect(g_sdlRdr, &rc);
        svgicon::draw(g_sdlRdr, "play", rc.x + rc.w / 2, rc.y + rc.h / 2, U(20),
                      255, 255, 255, 255);
    } else {
        g_thumbAccess[path] = SDL_GetTicks();  // LRU: update access time
        int tw = 0, th = 0;
        SDL_QueryTexture(it->second, nullptr, nullptr, &tw, &th);
        SDL_Rect src = {0, 0, tw, th};
        double dstA = (double)rc.w / std::max(1, rc.h);
        double srcA = (double)tw / std::max(1, th);
        if (srcA > dstA + 0.01) {          // Դ���� �� ������
            int cw = (int)(th * dstA);
            src.x = (tw - cw) / 2; src.w = cw;
        } else if (srcA < dstA - 0.01) {   // Դ��խ �� ������
            int ch = (int)(tw / dstA);
            src.y = (th - ch) / 2; src.h = ch;
        }
        SDL_RenderCopy(g_sdlRdr, it->second, &src, &rc);
    }
    if (rad > 0) g_roundMasks.push_back({rc.x, rc.y, rc.w, rc.h, rad});
}

// ================================================================
// renderOverlay() — the main UI rendering function
// ================================================================

static Uint32 s_frameCount = 0;
static Uint32 s_renderTimeAcc = 0;
void renderOverlay() {
    if (!g_sdlRdr || !g_sdlWin) return;

    uploadThumbs(g_sdlRdr);   // Upload pending thumbnail RGB to GPU textures

    // LRU eviction: keep at most 60 thumbnail textures in VRAM
    static const int kMaxThumbTex = 60;
    if ((int)g_thumbTex.size() > kMaxThumbTex) {
        // Find and evict oldest entries
        while ((int)g_thumbTex.size() > kMaxThumbTex - 10) {
            std::string oldestPath;
            Uint32 oldestTick = UINT32_MAX;
            for (auto& kv : g_thumbAccess) {
                if (kv.second < oldestTick && g_thumbTex.count(kv.first)) {
                    oldestTick = kv.second;
                    oldestPath = kv.first;
                }
            }
            if (oldestPath.empty()) break;
            auto it = g_thumbTex.find(oldestPath);
            if (it != g_thumbTex.end()) {
                SDL_DestroyTexture(it->second);
                g_thumbTex.erase(it);
            }
            g_thumbAccess.erase(oldestPath);
        }
    }

    // ���� UI �������� ARGB ����(�� alpha); �󱸻��岻ʹ��
    // ֱ���� g_ui.winW/winH (WM_SIZE �Ѹ���), ���� SDL_GetWindowSize �����ӳ�
    int ow = g_ui.totalW > 0 ? g_ui.totalW : g_ui.winW;
    int oh = g_ui.winH > 0 ? g_ui.winH : 540;
    if (ow <= 0 || oh <= 0) return;
    if (!ovTexEnsure(ow, oh)) return;
    SDL_SetRenderTarget(g_sdlRdr, g_ovTex);

    SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 0);   // ȫ͸����(per-pixel alpha)
    SDL_RenderClear(g_sdlRdr);

    int w = g_ui.winW, h = g_ui.winH, totalW = g_ui.totalW;

    if (!g_mpv || !g_mpv->hasMedia()) {
        // --- M36 welcome page: Apple ���� Hero + YouTube ����ͼ��Ƭ ---
        int w = g_ui.winW, h = g_ui.winH, totalW = g_ui.totalW;

        // �볡���� (�뿪��ӭҳʱ����, ���·����ŷ�֧����)
        g_ui.introAlpha = std::min(1.0f, g_ui.introAlpha + 0.055f);
        Uint8 fa8 = (Uint8)(255 * g_ui.introAlpha);
        auto A8 = [&](Uint8 base) { return (Uint8)(base * g_ui.introAlpha); };

        g_ui.continueHits.clear();
        g_ui.gridHits.clear();
        std::vector<std::string> wantThumbs;

        // ��ɫ��: ��ý��ʱ mpv �Ӵ����ǰ׵�, ��ӭҳ�����Լ�������ס
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, ui::SURFACE0_R, ui::SURFACE0_G,
                               ui::SURFACE0_B, A8(255));
        SDL_Rect fullBg = {0, 0, totalW, h};
        SDL_RenderFillRect(g_sdlRdr, &fullBg);

        // ---- topbar (�벥��̬һ�µ�ȫ��ͼ��) ----
        drawGradientBar(g_sdlRdr, 0, 0, 0, totalW, curTopH(), 11, 11, 11,
                        (Uint8)(ui::TOPBAR_A0 * g_ui.introAlpha), 0, g_gradCache);
        {
            std::string title = i18n::appName();
            g_text.drawText(U(20), U(14), title, Tpt(14), 255, 255, 255, A8(255));
        }
        {
            int iconY = curTopH() / 2;
            int iconSz = U(34);
            int rx = totalW - U(20);
            const char* ids[6] = {"close", "maximize", "minimize",
                                  "list", "pip", "camera"};
            for (int i = 0; i < 6; ++i) {
                if (g_ui.topbarHover == i) {
                    SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
                    if (i == 0) SDL_SetRenderDrawColor(g_sdlRdr, 232, 17, 35, A8(240));
                    else        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, A8(50));
                    int hr = iconSz / 2 + U(2);
                    SDL_Rect hrc = {rx - hr, iconY - hr, hr * 2, hr * 2};
                    SDL_RenderFillRect(g_sdlRdr, &hrc);
                }
                Uint8 ic = (g_ui.topbarHover == i) ? 255 : 235;
                svgicon::draw(g_sdlRdr, ids[i], rx, iconY, U(28), 255, 255, 255, A8(255));
                rx -= iconSz;
            }
        }

        // ---- Hero: ����Բ��Ӧ��ͼ�� (�������Զ�����) ----
        const int margin = U(48);
        bool compact = (h < U(660));
        int iconSz = compact ? U(60) : U(88);
        int ix = (w - iconSz) / 2;
        int iy = curTopH() + (compact ? U(20) : U(36));
        roundedRectFill(g_sdlRdr, ix, iy, iconSz, iconSz, U(20),
                        ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, A8(255));
        // �ڹ���: ���Ϲ�Դ, ˫��ͬ��Բ (ȫ����ͼ���ڲ�)
        fillCircle(g_sdlRdr, ix + U(28), iy + U(28), U(18),
                   ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, A8(38));
        fillCircle(g_sdlRdr, ix + U(28), iy + U(28), U(10),
                   ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, A8(52));
        svgicon::draw(g_sdlRdr, "play", ix + iconSz / 2, iy + iconSz / 2, U(30),
                      255, 255, 255, A8(255));

        // ---- ��Ʒ�� + ���� (��ఴ�ֺ�ʵ�ʸ߶�, ���ص�) ----
        int namePt = Tpt(ui::T_DISPLAY);
        int nameHpx = (int)(namePt * g_dpi * 1.4f);   // GDI �и߽���
        int nameY = iy + iconSz + (compact ? U(10) : U(18));
        {
            std::string nm = i18n::appName();
            int nw = g_text.measureText(nm, namePt);
            g_text.drawText((w - nw) / 2, nameY, nm, namePt,
                            255, 255, 255, A8(255));
        }
        int tagY = nameY + nameHpx + U(4);
        {
            std::string tg = i18n::tagline();
            int tgw = g_text.measureText(tg, Tpt(ui::T_BODY));
            g_text.drawText((w - tgw) / 2, tagY, tg, Tpt(ui::T_BODY),
                            170, 170, 178, A8(255));
        }

        // ---- ˫ҩ�谴ť (MD3: ��������� + ��ߴβ���) ----
        int btnH = compact ? U(40) : U(46);
        int btnY = tagY + U(18) + (compact ? U(6) : U(18));
        {
            std::string l1 = i18n::openFile(), l2 = i18n::openFolder();
            int w1 = g_text.measureText(l1, Tpt(ui::T_BODY)) + U(48);
            int w2 = g_text.measureText(l2, Tpt(ui::T_BODY)) + U(48);
            int gap = U(14);
            int bx1 = (w - (w1 + gap + w2)) / 2;
            int bx2 = bx1 + w1 + gap;
            g_ui.heroFileBtn   = {bx1, btnY, w1, btnH};
            g_ui.heroFolderBtn = {bx2, btnY, w2, btnH};

            bool hov1 = (g_ui.mouseX >= bx1 && g_ui.mouseX <= bx1 + w1 &&
                         g_ui.mouseY >= btnY && g_ui.mouseY <= btnY + btnH);
            bool hov2 = (g_ui.mouseX >= bx2 && g_ui.mouseX <= bx2 + w2 &&
                         g_ui.mouseY >= btnY && g_ui.mouseY <= btnY + btnH);

            // ���ʽ����ť: ��, ��ͣ����Ϊ��������
            roundedRectFill(g_sdlRdr, bx1, btnY, w1, btnH, btnH / 2,
                            hov1 ? ui::ACCENT2_R : ui::ACCENT_R_,
                            hov1 ? ui::ACCENT2_G : ui::ACCENT_G_,
                            hov1 ? ui::ACCENT2_B : ui::ACCENT_B_, A8(255));
            int t1w = g_text.measureText(l1, Tpt(ui::T_BODY));
            g_text.drawText(bx1 + (w1 - t1w) / 2, btnY + U(12), l1,
                            Tpt(ui::T_BODY), 255, 255, 255, A8(255));

            // ���ʽ�ΰ�ť: �����, ��ͣ���𱳾�
            if (hov2) {
                SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, A8(22));
                SDL_Rect fb = {bx2, btnY, w2, btnH};
                SDL_RenderFillRect(g_sdlRdr, &fb);
            }
            roundedRectStroke(g_sdlRdr, bx2, btnY, w2, btnH, btnH / 2,
                              255, 255, 255, A8(hov2 ? 90 : 60));
            int t2w = g_text.measureText(l2, Tpt(ui::T_BODY));
            g_text.drawText(bx2 + (w2 - t2w) / 2, btnY + U(12), l2,
                            Tpt(ui::T_BODY), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5, A8(255));
        }
        // ��ק��ʾ (������ť�·�)
        int hintY = btnY + btnH + U(8);
        {
            std::string dh = i18n::dropAnywhere();
            int dw = g_text.measureText(dh, Tpt(ui::T_CAPTION));
            g_text.drawText((w - dw) / 2, hintY, dh,
                            Tpt(ui::T_CAPTION), 140, 140, 148, A8(255));
        }

        // ---- �����ۿ��� (YouTube ����ͼ��Ƭ) ----
        int contentY = hintY + U(22) + (compact ? U(24) : U(40));
        struct CWItem { std::string path; double pos, dur; long long ts; };
        static std::vector<CWItem> cw;
        static uint32_t cwBuildTick = 0;
        uint32_t nowTick = SDL_GetTicks();
        if (nowTick - cwBuildTick > 2000 || cwBuildTick == 0) {
            cw.clear();
            for (const auto& kv : g_cfg.history) {
                const HistoryEntry& e = kv.second;
                if (e.pos > 1.0 && (e.dur <= 0 || e.pos < e.dur * 0.95))
                    cw.push_back({kv.first, e.pos, e.dur, e.lastPlayed});
            }
            if (!cw.empty()) {
                std::sort(cw.begin(), cw.end(),
                          [](const CWItem& a, const CWItem& b) { return a.ts > b.ts; });
            }
            cwBuildTick = nowTick;
        }
        if (!cw.empty()) {
            int cardW = compact ? U(150) : U(180);
            int gap = U(14);
            int maxCards = std::max(1, (totalW - margin * 2 + gap) / (cardW + gap));
            int nShow = std::min((int)cw.size(), maxCards);
            int rowW = nShow * cardW + (nShow - 1) * gap;
            int gx = ((totalW > w ? totalW : w) - rowW) / 2;   // �б�����ʱ������ȫ�ͻ���

            {
                std::string hd = i18n::continueWatching();
                g_text.drawText(std::max(margin, gx), contentY - U(30), hd,
                                Tpt(ui::T_HEADLINE), 255, 255, 255, A8(255));
            }
            for (int i = 0; i < nShow; ++i) {
                const CWItem& it = cw[i];
                int cx = gx + i * (cardW + gap);
                int thumbH = cardW * 9 / 16;
                SDL_Rect trc = {cx, contentY, cardW, thumbH};
                drawThumbCover(it.path, trc, U(8));

                // ������ (��)
                if (it.dur > 0) {
                    float frac = (float)std::min(1.0, it.pos / it.dur);
                    int pbH = U(3);
                    SDL_SetRenderDrawColor(g_sdlRdr, 40, 40, 44, A8(220));
                    SDL_Rect pbg = {cx, contentY + thumbH - pbH, cardW, pbH};
                    SDL_RenderFillRect(g_sdlRdr, &pbg);
                    int pw = (int)(cardW * frac);
                    if (pw > 0) {
                        SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_,
                                               ui::ACCENT_B_, A8(255));
                        SDL_Rect pfg = {cx, contentY + thumbH - pbH, pw, pbH};
                        SDL_RenderFillRect(g_sdlRdr, &pfg);
                    }
                }
                // ���� (�������ؼ�ʡ��)
                std::string fn = fileNameOf(it.path);
                fn = ellipsize(fn, ui::T_BODY, cardW);
                g_text.drawText(cx, contentY + thumbH + U(8), fn,
                                Tpt(ui::T_BODY), 235, 235, 240, A8(255));
                // ������: ���� xx% �� ʱ���
                char tb1[16];
                formatTime(tb1, sizeof(tb1), it.pos);
                std::string sub2 = std::string(T("已播放 ", "Watched ")) +
                                   (it.dur > 0 ? std::to_string((int)(it.pos / it.dur * 100 + 0.5)) + "% 时 " : "") +
                                   tb1;
                g_text.drawText(cx, contentY + thumbH + U(30), sub2,
                                Tpt(ui::T_CAPTION), 150, 150, 158, A8(255));

                g_ui.continueHits.push_back({it.path, {cx, contentY - U(4), cardW, thumbH + U(52)}});
                wantThumbs.push_back(it.path);
            }
            contentY += cardW * 9 / 16 + (compact ? U(58) : U(76));
        }

        // ---- �ļ��ж������� (����ͼ��) ----
        if (!g_playlist.empty()) {
            {
                std::string hd = i18n::playlist();
                g_text.drawText(std::max(margin, (w - 0) / 2), contentY - U(26), hd,
                                Tpt(ui::T_HEADLINE), 255, 255, 255, A8(255));
            }
            int cardW = U(180);
            int gap = U(14);
            int cols = std::max(1, std::min((int)g_playlist.size(),
                               (w - margin * 2 + gap) / (cardW + gap)));
            int gridW = cols * cardW + (cols - 1) * gap;
            int gx = (w - gridW) / 2;
            int thumbH = cardW * 9 / 16;
            int rowsMax = std::max(1, (h - contentY - U(70)) / (thumbH + U(58)));
            int shown = std::min((int)g_playlist.size(), cols * rowsMax);
            std::string curPath = g_mpv ? g_mpv->path() : "";
            for (int i = 0; i < shown; ++i) {
                int col = i % cols, row = i / cols;
                int cx = gx + col * (cardW + gap);
                int cy = contentY + row * (thumbH + U(58));
                bool isCur = (g_playlist[i] == curPath);
                bool hov = (g_ui.mouseX >= cx && g_ui.mouseX <= cx + cardW &&
                            g_ui.mouseY >= cy && g_ui.mouseY <= cy + thumbH + U(50));

                drawThumbCover(g_playlist[i], {cx, cy, cardW, thumbH}, U(8));
                // ��ǰ��컷 / ��ͣ�׻�
                if (isCur)
                    roundedRectStroke(g_sdlRdr, cx - U(2), cy - U(2), cardW + U(4),
                                      thumbH + U(4), U(9),
                                      ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, A8(255));
                else if (hov)
                    roundedRectStroke(g_sdlRdr, cx - U(2), cy - U(2), cardW + U(4),
                                      thumbH + U(4), U(9), 255, 255, 255, A8(80));

                std::string fn = ellipsize(fileNameOf(g_playlist[i]), ui::T_CAPTION, cardW);
                g_text.drawText(cx, cy + thumbH + U(8), fn,
                                Tpt(ui::T_CAPTION), isCur ? 255 : 225, isCur ? 255 : 225,
                                isCur ? 255 : 230, A8(255));
                // ʱ��/���ȸ�����
                auto hit = g_cfg.history.find(g_playlist[i]);
                double hp = (hit != g_cfg.history.end()) ? hit->second.pos : 0;
                char sub[40] = "";
                if (hp > 0) {
                    char tb[16];
                    formatTime(tb, sizeof(tb), hp);
                    std::snprintf(sub, sizeof(sub), "@ %s", tb);
                }
                if (sub[0])
                    g_text.drawText(cx, cy + thumbH + U(26), sub,
                                    Tpt(ui::T_CAPTION), 140, 140, 148, A8(255));

                g_ui.gridHits.push_back({i, {cx, cy, cardW, thumbH + U(50)}});
                wantThumbs.push_back(g_playlist[i]);
            }
        }

        // �ύ��ӭҳ�ɼ�����ͼ���� (�벥���б����ϲ�����)
        if (!wantThumbs.empty()) {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            for (auto& p : wantThumbs)
                if (!g_thumbRgb.count(p) && !g_thumbTex.count(p) &&
                    std::find(g_thumbWant.begin(), g_thumbWant.end(), p) == g_thumbWant.end())
                    g_thumbWant.push_back(p);
        }

        // ---- �ײ�: ������ʾ(����) + �汾(����) ----
        {
            std::string hint = T("空格 播放/暂停 → 方向键 快进退 → F 全屏 → M 静音",
                                 "Space Play/Pause \u00bb Arrows Seek \u00bb F Fullscreen \u00bb M Mute");
            int hw = g_text.measureText(hint, Tpt(ui::T_CAPTION));
            g_text.drawText((totalW - hw) / 2, h - U(30), hint,
                            Tpt(ui::T_CAPTION), ui::HINT_TEXT, ui::HINT_TEXT, ui::HINT_TEXT + 6, A8(200));
            std::string ver = std::string("v") + PHANTOM_VERSION;
            int vw = g_text.measureText(ver, Tpt(ui::T_CAPTION));
            g_text.drawText(totalW - vw - U(16), h - U(30), ver,
                            Tpt(ui::T_CAPTION), 110, 110, 116, A8(160));
        }

        overlayPresent();
        return;
    }

    g_ui.introAlpha = 0.0f;   // �뿪��ӭҳ, �´ν������µ���
    double dur = g_mpv->duration();
    // �ٶ��л�����ݶ��������, ��ֹ time-pos ���䵼�¶���
    static double s_lastPos = 0.0;
    static Uint32 s_freezeStart = 0;
    double pos;
    if (g_ui.seekingDrag) {
        pos = g_ui.seekTarget;
    } else if (g_mpv->seekbarFrozen()) {
        // �����ڼ�: �ö���ǰ�� pos + ������ǽ��ʱ�� * ���ٶ� �ƽ�
        if (s_freezeStart == 0) { s_lastPos = g_mpv->clock(); s_freezeStart = SDL_GetTicks(); }
        double elapsed = (SDL_GetTicks() - s_freezeStart) / 1000.0;
        pos = s_lastPos + elapsed * g_mpv->speed();
        double d = g_mpv->duration();
        if (d > 0 && pos > d) pos = d;
    } else {
        pos = g_mpv->clock();
        s_freezeStart = 0;  // �ⶳ: ����
    }

    // �ؼ���������: alpha=0 ʱ��������������������������
    float fa = g_ui.ctrlAlpha;
    Uint8 fade = (Uint8)(fa * 255.0f);
    int topOff = -(int)((1.0f - fa) * curTopH() + 0.5f);

    // --- topbar (gradient: glass ��͸��Ч��, ��Ƶ��Լ�ɼ�) ---
    {
        drawGradientBar(g_sdlRdr, 0, 0, topOff, w, U(52), 11, 11, 11,
                        (Uint8)(ui::TOPBAR_A0 * fa), 0, g_gradCache);

        // title (left)
        std::string title = g_mpv->title();
        if (title.empty()) title = "幻影视频";
        if (title.size() > 55) title = title.substr(0, 52) + "...";
        g_text.drawText(U(20), U(14) + topOff, title, Tpt(14), 255, 255, 255);

        // icons (right) �� ����ͼ�� + ��ͣ��������
        int topH = U(52);
        int iconY = topH / 2 + topOff;
        auto A = [&](Uint8 base) { return (Uint8)(base * fa); };
        int iconSz = U(42);
        int iconDrawSz = U(28);

        int hoverR = iconSz / 2 + U(2);
        int rx = w - U(20);
        struct TopDef { const char* id; };
        const TopDef topIcons[] = {
            {"close"}, {"maximize"}, {"minimize"},
            {"list"}, {"pip"}, {"camera"}
        };
        for (int i = 0; i < 6; ++i) {
            // ��ͣ����
            if (g_ui.topbarHover == i) {
                SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
                if (i == 0) {
                    // close: ��ɫ��ͣ
                    SDL_SetRenderDrawColor(g_sdlRdr, 232, 17, 35, A(240));
                } else {
                    SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, A(50));
                }
                SDL_Rect hrc = {rx - hoverR, iconY - hoverR, hoverR * 2, hoverR * 2};
                SDL_RenderFillRect(g_sdlRdr, &hrc);
            }
            // ����ͼ��, �Ŵ�Ӵ�
            svgicon::draw(g_sdlRdr, topIcons[i].id, rx, iconY, iconDrawSz,
                          255, 255, 255, A(255));
            rx -= iconSz;
        }
    }

    // �ؼ�����: �������� alpha ��������
    int ctrlH = U(80);
    int barTop = sbTopY() + (int)((1.0f - fa) * ctrlH + 0.5f);

    // --- ��ͣѹ������ + ���벥��ͼ�� (per-pixel alpha ���͸��, ȫ�������) ---
    if (fa > 0.01f && g_mpv->state() == MpvBackend::State::Paused) {
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, (Uint8)(110 * fa));
        SDL_Rect dim = {0, 0, w, h};
        SDL_RenderFillRect(g_sdlRdr, &dim);
        int ccx = w / 2, ccy = h / 2;
        svgicon::draw(g_sdlRdr, "play", ccx, ccy, U(52),
                      255, 255, 255, (Uint8)(220 * fa));
    }

    // --- gradient background (Ч��ͼ: ���㽥�� �ײ�������ȫ͸) ---
    drawGradientBar(g_sdlRdr, 1, 0, barTop, w, ctrlH, 0, 0, 0, ui::CTRLBAR_A0, ui::CTRLBAR_A1, g_gradCache);

    // --- seekbar (at very top of bar) ---
    if (dur > 0) {
        int tx = sbLeftX(), tw = sbWidth();
        int ty = barTop + U(9);
        bool thumbActive = g_ui.seekbarHover || g_ui.seekingDrag;
        int th = thumbActive ? ui::SEEK_TRACK_H_HOVER : ui::SEEK_TRACK_H;

        // track background
        SDL_Rect bgRc = {tx, ty, tw, th};
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, ui::SEEK_TRACK_A);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);

        // buffer fill (behind progress)
        double buf = g_mpv->bufferFill();
        if (buf > 0.0 && buf < 1.0) {
            int bufW = (int)(tw * buf);
            SDL_Rect bufRc = {tx, ty, bufW, th};
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, ui::SEEK_BUF_A);
            SDL_RenderFillRect(g_sdlRdr, &bufRc);
        }

        // progress
        int progW = (int)(tw * pos / dur);
        if (progW > 0) {
            SDL_Rect prRc = {tx, ty, progW, th};
            SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
            SDL_RenderFillRect(g_sdlRdr, &prRc);
        }

        // --- seekbar thumb (always-visible small dot + hover/drag enlarged glow) ---
        int cx = tx + progW;
        int cy = ty + th / 2;
        // Ĭ��̬: СԲ�� (Ʒ��ɫ)
        int rDefault = std::max(ui::THUMB_R_DEFAULT, (int)(g_ui.winW * 0.003f));
        fillCircle(g_sdlRdr, cx, cy, rDefault,
                   ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
        // hover/drag ̬: �Ŵ��Բ + ��͸������
        if (thumbActive) {
            int rHov = std::max(ui::THUMB_R_HOVER, (int)(g_ui.winW * 0.006f));
            // ��Ȧ���� (ͬɫ��͸��)
            fillCircle(g_sdlRdr, cx, cy, rHov + ui::THUMB_GLOW_R,
                       ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 50);
            // ��ɫʵ��Բ
            fillCircle(g_sdlRdr, cx, cy, rHov, 255, 255, 255, 255);
        }

        // ʱ��Ԥ������ (hover/drag only)
        if (thumbActive) {

            // Ԥ��ʱ�������
            double hoverPos = dur * ((double)(g_ui.mouseX - tx) / tw);
            if (hoverPos < 0) hoverPos = 0;
            if (hoverPos > dur) hoverPos = dur;
            char pv[16];
            formatTime(pv, sizeof(pv), hoverPos);
            int bw = g_text.measureText(pv, Tpt(11)) + U(16);
            int bh = U(22);
            int bx = g_ui.mouseX - bw / 2;
            if (bx < tx) bx = tx;
            if (bx + bw > tx + tw) bx = tx + tw - bw;
            int by = ty - bh - U(10);
            // ���ݱ���(Բ�ǽ���)
            SDL_SetRenderDrawColor(g_sdlRdr, 20, 20, 22, 235);
            SDL_Rect bubble = {bx + U(4), by, bw - U(8), bh};
            SDL_RenderFillRect(g_sdlRdr, &bubble);
            SDL_Rect bubbleL = {bx, by + U(4), U(4), bh - U(8)};
            SDL_RenderFillRect(g_sdlRdr, &bubbleL);
            SDL_Rect bubbleR = {bx + bw - U(4), by + U(4), U(4), bh - U(8)};
            SDL_RenderFillRect(g_sdlRdr, &bubbleR);
            fillCircle(g_sdlRdr, bx + U(4), by + U(4), U(4), 20, 20, 22, 235);
            fillCircle(g_sdlRdr, bx + bw - U(4), by + U(4), U(4), 20, 20, 22, 235);
            fillCircle(g_sdlRdr, bx + U(4), by + bh - U(4), U(4), 20, 20, 22, 235);
            fillCircle(g_sdlRdr, bx + bw - U(4), by + bh - U(4), U(4), 20, 20, 22, 235);
            // ��������
            g_text.drawText(bx + U(8), by + U(4), pv, Tpt(11), 255, 255, 255);
        }
    }

    // --- controlbar row1 (Ч��ͼ����): prev/PLAY�׵�/next/time ... ��Ļ/����/����/����/����/ȫ�� ---
    {
        Row1Layout L;
        bool volOpen = (g_ui.volumeSliderOpen || g_ui.volumeDragging);
        layoutRow1(w, barTop, volOpen, L);
        auto A = [&](Uint8 base) { return (Uint8)(base * fa); };
        const int iconC = ui::ICON_BRIGHT, text2 = ui::ICON_DIM;

        // prev
        int ctrlIconSz = U(42);
        svgicon::draw(g_sdlRdr, "prev", L.prev.x + ctrlIconSz / 2, L.prev.y + ctrlIconSz / 2, U(28),
                      255, 255, 255, A(255));
        // PLAY ��ͼ��
        {
            const char* pi = (g_mpv->state() == MpvBackend::State::Paused) ? "play" : "pause";
            svgicon::draw(g_sdlRdr, pi, L.play.x + L.play.w / 2, L.play.y + L.play.h / 2,
                          U(28), 255, 255, 255, A(255));
        }
        // next
        svgicon::draw(g_sdlRdr, "next", L.next.x + ctrlIconSz / 2, L.next.y + ctrlIconSz / 2, U(28),
                      255, 255, 255, A(255));
        // time��tabular �۸�: �ȿ������屣֤��
        {
            char cur[32], tot[32], ts[80];
            formatTime(cur, sizeof(cur), pos);
            formatTime(tot, sizeof(tot), dur);
            std::snprintf(ts, sizeof(ts), "%s / %s", cur, tot);
            g_text.drawText(L.timeX, L.cy - U(9), ts, Tpt(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        }

        // �Ҳ� textbtn �� (���� + ͼ��)
        auto drawTextBtn = [&](const SDL_Rect& rc, const char* label,
                               const char* iconId, Uint8 ir, Uint8 ig, Uint8 ib) {
            int tw = g_text.measureText(label, Tpt(12));
            int tx = rc.x + U(8);
            g_text.drawText(tx, rc.y + U(10), label, Tpt(12), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5);
            svgicon::draw(g_sdlRdr, iconId, tx + tw + U(9), rc.y + U(17), U(22),
                          ir, ig, ib, A(255));
        };
        // ��Ļ
        {
            Uint8 ic = g_mpv->subVisible() ? 255 : 110;
            drawTextBtn(L.subBtn, i18n::subtitles(), "cc", ic, ic, ic);
        }
        // ���� (���ְ�ť)
        {
            Uint8 ic = g_mpv->audioTracks().size() > 1 ? 255 : 110;
            drawTextBtn(L.audioBtn, i18n::audioTrack(), "cc", ic, ic, ic);
        }
        // �½� (���ְ�ť)
        {
            Uint8 ic = g_mpv->chapters().size() > 1 ? 255 : 110;
            drawTextBtn(L.chapterBtn, i18n::chapName(), "list", ic, ic, ic);
        }
        // AB loop
        {
            bool abActive = g_mpv->looping();
            Uint8 r = abActive ? ui::ACCENT2_R : ui::TEXT_DIM;
            Uint8 gv = abActive ? ui::ACCENT2_G : ui::TEXT_DIM;
            Uint8 b = abActive ? ui::ACCENT2_B : ui::TEXT_DIM + 5;
            g_text.drawText(L.abBtn.x + U(8), L.abBtn.y + U(10), "AB", Tpt(12), r, gv, b);
        }
        // EQ
        {
            bool eqOn = g_mpv->eqEnabled();
            Uint8 r = eqOn ? ui::ACCENT2_R : ui::TEXT_DIM;
            Uint8 gv = eqOn ? ui::ACCENT2_G : ui::TEXT_DIM;
            Uint8 b = eqOn ? ui::ACCENT2_B : ui::TEXT_DIM + 5;
            g_text.drawText(L.eqBtn.x + U(8), L.eqBtn.y + U(10), "EQ", Tpt(12), r, gv, b);
        }
        // speed
        {
            char spd[16];
            float s = g_mpv->speed();
            if (s == (int)s) std::snprintf(spd, sizeof(spd), "%.0fx", s);
            else             std::snprintf(spd, sizeof(spd), "%.2fx", s);
            int lw = g_text.measureText(i18n::speed(), Tpt(12));
            g_text.drawText(L.speedBtn.x + U(8), L.speedBtn.y + U(10), i18n::speed(), Tpt(12), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5);
            g_text.drawText(L.speedBtn.x + U(8) + lw + U(4), L.speedBtn.y + U(10), spd, Tpt(12),
                            ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B);
        }
        // ���� + �ֱ��ʱ�ǩ
        {
            const char* ql = qualityLabel();
            int qw = g_text.measureText(i18n::quality(), Tpt(12));
            g_text.drawText(L.qualityBtn.x + U(8), L.qualityBtn.y + U(10), i18n::quality(), Tpt(12), ui::TEXT_DIM, ui::TEXT_DIM, ui::TEXT_DIM + 5);
            g_text.drawText(L.qualityBtn.x + U(8) + qw + U(4), L.qualityBtn.y + U(11), ql, Tpt(11), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        }
        // ����ͼ��
        {
            const char* vid = g_mpv->muted() ? "mute" : "volume";
            svgicon::draw(g_sdlRdr, vid, L.volIconCx, L.cy, U(28),
                          255, 255, 255, A(255));
        }
        // ����(����+gear)
        drawTextBtn(L.setBtn, i18n::settings(), "gear", 255, 255, 255);
        // ȫ��
        const char* fid = g_ui.fullscreen ? "exitfull" : "full";
        svgicon::draw(g_sdlRdr, fid, L.fullBtn.x + ctrlIconSz / 2, L.fullBtn.y + ctrlIconSz / 2, U(28),
                      255, 255, 255, A(255));

        // ��������(չ��̬)
        if (volOpen && L.volSliderW > 0) {
            int sldW = U(80);
            int sx = L.volSliderX;
            bool hov = g_ui.volumeSliderHover || g_ui.volumeDragging;
            int slH = hov ? U(5) : U(4);
            int sy = L.cy - slH / 2;
            // �������
            Uint8 trackA = hov ? 65 : 40;
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, trackA);
            SDL_Rect trk = {sx, sy, sldW, slH};
            SDL_RenderFillRect(g_sdlRdr, &trk);
            // �����
            float v = g_mpv->volume();
            int fw = (int)(sldW * v);
            if (fw > 1) {
                SDL_Rect fl = {sx, sy, fw, slH};
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 255);
                SDL_RenderFillRect(g_sdlRdr, &fl);
            }
            // Բ�� thumb (hover ʱ�ӹ���)
            int tx = sx + fw;
            int ty = L.cy;
            int thumbR = hov ? U(7) : U(6);
            if (hov) {
                // ����
                fillCircle(g_sdlRdr, tx, ty, thumbR + U(3), 255, 255, 255, 40);
            }
            fillCircle(g_sdlRdr, tx, ty, thumbR, 255, 255, 255, 255);
        }
    }

    // --- buffering indicator (animated spinner + percentage) ---
    if (g_mpv->isBuffering() && g_mpv->bufferFill() < 0.5) {
        int cx = w / 2, cy = barTop + U(60);
        int r = U(18);
        // 绘制旋转弧线 (6 段弧，旋转动画)
        Uint32 ticks = SDL_GetTicks();
        double angle = (ticks % 1200) / 1200.0 * 6.28318;  // 1.2 秒一圈
        int segments = 8;
        for (int i = 0; i < segments; i++) {
            double a1 = angle + (double)i / segments * 6.28318;
            double a2 = a1 + 6.28318 / segments * 0.55;
            int x1 = cx + (int)(std::cos(a1) * r);
            int y1 = cy + (int)(std::sin(a1) * r);
            int x2 = cx + (int)(std::cos(a2) * r);
            int y2 = cy + (int)(std::sin(a2) * r);
            Uint8 alpha = (Uint8)(180 - i * (160 / segments));
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, alpha);
            SDL_RenderDrawLine(g_sdlRdr, x1, y1, x2, y2);
        }
        // 百分比文字
        double lvl = g_mpv->bufferingLevel();
        if (lvl > 0.01 && lvl < 1.0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d%%", (int)(lvl * 100));
            g_text.drawText(cx - U(12), cy + r + U(6), buf, Tpt(10),
                            ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        } else {
            g_text.drawText(cx - U(22), cy + r + U(6), "Buffering...", Tpt(10),
                            ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        }
    }

    // --- speed popup menu��Ч��ͼ���: Բ��r8/����չ��/k��ע�� ---
    if (g_ui.speedMenuOpen) {
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32);
        int menuW = U(132);
        int menuH = SPEED_PRESET_COUNT * itemH + U(12);
        int menuX = L.speedBtn.x;                        // �밴ť�����
        int menuY = L.speedBtn.y - menuH - U(6);        // ����չ��
        if (menuY < 0) menuY = L.speedBtn.y + L.speedBtn.h + U(6);  // �ռ䲻��ʱ��������
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);

        // Բ�Ǿ���: �Ȼ���������, ����Բ����Ľ�
        int cr = U(8);  // corner radius
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        // �м���Բ�ǲ���(������)
        SDL_Rect midH = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH);
        // �Ľ�Բ
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        // �߿�(��: ֻ��ֱ�߶�)
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);

        float curSpeed = g_mpv->speed();
        for (int i = 0; i < SPEED_PRESET_COUNT; ++i) {
            int iy = menuY + U(6) + i * itemH;
            bool sel = (std::abs(curSpeed - SPEED_PRESETS[i]) < 0.01f);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            char label[16];
            float sp = SPEED_PRESETS[i];
            if (sp == (int)sp) std::snprintf(label, sizeof(label), "%.2fx", sp);
            else               std::snprintf(label, sizeof(label), "%.2fx", sp);
            g_text.drawText(menuX + U(10), iy + U(6), label, Tpt(13), tr, tg, tb);
            // k ��ע: ��/����/��
            const char* k = (sp < 0.99f) ? T("慢", "Slow") : (sp < 1.01f) ? T("正常", "Normal") :
                            (sp < 2.01f) ? nullptr : T("快", "Fast");
            if (k) {
                int kw = g_text.measureText(k, Tpt(11));
                g_text.drawText(menuX + menuW - kw - U(10), iy + U(7), k, Tpt(11), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
            }
        }
    }

    // --- quality popup menu (����: ��Ƶ��Ϣ + ����Ԥ��) ---
    if (g_ui.qualityMenuOpen) {
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32);
        int menuW = U(140);
        int infoH = U(38);
        int menuH = infoH + QUALITY_PRESET_COUNT * itemH + U(12);
        int menuX = L.qualityBtn.x;
        int menuY = L.qualityBtn.y - menuH - U(6);
        if (menuY < 0) menuY = L.qualityBtn.y + L.qualityBtn.h + U(6);
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);

        // Բ�Ǿ��α���
        int cr = U(8);
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);

        // ��Ƶ��Ϣ��
        int iw = g_mpv->videoWidth(), ih = g_mpv->videoHeight();
        char info[64];
        std::snprintf(info, sizeof(info), "%dx%d", iw, ih);
        g_text.drawText(menuX + U(10), menuY + U(8), info, Tpt(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        // �ָ���
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 20);
        SDL_RenderDrawLine(g_sdlRdr, menuX + U(8), menuY + infoH - U(4),
                           menuX + menuW - U(8), menuY + infoH - U(4));

        // Ԥ��ѡ��
        const char* qNames[] = { T("省电", "Power Saving"), T("标准", "Standard"), T("卓越", "Ultimate") };
        for (int i = 0; i < QUALITY_PRESET_COUNT; ++i) {
            int iy = menuY + infoH + i * itemH;
            bool sel = (g_ui.qualityPreset == i);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            g_text.drawText(menuX + U(10), iy + U(8), qNames[i], Tpt(13), tr, tg, tb);
            // ��ǰѡ�б��
            if (sel) {
                fillCircle(g_sdlRdr, menuX + menuW - U(18), iy + U(14), U(4), ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, 255);
            }
        }
    }
    // --- EQ popup menu (6Ƶ�ξ�����) ---
    if (g_ui.eqMenuOpen) {
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        static const char* bandNames[] = {"60Hz","170Hz","310Hz","600Hz","3kHz","12kHz"};
        int sliderW = U(100);
        int itemH = U(36);
        int menuW = U(200);
        int menuH = U(32) + 6 * itemH + U(40) + U(50);  // title + 6 bands + reset + presets
        int menuX = w / 2 - menuW / 2;           // ������ʾ
        int menuY = h / 2 - menuH / 2;

        // ����
        int cr = U(8);
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        // ����
        g_text.drawText(menuX + U(10), menuY + U(10), i18n::equalizer(), Tpt(13), 255, 255, 255);

        // 6 频段滑块
        int baseY = menuY + U(32);
        int trackX = menuX + U(60);
        int trackW = sliderW;
        for (int i = 0; i < 6; ++i) {
            int iy = baseY + i * itemH;
            g_text.drawText(menuX + U(10), iy + U(8), bandNames[i], Tpt(11), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
            // ���
            SDL_SetRenderDrawColor(g_sdlRdr, 58, 58, 62, 255);
            SDL_Rect trk = {trackX, iy + U(14), trackW, U(4)};
            SDL_RenderFillRect(g_sdlRdr, &trk);
            // ����λ��: gain -12..+12 �� 0..1
            float gain = g_mpv->eqGain(i);
            float norm = (gain + 12.0f) / 24.0f;
            if (norm < 0.0f) norm = 0.0f; if (norm > 1.0f) norm = 1.0f;
            int thumbX = trackX + (int)(norm * trackW);
            // ���� thumb
            fillCircle(g_sdlRdr, thumbX, iy + U(16), U(6), ui::ACCENT2_R, ui::ACCENT2_G, ui::ACCENT2_B, 255);
            // ��ֵ
            char val[16];
            std::snprintf(val, sizeof(val), "%+.0f", gain);
            g_text.drawText(trackX + trackW + U(8), iy + U(8), val, Tpt(11), ui::ICON_BRIGHT, ui::ICON_BRIGHT, 231);
            // �洢�����������ڵ��
            static SDL_Rect s_bandRects[6];
            s_bandRects[i] = {trackX - U(8), iy, trackW + U(16), itemH};
            // (hit-test �ں��洦��)
        }
        // Reset ��ť
        int resetY = baseY + 6 * itemH + U(4);
        SDL_Rect resetRc = {menuX + menuW / 2 - U(30), resetY, U(60), U(26)};
        SDL_SetRenderDrawColor(g_sdlRdr, 58, 58, 62, 255);
        SDL_RenderFillRect(g_sdlRdr, &resetRc);
        g_text.drawText(resetRc.x + U(14), resetRc.y + U(5), i18n::reset(), Tpt(11), ui::ICON_BRIGHT, ui::ICON_BRIGHT, 231);

        // P1-6: EQ Ԥ�谴ť
        struct EqPreset { const char* name; float bands[6]; };
        static const EqPreset presets[] = {
            { nullptr,    { 0,  0,  0,  0,  0,  0 } },  // Flat — 指针占位，运行时用 i18n
            { nullptr,    { 6,  4,  1, -1, -2, -3 } },  // Bass
            { nullptr,    {-3, -2, -1,  1,  4,  6 } },  // Treble
            { nullptr,    {-2, -1,  3,  4,  2, -1 } },  // Vocal
            { nullptr,    { 5,  3, -1, -1,  3,  5 } },  // Rock
        };
        static const char* presetNames[] = {
            i18n::presetFlat(), i18n::presetBass(), i18n::presetTreble(),
            i18n::presetVocal(), i18n::presetRock()
        };
        static const int kPresetCount = (int)(sizeof(presets) / sizeof(presets[0]));
        int presetY = resetY + U(30);
        int presetBtnW = (menuW - U(20)) / kPresetCount;
        g_text.drawText(menuX + U(10), presetY - U(2), T("预设:", "Presets:"), Tpt(10), 140, 140, 148);
        for (int i = 0; i < kPresetCount; ++i) {
            int bx = menuX + U(10) + i * presetBtnW;
            int by = presetY + U(14);
            int bw = presetBtnW - U(4);
            int bh = U(22);
            // ����Ƿ�ǰƥ��
            bool match = true;
            for (int b = 0; b < 6; ++b) {
                if (std::abs(g_mpv->eqGain(b) - presets[i].bands[b]) > 0.5f) { match = false; break; }
            }
            SDL_SetRenderDrawColor(g_sdlRdr, match ? 59 : 48, match ? 130 : 48, match ? 246 : 52, 255);
            SDL_Rect btnRc = {bx, by, bw, bh};
            SDL_RenderFillRect(g_sdlRdr, &btnRc);
            g_text.drawText(bx + U(4), by + U(4), presetNames[i], Tpt(9),
                            match ? 255 : 180, match ? 255 : 180, match ? 255 : 186);
            // �洢��ť����
            static SDL_Rect s_presetRects[5];
            s_presetRects[i] = btnRc;
        }
    }
    // --- subtitle track popup menu ---
    if (g_ui.subMenuOpen) {
        auto subs = g_mpv->subTracks();
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32), menuW = U(180);
        int menuH = (int)(subs.size() + 2) * itemH + U(12);  // +1 off +1 load external
        int menuX = L.subBtn.x;
        int menuY = L.subBtn.y - menuH - U(6);
        if (menuY < 0) menuY = L.subBtn.y + L.subBtn.h + U(6);
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);
        // rounded rect bg
        int cr = U(8);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH2 = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH2);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);
        int curSubId = g_mpv->currentSubId();
        bool subVis = g_mpv->subVisible();
        // item 0: �ر���Ļ
        {
            int iy = menuY + U(6);
            bool sel = !subVis;
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            g_text.drawText(menuX + U(10), iy + U(6), T("关闭", "Off"), Tpt(13), tr, tg, tb);
        }
        for (int i = 0; i < (int)subs.size(); ++i) {
            int iy = menuY + U(6) + (i + 1) * itemH;
            bool sel = (subs[i].id == curSubId && subVis);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            g_text.drawText(menuX + U(10), iy + U(6), subs[i].desc.c_str(), Tpt(13), tr, tg, tb);
        }
        // �ָ���
        int sepY = menuY + U(6) + (int)(subs.size() + 1) * itemH - U(2);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + U(10), sepY, menuX + menuW - U(10), sepY);
        // �����ⲿ��Ļ
        {
            int iy = menuY + U(6) + (int)(subs.size() + 1) * itemH;
            g_text.drawText(menuX + U(10), iy + U(6), T("加载外部字幕...", "Load external..."),
                            Tpt(13), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B);
        }
    }
    // --- audio track popup menu ---
    if (g_ui.audioMenuOpen) {
        auto tracks = g_mpv->audioTracks();
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32), menuW = U(180);
        int menuH = (int)tracks.size() * itemH + U(12);
        int menuX = L.audioBtn.x;
        int menuY = L.audioBtn.y - menuH - U(6);
        if (menuY < 0) menuY = L.audioBtn.y + L.audioBtn.h + U(6);
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);
        int cr = U(8);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH2 = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH2);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);
        int curAudioId = g_mpv->currentAudioTrack();
        for (int i = 0; i < (int)tracks.size(); ++i) {
            int iy = menuY + U(6) + i * itemH;
            bool sel = (tracks[i].id == curAudioId);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            g_text.drawText(menuX + U(10), iy + U(6), tracks[i].desc.c_str(), Tpt(13), tr, tg, tb);
        }
    }
    // --- chapter popup menu ---
    if (g_ui.chapterMenuOpen) {
        auto chs = g_mpv->chapters();
        Row1Layout L;
        layoutRow1(w, barTop, g_ui.volumeSliderOpen || g_ui.volumeDragging, L);
        int itemH = U(32), menuW = U(240);
        int menuH = (int)chs.size() * itemH + U(12);
        int menuX = L.chapterBtn.x;
        int menuY = L.chapterBtn.y - menuH - U(6);
        if (menuY < 0) menuY = L.chapterBtn.y + L.chapterBtn.h + U(6);
        if (menuX + menuW > w - U(8)) menuX = w - menuW - U(8);
        int cr = U(8);
        SDL_Rect bgRc = {menuX + cr, menuY, menuW - cr * 2, menuH};
        SDL_SetRenderDrawColor(g_sdlRdr, 24, 24, 26, 255);
        SDL_RenderFillRect(g_sdlRdr, &bgRc);
        SDL_Rect midH2 = {menuX, menuY + cr, menuW, menuH - cr * 2};
        SDL_RenderFillRect(g_sdlRdr, &midH2);
        fillCircle(g_sdlRdr, menuX + cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        fillCircle(g_sdlRdr, menuX + menuW - cr, menuY + menuH - cr, cr, 24, 24, 26, 255);
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 26);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY, menuX + menuW - cr, menuY);
        SDL_RenderDrawLine(g_sdlRdr, menuX + cr, menuY + menuH, menuX + menuW - cr, menuY + menuH);
        SDL_RenderDrawLine(g_sdlRdr, menuX, menuY + cr, menuX, menuY + menuH - cr);
        SDL_RenderDrawLine(g_sdlRdr, menuX + menuW, menuY + cr, menuX + menuW, menuY + menuH - cr);
        int curCh = g_mpv->currentChapter();
        for (int i = 0; i < (int)chs.size(); ++i) {
            int iy = menuY + U(6) + i * itemH;
            bool sel = (i == curCh);
            Uint8 tr = sel ? 59 : 228, tg = sel ? 130 : 228, tb = sel ? 246 : 231;
            const char* name = chs[i].title.empty()
                ? T("无标题", "Untitled") : chs[i].title.c_str();
            char label[128];
            std::snprintf(label, sizeof(label), "%d. %s", i + 1, name);
            g_text.drawText(menuX + U(10), iy + U(6), label, Tpt(13), tr, tg, tb);
        }
    }
    if (g_ui.playlistOpen) {
        int panelW, panelX;
        if (!g_ui.fullscreen) {
            panelW = totalW - w;                 // ������չ���Ķ�������
            panelX = w;
        } else {                                  // ȫ���޷�����: ����ʽ
            panelW = U(430);
            panelX = w - panelW;
        }
        if (panelW < U(200)) { panelW = U(200); panelX = w - panelW; }   // ����
        int panelH = h;
        int panelY = 0;

        // panel background����������͸����
        SDL_Rect pRc = {panelX, panelY, panelW, panelH};
        SDL_SetRenderDrawColor(g_sdlRdr, 16, 16, 17, 255);
        SDL_RenderFillRect(g_sdlRdr, &pRc);
        // left border
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 25);
        SDL_RenderDrawLine(g_sdlRdr, panelX, panelY, panelX, panelY + panelH);

        // title + �ر�ť��Ч��ͼ .pl-head��
        g_text.drawText(panelX + U(14), panelY + U(16), i18n::playlist(), Tpt(13), 255, 255, 255);
        int closeX = panelX + panelW - U(44);
        int closeY = panelY + U(8);
        SDL_Rect closeRc = {closeX, closeY, U(36), U(36)};
        svgicon::draw(g_sdlRdr, "close", closeX + U(18), closeY + U(18), U(22),
                      255, 255, 255, 255);
        g_ui.plCloseRect = closeRc;

        // items from playlist queue����Ƭ��: thumb100��56+dur�Ǳ�+title+state��
        int itemY = panelY + U(45);
        int itemH = U(72);                       // ��Ƭ�߶�(56 thumb+padding)
        int scroll = g_ui.playlistScroll;
        std::vector<std::string> visiblePaths;
        // �ü����б���: ����ʱ���ݲ����ס�̶�������
        SDL_Rect listClip = {panelX, itemY, panelW, panelY + panelH - itemY};
        SDL_RenderSetClipRect(g_sdlRdr, &listClip);
        for (size_t pi = 0; pi < g_playlist.size(); ++pi) {
            int iy = itemY + (int)pi * itemH - scroll;
            if (iy + itemH < itemY - U(60)) continue;
            if (iy >= panelY + panelH - U(10)) break;
            const std::string& p = g_playlist[pi];
            visiblePaths.push_back(p);
            bool isCurrent = (g_mpv && g_mpv->path() == p);

            double hpos = 0;
            auto hit = g_cfg.history.find(p);
            if (hit != g_cfg.history.end()) hpos = hit->second.pos;

            // hover ����������ڱ����ڣ�
            bool hovered = (g_ui.mouseX >= panelX + U(8) &&
                            g_ui.mouseX <= panelX + panelW - U(8) &&
                            g_ui.mouseY >= iy && g_ui.mouseY <= iy + itemH - U(6));
            if (isCurrent || hovered) {
                SDL_Rect hlRc = {panelX + U(7), iy, panelW - U(15), itemH - U(4)};
                if (isCurrent) SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 46);
                else           SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 15);
                SDL_RenderFillRect(g_sdlRdr, &hlRc);
            }

            // ����ͼ 100��56 r7������ռλ�� #26262c��#15151a ���ƣ�
            SDL_Rect thRc = {panelX + U(12), iy + U(8), U(100), U(56)};
            SDL_SetRenderDrawColor(g_sdlRdr, 33, 33, 38, 255);
            SDL_RenderFillRect(g_sdlRdr, &thRc);
            auto texIt = g_thumbTex.find(p);
            if (texIt != g_thumbTex.end()) {
                SDL_RenderCopy(g_sdlRdr, texIt->second, nullptr, &thRc);
            } else {
                svgicon::draw(g_sdlRdr, "play", thRc.x + U(50), thRc.y + U(28), U(24),
                              255, 255, 255, 255);
            }
            // dur �Ǳ�(right4 bottom4 ��.72)
            {
                char durBuf[16] = "";
                if (hpos > 1.0) {
                    std::snprintf(durBuf, sizeof(durBuf), "%02d:%02d",
                                  (int)(hpos / 60), (int)hpos % 60);
                    int dw = g_text.measureText(durBuf, Tpt(9)) + U(8);
                    int dx = thRc.x + thRc.w - dw - U(4);
                    int dy = thRc.y + thRc.h - U(18);
                    SDL_Rect db = {dx, dy, dw, U(15)};
                    SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 184);
                    SDL_RenderFillRect(g_sdlRdr, &db);
                    g_text.drawText(dx + U(4), dy + U(2), durBuf, Tpt(9), 255, 255, 255);
                }
            }

            // meta: title һ�� + state ��
            std::string fn = fileNameOf(p);
            int maxTw = panelW - U(140);
            if (maxTw < U(80)) maxTw = U(80);
            {
                // �����ؿ��ض�
                if (g_text.measureText(fn, Tpt(12)) > maxTw) {
                    while (fn.size() > 4 && g_text.measureText(fn + "...", Tpt(12)) > maxTw)
                        fn.pop_back();
                    fn += "...";
                }
                Uint8 tr = isCurrent ? 191 : 240, tg = isCurrent ? 214 : 240,
                      tb = isCurrent ? 255 : 240;   // playing #bfd6ff
                g_text.drawText(thRc.x + thRc.w + U(10), iy + U(10), fn, Tpt(12), tr, tg, tb);
            }
            // state: ���ڲ���(accent2)/�Ѳ���(#6b7280)/δ����(#3f3f46)
            {
                const char* st; Uint8 sr, sg_, sb_;
                if (isCurrent) { st = i18n::playing(); sr = 59; sg_ = 130; sb_ = 246; }
                else if (hpos > 1.0) { st = i18n::played(); sr = 107; sg_ = 114; sb_ = 128; }
                else { st = i18n::unplayed(); sr = 63; sg_ = 63; sb_ = 70; }
                g_text.drawText(thRc.x + thRc.w + U(10), iy + U(32), st, Tpt(11), sr, sg_, sb_);
            }
        }
        SDL_RenderSetClipRect(g_sdlRdr, nullptr);   // ����ü�(��קָʾ��/��������Խ��)

        // ��ק�����Ӿ�����������ָʾ�� + ���������
        if (g_ui.plDragging && g_ui.plDragFrom >= 0) {
            int itemH = U(72);
            int topY = panelY + U(45);
            float rel = (float)(g_ui.plDragY - topY) + g_ui.playlistScroll;
            int drop = (int)(rel / itemH + 0.5f);
            int n = (int)g_playlist.size();
            if (drop < 0) drop = 0;
            if (drop > n) drop = n;
            int lineY = topY + drop * itemH - g_ui.playlistScroll - itemH / 2 + itemH / 2;
            lineY = topY + drop * itemH - g_ui.playlistScroll;
            if (lineY >= panelY && lineY <= panelY + panelH) {
                SDL_Rect line = {panelX + U(4), lineY - U(2), panelW - U(10), U(3)};
                SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                SDL_RenderFillRect(g_sdlRdr, &line);
            }
            int fromY = topY + g_ui.plDragFrom * itemH - g_ui.playlistScroll;
            if (fromY >= panelY && fromY <= panelY + panelH) {
                SDL_Rect hl = {panelX + U(4), fromY - U(2), panelW - U(8), itemH};
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 30);
                SDL_RenderFillRect(g_sdlRdr, &hl);
            }
        }

        // �ύ�ɼ���������ͼ worker����ȱͼ��; �ϲ�����, ����������ͼ������
        {
            std::lock_guard<std::mutex> lk(g_thumbMtx);
            for (auto& p : visiblePaths) {
                if (!g_thumbRgb.count(p) && !g_thumbTex.count(p) &&
                    std::find(g_thumbWant.begin(), g_thumbWant.end(), p) == g_thumbWant.end())
                    g_thumbWant.push_back(p);
            }
        }

        // scrollbar��M33d: ��ͣ����/��ק/�����ҳ��
        {
            int contentH = (int)g_playlist.size() * itemH;
            int viewH = panelH - U(55);
            if (contentH > viewH && contentH > 0) {
                int trackW = U(6);
                int trackX = panelX + panelW - trackW - U(4);
                int trackY = panelY + U(45);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 18);
                SDL_Rect trk = {trackX, trackY, trackW, viewH};
                SDL_RenderFillRect(g_sdlRdr, &trk);
                int barH = std::max(U(30), viewH * viewH / contentH);
                int barY = trackY + g_ui.playlistScroll * (viewH - barH) / (contentH - viewH);
                // hover/��קʱ����
                Uint8 ba = (g_ui.sbHover || g_ui.sbDragging) ? 160 : 70;
                SDL_SetRenderDrawColor(g_sdlRdr, 235, 235, 240, ba);
                SDL_Rect br = {trackX, barY, trackW, barH};
                SDL_RenderFillRect(g_sdlRdr, &br);

                // ��¶���θ����в���
                g_ui.sbTrackX = trackX; g_ui.sbTrackY = trackY;
                g_ui.sbTrackW = trackW; g_ui.sbTrackH = viewH;
                g_ui.sbBarY = barY;     g_ui.sbBarH = barH;
            } else {
                g_ui.sbTrackX = -1;
            }
        }

        if (g_playlist.empty()) {
            g_text.drawText(panelX + U(16), itemY + U(10), i18n::emptyPlaylist(), Tpt(12), 100, 100, 100);
        }
    }

    // --- settings modal panel ---
    if (g_ui.settingsOpen) {
        SettingsGeom sg = settingsGeom(w, h);

        // ģ̬����: ���͸��ѹ��(per-pixel alpha), ��Ƶ��Լ�ɼ�
        SDL_SetRenderDrawBlendMode(g_sdlRdr, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, 140);
        SDL_Rect fullRc = {0, 0, w, h};
        SDL_RenderFillRect(g_sdlRdr, &fullRc);

        // �����Ӱ�������ɢ��
        for (int i = 4; i >= 1; --i) {
            Uint8 sha = (Uint8)(12 * i);
            SDL_SetRenderDrawColor(g_sdlRdr, 0, 0, 0, sha);
            SDL_Rect sr = {sg.panelX - i*2, sg.panelY - i*2, sg.panelW + i*4, sg.panelH + i*4};
            SDL_RenderDrawRect(g_sdlRdr, &sr);
        }

        // panel (Բ�Ǿ���)
        int cr = U(12);
        SDL_SetRenderDrawColor(g_sdlRdr, 28, 28, 30, 255);
        SDL_Rect pBody = {sg.panelX + cr, sg.panelY, sg.panelW - cr*2, sg.panelH};
        SDL_RenderFillRect(g_sdlRdr, &pBody);
        SDL_Rect pH = {sg.panelX, sg.panelY + cr, sg.panelW, sg.panelH - cr*2};
        SDL_RenderFillRect(g_sdlRdr, &pH);
        fillCircle(g_sdlRdr, sg.panelX + cr, sg.panelY + cr, cr, 28, 28, 30, 255);
        fillCircle(g_sdlRdr, sg.panelX + sg.panelW - cr, sg.panelY + cr, cr, 28, 28, 30, 255);
        fillCircle(g_sdlRdr, sg.panelX + cr, sg.panelY + sg.panelH - cr, cr, 28, 28, 30, 255);
        fillCircle(g_sdlRdr, sg.panelX + sg.panelW - cr, sg.panelY + sg.panelH - cr, cr, 28, 28, 30, 255);
        // �߿�
        SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 20);
        SDL_Rect borderH = {sg.panelX + cr, sg.panelY, sg.panelW - cr*2, sg.panelH};
        SDL_RenderDrawRect(g_sdlRdr, &borderH);
        SDL_Rect borderV = {sg.panelX, sg.panelY + cr, sg.panelW, sg.panelH - cr*2};
        SDL_RenderDrawRect(g_sdlRdr, &borderV);

        // title + close
        g_text.drawText(sg.panelX + U(20), sg.panelY + U(16), i18n::settingsTitle(), Tpt(16), 255, 255, 255);
        svgicon::draw(g_sdlRdr, "close", sg.closeCx, sg.closeCy, U(26), 255, 255, 255, 200);

        // toggle rows
        int toggleVals[SET_ROW_COUNT] = { g_cfg.hwDecode, g_cfg.volNorm,
            g_cfg.subAutoLoad, g_cfg.thumbCache, g_cfg.resume,
            g_cfg.nightMode, g_cfg.audioExclusive, g_cfg.motionInterp,
            g_cfg.hiQScale };
        const char* rowLabels[SET_ROW_COUNT] = {
            i18n::hwDecode(), i18n::volNorm(), i18n::subAutoLoad(),
            i18n::thumbCache(), i18n::resume(), i18n::nightMode(),
            i18n::exclusiveAudio(), i18n::motionInterp(), i18n::hiQScaling(),
        };
        for (int i = 0; i < SET_ROW_COUNT; ++i) {
            int ry = sg.rowY[i];
            bool on = (toggleVals[i] != 0);
            g_text.drawText(sg.panelX + U(20), ry + U(3), rowLabels[i], Tpt(13), on ? 230 : 170, on ? 230 : 170, on ? 230 : 170);

            // Switch
            SDL_Rect swRc = {sg.swX, ry, sg.swW, sg.swH};
            int swR = sg.swH / 2;
            if (on) {
                fillCircle(g_sdlRdr, sg.swX + swR, ry + swR, swR, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                fillCircle(g_sdlRdr, sg.swX + sg.swW - swR, ry + swR, swR, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                SDL_Rect mid = {sg.swX + swR, ry, sg.swW - sg.swH, sg.swH};
                SDL_RenderFillRect(g_sdlRdr, &mid);
            } else {
                fillCircle(g_sdlRdr, sg.swX + swR, ry + swR, swR, 80, 80, 80, 255);
                fillCircle(g_sdlRdr, sg.swX + sg.swW - swR, ry + swR, swR, 80, 80, 80, 255);
                SDL_Rect mid = {sg.swX + swR, ry, sg.swW - sg.swH, sg.swH};
                SDL_RenderFillRect(g_sdlRdr, &mid);
            }
            int thumbX = on ? sg.swX + sg.swW - sg.swH + U(2) : sg.swX + U(2);
            fillCircle(g_sdlRdr, thumbX + (sg.swH - U(4))/2, ry + U(10), (sg.swH - U(4))/2, 255, 255, 255, 255);
        }

        // playback mode row (ѡ��=��ɫ����, δѡ��=�������ޱ߿�)
        g_text.drawText(sg.panelX + U(20), sg.modeRowY + U(3), i18n::playbackMode(), Tpt(13), 200, 200, 200);
        const char* modes[] = { i18n::modeSingle(), i18n::modeLoop(), i18n::modeShuffle() };
        for (int i = 0; i < 3; ++i) {
            int lx = sg.swX - U(180) + i * (sg.chipW + U(6));
            bool sel = (g_cfg.playMode == i);
            if (sel) {
                int cr2 = sg.chipH / 2;
                SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                SDL_Rect lr = {lx + cr2, sg.chipY, sg.chipW - cr2*2, sg.chipH};
                SDL_RenderFillRect(g_sdlRdr, &lr);
                fillCircle(g_sdlRdr, lx + cr2, sg.chipY + cr2, cr2, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                fillCircle(g_sdlRdr, lx + sg.chipW - cr2, sg.chipY + cr2, cr2, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
            }
            int tw = g_text.measureText(modes[i], Tpt(11));
            g_text.drawText(lx + (sg.chipW - tw) / 2, sg.chipY + U(4), modes[i], Tpt(11),
                            sel ? 255 : 150, sel ? 255 : 150, sel ? 255 : 150);
        }

        // �����л��� (ͬ���: ѡ��=��ɫ����, δѡ��=������)
        g_text.drawText(sg.panelX + U(20), sg.langRowY + U(3), i18n::language(), Tpt(13), 200, 200, 200);
        const char* langLabels[] = { i18n::chinese(), i18n::english() };
        for (int i = 0; i < 2; ++i) {
            int lx = sg.langSegX + i * (sg.langSegW / 2);
            bool sel = (g_cfg.lang == i);
            int halfW = sg.langSegW / 2;
            if (sel) {
                int cr2 = sg.langSegH / 2;
                SDL_SetRenderDrawColor(g_sdlRdr, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                SDL_Rect lr = {lx + cr2, sg.langRowY, halfW - cr2*2, sg.langSegH};
                SDL_RenderFillRect(g_sdlRdr, &lr);
                fillCircle(g_sdlRdr, lx + cr2, sg.langRowY + cr2, cr2, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
                fillCircle(g_sdlRdr, lx + halfW - cr2, sg.langRowY + cr2, cr2, ui::ACCENT_R_, ui::ACCENT_G_, ui::ACCENT_B_, 255);
            }
            int tw2 = g_text.measureText(langLabels[i], Tpt(11));
            g_text.drawText(lx + (halfW - tw2) / 2, sg.langRowY + U(4), langLabels[i], Tpt(11),
                            sel ? 255 : 150, sel ? 255 : 150, sel ? 255 : 150);
        }
    }

    // --- toast notification��M32g ������ʽ: ����Բ����� + ���֣� ---
    if (g_ui.toastActive) {
        Uint32 elapsed = SDL_GetTicks() - g_ui.toastStart;
        if (elapsed > ui::TOAST_MS) {
            g_ui.toastActive = false;
        } else {
            float alpha = 1.0f;
            if (elapsed > ui::TOAST_MS - 300)
                alpha = 1.0f - (float)(elapsed - (ui::TOAST_MS - 300)) / 300.0f;
            Uint8 a = (Uint8)(alpha * 255);
            int tw = g_text.measureText(g_ui.toastMsg, Tpt(13));
            int bh = U(36), capR = bh / 2, padX = U(20);
            int bw = tw + padX * 2;
            int bx = w / 2 - bw / 2, by = U(60);
            SDL_SetRenderDrawColor(g_sdlRdr, 15, 15, 17, a);
            SDL_Rect mid = {bx + capR, by, bw - capR * 2, bh};
            SDL_RenderFillRect(g_sdlRdr, &mid);
            fillCircle(g_sdlRdr, bx + capR, by + bh / 2, bh / 2, 15, 15, 17, a);
            fillCircle(g_sdlRdr, bx + bw - capR, by + bh / 2, bh / 2, 15, 15, 17, a);
            // border (white.10)
            Uint8 ba = (Uint8)(26 * alpha);
            SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, ba);
            SDL_RenderDrawRect(g_sdlRdr, &mid);
            g_text.drawText(w / 2 - tw / 2, by + U(8), g_ui.toastMsg, Tpt(13), 255, 255, 255);
        }
    }

    // --- OSD ��Ϣ���ӣ��� I �л���8 ���Զ���ʧ�� ---
    if (g_ui.osdActive) {
        if (SDL_GetTicks() - g_ui.osdStart > 8000) {
            g_ui.osdActive = false;
        } else {
            std::string vfmt   = mpvStr("video-format");
            std::string vfps   = mpvStr("container-fps");
            if (vfps.empty()) vfps = mpvStr("estimated-vf-fps");
            std::string vbr    = formatBitrate(mpvStr("video-bitrate"));
            std::string afmt   = mpvStr("audio-codec-name");
            std::string asr    = mpvStr("audio-params/samplerate");
            std::string ach    = mpvStr("audio-params/channel-count");
            int vw = g_mpv->videoWidth(), vh = g_mpv->videoHeight();

            char line1[128] = {}, line2[64] = {}, line3[96] = {}, line4[64] = {};
            if (!vfmt.empty())
                std::snprintf(line1, sizeof(line1), "%s  %dx%d%s%s",
                    vfmt.c_str(), vw, vh,
                    vfps.empty() ? "" : " @ ", vfps.c_str());
            if (!vbr.empty()) std::snprintf(line2, sizeof(line2), "%s", vbr.c_str());
            if (!afmt.empty()) {
                int sr = std::atoi(asr.c_str());
                std::snprintf(line3, sizeof(line3), "%s %s Hz %sch",
                    afmt.c_str(),
                    sr > 0 ? asr.c_str() : "?",
                    ach.empty() ? "?" : ach.c_str());
            }
            const char* hwPath = g_mpv->hwdecCurrent();
            if (hwPath && hwPath[0] && std::strcmp(hwPath, "no") != 0)
                std::snprintf(line4, sizeof(line4), "hwdec: %s%s", hwPath,
                              g_mpv->hwdecRetryCount() > 0 ? " (fallback)" : "");

            // ���ߴ�������
            int lines = 0;
            if (line1[0]) ++lines;
            if (line2[0]) ++lines;
            if (line3[0]) ++lines;
            if (line4[0]) ++lines;
            if (lines > 0) {
                int padX = U(14), padY = U(10), lineH = U(22);
                int boxW = U(340), boxH = padY * 2 + lines * lineH;
                int boxX = U(16), boxY = curTopH() + U(12);
                SDL_Rect bg = {boxX, boxY, boxW, boxH};
                SDL_SetRenderDrawColor(g_sdlRdr, 11, 11, 11, 200);
                SDL_RenderFillRect(g_sdlRdr, &bg);
                SDL_SetRenderDrawColor(g_sdlRdr, 255, 255, 255, 30);
                SDL_RenderDrawRect(g_sdlRdr, &bg);

                int ty = boxY + padY;
                if (line1[0]) { g_text.drawText(boxX + padX, ty, line1, Tpt(12), 255, 255, 255); ty += lineH; }
                if (line2[0]) { g_text.drawText(boxX + padX, ty, line2, Tpt(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B); ty += lineH; }
                if (line3[0]) { g_text.drawText(boxX + padX, ty, line3, Tpt(12), ui::TIME_TEXT_R, ui::TIME_TEXT_G, ui::TIME_TEXT_B); ty += lineH; }
                if (line4[0]) { g_text.drawText(boxX + padX, ty, line4, Tpt(12), 0, 200, 120); }
            }
        }
    }

    overlayPresent();
}