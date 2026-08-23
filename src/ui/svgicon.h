#pragma once
// M32a: SVG path 光栅化图标系统。
// 直接使用《播放器效果图.html》中的 Material Design 24x24 原始 path 数据，
// 运行时解析 → 多边形扁平化 → 扫描线非零环绕光栅化(2x超采样抗锯齿) → 纹理缓存。
// 保证图标形状与设计稿一比一。

#include <SDL.h>

namespace svgicon {

// 效果图中的全部图标 path（24x24 viewBox，逐字取自 HTML）
struct Def { const char* id; const char* path; };
const Def* defs();
const Def* find(const char* id);

// 将 id 对应图标以指定颜色绘制到 (cx,cy) 为中心、边长 size 的区域（带缓存）。
void draw(SDL_Renderer* r, const char* id, int cx, int cy, int size,
          Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca);

void shutdown();

} // namespace svgicon
