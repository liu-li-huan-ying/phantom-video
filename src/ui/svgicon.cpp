// SVG path rasterizer icon system.
// Material Design 24x24 filled icons, parsed from the original design HTML.
// Scanline nonzero-winding fill with 3x supersampled AA.
// Arcs flattened to cubic Beziers, quadratic Beziers tessellated.

#include "ui/svgicon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace svgicon {

// Material Design filled icons (24x24 viewBox, taken from design HTML)
static const Def kDefs[] = {
    { "play",    "M8 5v14l11-7z" },
    { "pause",   "M6 4h4v16H6zM14 4h4v16h-4z" },
    { "prev",    "M6 6h2v12H6zm3.5 6 8.5 6V6z" },
    { "next",    "M16 6h2v12h-2zM6 18l8.5-6L6 6z" },
    { "volume",  "M16.5 12c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02zM3 9v6h4l5 5V4L7 9H3zm16.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02zM14 3.23v2.06c2.89.86 5 3.54 5 6.71s-2.11 5.85-5 6.71v2.06c4.01-.91 7-4.49 7-8.77s-2.99-7.86-7-8.77z" },
    { "mute",    "M3 10v4h4l5 5V5L7 10H3zm13.5 2A4.5 4.5 0 0 0 14 7.97v8.06A4.5 4.5 0 0 0 16.5 12z" },
    { "full",    "M7 14H5v5h5v-2H7v-3zm-2-4h2V7h3V5H5v5zm12 7h-3v2h5v-5h-2v3zM14 5v2h3v3h2V5h-5z" },
    { "exitfull","M5 16h3v3h2v-5H5v2zm3-8H5v2h5V5H8v3zm6 11h2v-3h3v-2h-5v5zm2-11V5h-2v5h5V8h-3z" },
    { "close",   "M19 6.41 17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59z" },
    { "delete",  "M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z" },
    { "minimize","M19 13H5v-2h14v2z" },
    { "maximize","M7 14H5v5h5v-2H7v-3zm-2-4h2V7h3V5H5v5zm12 7h-3v2h5v-5h-2v3zM14 5v2h3v3h2V5h-5z" },
    { "list",    "M3 13h12v-2H3v2zm0-4h18V7H3v2zm0 8h18v-2H3v2z" },
    { "camera",  "M9 3 7.17 5H4a2 2 0 0 0-2 2v11a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2h-3.17L15 3zm3 13a5 5 0 1 1 0-10 5 5 0 0 1 0 10z" },
    { "pip",     "M19 11h-8v6h8v-6zm4 8V4.98C23 3.88 22.1 3 21 3H3c-1.1 0-2 .88-2 1.98V19c0 1.1.9 2 2 2h18c1.1 0 2-.9 2-2zm-2 .02H3V4.97h18v14.05z" },
    { "cc",      "M20 4H4a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V6a2 2 0 0 0-2-2zM4 12h4v2H4v-2zm0 4h8v2H4v-2zm14 0h2v2h-2v-2zm0-4h2v2h-2v-2zm-4-4h6v2h-6V8zm0 4h6v2h-6v-2zM4 8h10v2H4V8z" },
    { "gear",    "M19.43 12.98a7.93 7.93 0 0 0 .14-1.98c0-.7-.08-1.38-.14-2.04l2.05-1.6a.5.5 0 0 0 .12-.64l-1.94-3.36a.5.5 0 0 0-.61-.22l-2.42.97a7.7 7.7 0 0 0-1.69-.98L14.5 2.6a.5.5 0 0 0-.5-.42h-3.88a.5.5 0 0 0-.5.42l.37 2.71c-.62.34-1.19.74-1.69.98l-2.42-.97a.5.5 0 0 0-.61.22L1.9 6.72a.5.5 0 0 0 .12.64l2.05 1.6c-.06.66-.14 1.34-.14 2.04 0 .7.08 1.38.14 1.98l-2.05 1.6a.5.5 0 0 0-.12.64l1.94 3.36a.5.5 0 0 0 .61.22l2.42-.97c.5.24 1.07.64 1.69.98l-.37 2.71a.5.5 0 0 0 .5.42h3.88a.5.5 0 0 0 .5-.42l-.37-2.71c.62-.34 1.19-.74 1.69-.98l2.42.97a.5.5 0 0 0 .61-.22l1.94-3.36a.5.5 0 0 0-.12-.64zM12 15.5A3.5 3.5 0 1 1 12 8.5a3.5 3.5 0 0 1 0 7z" },
};
static const int kDefCount = (int)(sizeof(kDefs) / sizeof(kDefs[0]));

