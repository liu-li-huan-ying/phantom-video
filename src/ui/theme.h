#pragma once
// M32a: 设计令牌 —— 与《播放器效果图.html》CSS 变量一比一对应。
// 所有 UI 绘制从此处取值，禁止在调用点硬编码颜色/尺寸。

namespace ui {

// ---- 色板 ----
constexpr float BG_R = 0x0b, BG_G = 0x0b, BG_B = 0x0b;          // --bg #0b0b0b
constexpr float PANEL_R = 0x15, PANEL_G = 0x15, PANEL_B = 0x15; // --panel #151515
constexpr float TEXT_R = 255, TEXT_G = 255, TEXT_B = 255;       // --text
constexpr float TEXT2_R = 0xa1, TEXT2_G = 0xa1, TEXT2_B = 0xa6; // --text2 #a1a1a6
constexpr float ACCENT_R = 0x25, ACCENT_G = 0x63, ACCENT_B = 0xeb; // --accent #2563eb
constexpr float ACCENT2_R = 0x3b, ACCENT2_G = 0x82, ACCENT2_B = 0xf6; // --accent2

// 白色系透明度（乘到 RGB=255）
constexpr float BORDER_A = 0.10f;   // --border
constexpr float HOVER_A = 0.08f;    // --hover
constexpr float ACTIVE_A = 0.14f;   // --active

// ---- 渐变 alpha ----
constexpr Uint8 TOPBAR_A0 = 210;    // 顶栏渐变顶部 (82%)
constexpr Uint8 CTRLBAR_A0 = 220;   // 控制栏渐变顶部 (86%)
constexpr Uint8 CTRLBAR_A1 = 50;    // 控制栏渐变底部 (20%)

// ---- 按钮/文字颜色 (高对比度, 深色背景上清晰可辨) ----
constexpr Uint8 ICON_BRIGHT = 255;  // 主要图标 (纯白)
constexpr Uint8 TEXT_DIM = 235;     // 文字按钮 (亮灰白)
constexpr Uint8 ICON_DIM = 220;     // 次要图标 (亮灰)
constexpr Uint8 TIME_TEXT_R = 210, TIME_TEXT_G = 210, TIME_TEXT_B = 216; // 时间/次要文字

// ---- seekbar 颜色 ----
constexpr Uint8 SEEK_TRACK_A = 70;  // 轨道背景
constexpr Uint8 SEEK_BUF_A = 90;    // 缓冲区

// ---- 尺寸/圆角 ----
constexpr int RADIUS_WINDOW = 14;
constexpr int RADIUS_PANEL = 10;
constexpr int RADIUS_BTN = 8;
constexpr int RADIUS_CHIP = 8;
constexpr int RADIUS_THUMB = 7;

// 控件最小尺寸 (小窗口用这些值, 大窗口按比例缩放)
constexpr int ICONBTN_MIN = 34;
constexpr int PLAYBTN_MIN = 42;
constexpr int TOPBAR_H_MIN = 52;
constexpr int CTRLBAR_H_MIN = 80;
constexpr int SEEKBAR_H_MIN = 6;
constexpr int SEEKBAR_H_HOVER_MIN = 10;
constexpr int SEEKTHUMB_D = 13;
constexpr int VOLSIDER_W = 80;
constexpr int MARGIN_MIN = 16;

// 控制栏自动隐藏
constexpr Uint32 CTRLBAR_HIDE_MS = 2500;
// Toast 显示时长
constexpr Uint32 TOAST_MS = 1800;

} // namespace ui
