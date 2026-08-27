#pragma once
// SVG path 光栅化图标系统（Lucide 图标库 ISC License）。
// 解析器支持: M m L l H h V v Z z A a，圆弧转三次贝塞尔后扁平化。
// 支持 stroke 风格：扫描线填充后做形态学膨胀模拟描边。

#include <SDL.h>

namespace svgicon {

struct Def { const char* id; const char* path; };
const Def* defs();
const Def* find(const char* id);

void draw(SDL_Renderer* r, const char* id, int cx, int cy, int size,
          Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca);

void shutdown();

} // namespace svgicon