const Def* defs() { return kDefs; }

const Def* find(const char* id) {
    if (!id) return nullptr;
    for (int i = 0; i < kDefCount; ++i)
        if (std::strcmp(kDefs[i].id, id) == 0) return &kDefs[i];
    return nullptr;
}

// ---------------- Path parser ----------------
struct Vec2 { float x, y; };

static bool isCmdChar(char c) {
    return c && std::strchr("MmLlHhVvZzAaCcSsQqTt", c) != nullptr;
}

struct Tokenizer {
    const char* p;
    explicit Tokenizer(const char* s) : p(s) {}
    int num(double* out, int maxN) {
        int n = 0;
        while (n < maxN) {
            char* end = nullptr;
            double v = std::strtod(p, &end);
            if (end == p) break;
            out[n++] = v;
            p = end;
            while (*p == ' ' || *p == ',') ++p;
        }
        return n;
    }
};

static void arcToCubics(float curX, float curY, float rx, float ry, float rotDeg,
                        bool largeArc, bool sweep, float endX, float endY,
                        std::vector<Vec2>& out) {
    const float PI = 3.14159265f;
    if (rx == 0 || ry == 0) { out.push_back({ endX, endY }); return; }
    rx = std::fabs(rx); ry = std::fabs(ry);
    float phi = rotDeg * PI / 180.f;
    float cosP = std::cos(phi), sinP = std::sin(phi);
    float dx = (curX - endX) * 0.5f, dy = (curY - endY) * 0.5f;
    float x1p = cosP * dx + sinP * dy, y1p = -sinP * dx + cosP * dy;
    float lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { float s = std::sqrt(lam); rx *= s; ry *= s; }
    float sign = (largeArc != sweep) ? 1.f : -1.f;
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    if (den == 0) { out.push_back({ endX, endY }); return; }
    float co = sign * std::sqrt(num / den);
    float cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    float cx = cosP * cxp - sinP * cyp + (curX + endX) * 0.5f;
    float cy = sinP * cxp + cosP * cyp + (curY + endY) * 0.5f;

    auto angOf = [](float ux, float uy, float vx, float vy) {
        float d = ux * vx + uy * vy;
        float len = std::sqrt(ux * ux + uy * uy) * std::sqrt(vx * vx + vy * vy);
        float a = std::acos(std::max(-1.f, std::min(1.f, d / len)));
        if (ux * vy - uy * vx < 0) a = -a;
        return a;
    };
    float th1 = angOf(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    float dth = angOf((x1p - cxp) / rx, (y1p - cyp) / ry,
                      (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && dth > 0) dth -= 2 * PI;
    if (sweep && dth < 0) dth += 2 * PI;
    int segs = (int)std::ceil(std::fabs(dth) / (PI / 2));
    float delta = dth / segs;
    float t = 4.f / 3.f * std::tan(delta * 0.25f);
    float th = th1;
    for (int i = 0; i < segs; ++i) {
        float th2 = th + delta;
        float px0 = out.empty() ? curX : out.back().x;
        float py0 = out.empty() ? curY : out.back().y;
        float c1x = cx + cosP * rx * std::cos(th) - sinP * ry * std::sin(th);
        float c1y = cy + sinP * rx * std::cos(th) + cosP * ry * std::sin(th);
        float dx2 = cosP * rx * std::cos(th + delta) - sinP * ry * std::sin(th + delta);
        float dy2 = sinP * rx * std::cos(th + delta) + cosP * ry * std::sin(th + delta);
        float c2x = dx2 + t * (-cosP * rx * std::sin(th + delta) - sinP * ry * std::cos(th + delta));
        float c2y = dy2 + t * (-sinP * rx * std::sin(th + delta) + cosP * ry * std::cos(th + delta));
        for (int s = 1; s <= 8; ++s) {
            float u = s / 8.f, v = 1 - u;
            out.push_back({ v*v*v*px0 + 3*v*v*u*c1x + 3*v*u*u*c2x + u*u*u*dx2,
                            v*v*v*py0 + 3*v*v*u*c1y + 3*v*u*u*c2y + u*u*u*dy2 });
        }
        th = th2;
    }
}

// Parse SVG path d attribute into polygon subpaths (units = 24 viewBox)
static std::vector<std::vector<Vec2>> parsePath(const char* d) {
    std::vector<std::vector<Vec2>> polys;
    std::vector<Vec2> cur;
    Vec2 pt{ 0, 0 }, startPt{ 0, 0 };
    char cmd = 0;
    Tokenizer tk(d ? d : "");
    while (*tk.p) {
        while (*tk.p == ' ' || *tk.p == ',') ++tk.p;
        if (!*tk.p) break;
        char c = *tk.p;
        if (isCmdChar(c)) { cmd = c; ++tk.p; while (*tk.p == ' ') ++tk.p; }
        if (!cmd) break;
        double a[7] = { 0, 0, 0, 0, 0, 0, 0 };
        switch (cmd) {
        case 'Z': case 'z':
            if (!cur.empty()) { cur.push_back(startPt); polys.push_back(cur); cur.clear(); }
            pt = startPt;
            continue;
        case 'M': case 'm': {
            if (tk.num(a, 2) < 2) goto done;
            if (cmd == 'm') { a[0] += pt.x; a[1] += pt.y; }
            if (!cur.empty()) { cur.push_back(startPt); polys.push_back(cur); cur.clear(); }
            pt = startPt = Vec2{ (float)a[0], (float)a[1] };
            cur.push_back(pt);
            cmd = (cmd == 'm') ? 'l' : 'L';
            break; }
        case 'L': case 'l': {
            if (tk.num(a, 2) < 2) goto done;
            if (cmd == 'l') { a[0] += pt.x; a[1] += pt.y; }
            pt = Vec2{ (float)a[0], (float)a[1] };
            cur.push_back(pt);
            break; }
        case 'H': case 'h': {
            if (tk.num(a, 1) < 1) goto done;
            if (cmd == 'h') a[0] += pt.x;
            pt.x = (float)a[0]; cur.push_back(pt);
            break; }
        case 'V': case 'v': {
            if (tk.num(a, 1) < 1) goto done;
            if (cmd == 'v') a[0] += pt.y;
            pt.y = (float)a[0]; cur.push_back(pt);
            break; }
        case 'A': case 'a': {
            if (tk.num(a, 7) < 7) goto done;
            float ex = (float)a[5], ey = (float)a[6];
            if (cmd == 'a') { ex += pt.x; ey += pt.y; }
            arcToCubics(pt.x, pt.y, (float)a[0], (float)a[1], (float)a[2],
                        a[3] != 0, a[4] != 0, ex, ey, cur);
            pt = Vec2{ ex, ey };
            break; }
        case 'Q': case 'q': {
            if (tk.num(a, 4) < 4) goto done;
            float cx = (float)a[0], cy = (float)a[1];
            float ex = (float)a[2], ey = (float)a[3];
            if (cmd == 'q') { cx += pt.x; cy += pt.y; ex += pt.x; ey += pt.y; }
            for (int s = 1; s <= 8; ++s) {
                float u = s / 8.f, v = 1 - u;
                float bx = v*v*pt.x + 2*v*u*cx + u*u*ex;
                float by = v*v*pt.y + 2*v*u*cy + u*u*ey;
                cur.push_back({ bx, by });
            }
            pt = Vec2{ ex, ey };
            break; }
        default:
            ++tk.p;
            break;
        }
    }
done:
    if (!cur.empty()) { cur.push_back(startPt); polys.push_back(cur); }
    return polys;
}

// ---------------- Scanline rasterizer (nonzero winding, 3x supersampled AA) ----------------
static SDL_Texture* rasterize(SDL_Renderer* r,
                              const std::vector<std::vector<Vec2>>& polys,
                              int size, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    const int SS = 3;
    int W = size * SS;
    if (W <= 0) return nullptr;
    const float k = (float)W / 24.f;

    struct Edge { float x0, y0, x1, y1; };
    std::vector<Edge> edges;
    for (auto& poly : polys) {
        for (size_t i = 1; i < poly.size(); ++i) {
            const Vec2& p0 = poly[i - 1];
            const Vec2& p1 = poly[i];
            if (p0.y != p1.y)
                edges.push_back({ p0.x * k, p0.y * k, p1.x * k, p1.y * k });
        }
    }
    std::vector<Uint8> cov((size_t)W * W, 0);
    std::vector<float> xs;
    xs.reserve(32);
    for (int py = 0; py < W; ++py) {
        float yc = py + 0.5f;
        xs.clear();
        for (auto& e : edges) {
            float y0 = e.y0, y1 = e.y1;
            if ((yc >= y0 && yc < y1) || (yc >= y1 && yc < y0)) {
                float t = (yc - y0) / (y1 - y0);
                xs.push_back(e.x0 + t * (e.x1 - e.x0));
            }
        }
        if (xs.size() < 2) continue;
        std::sort(xs.begin(), xs.end());
        Uint8* row = &cov[(size_t)py * W];
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            int xa = (int)std::ceil(xs[i] - 0.5f);
            int xb = (int)std::floor(xs[i + 1] - 0.5f);
            if (xa < 0) xa = 0;
            if (xb > W - 1) xb = W - 1;
            for (int x = xa; x <= xb; ++x) row[x] = 255;
        }
    }

    std::vector<Uint32> pixels((size_t)size * size, 0);
    float inv = 1.f / (SS * SS * 255) * ca;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int sum = 0;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx)
                    sum += cov[(size_t)(y * SS + sy) * W + (x * SS + sx)];
            Uint8 a = (Uint8)(sum * inv);
            pixels[(size_t)y * size + x] =
                ((Uint32)a << 24) | ((Uint32)cb << 16) | ((Uint32)cg << 8) | cr;
        }
    }

    SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, size, size);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(tex, nullptr, pixels.data(), size * sizeof(Uint32));
    return tex;
}

// ---------------- Cache and draw ----------------
struct CacheKey {
    std::string id;
    int size;
    Uint32 rgb;
    bool operator==(const CacheKey& o) const {
        return size == o.size && rgb == o.rgb && id == o.id;
    }
};
struct KeyHash {
    size_t operator()(const CacheKey& k) const {
        return std::hash<std::string>()(k.id) ^ (k.size * 2654435761u) ^ k.rgb;
    }
};
static std::unordered_map<CacheKey, SDL_Texture*, KeyHash> g_cache;



void draw(SDL_Renderer* r, const char* id, int cx, int cy, int size,
          Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (ca == 0 || size <= 0) return;
    const Def* def = find(id);
    if (!def) return;
    Uint32 rgb = ((Uint32)cb << 16) | ((Uint32)cg << 8) | cr;
    CacheKey key{ id, size, rgb };
    auto it = g_cache.find(key);
    SDL_Texture* tex = nullptr;
    if (it != g_cache.end()) {
        tex = it->second;
    } else {
        tex = rasterize(r, parsePath(def->path), size, cr, cg, cb, 255);
        if (!tex) return;
        g_cache[key] = tex;
    }
    SDL_SetTextureAlphaMod(tex, ca);
    SDL_Rect dst{ cx - size / 2, cy - size / 2, size, size };
    SDL_RenderCopy(r, tex, nullptr, &dst);
}

void shutdown() {
    for (auto& kv : g_cache) SDL_DestroyTexture(kv.second);
    g_cache.clear();
}

} // namespace svgicon
