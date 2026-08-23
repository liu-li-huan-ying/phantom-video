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
constexpr float TOPBAR_A0 = 0.55f;  // 顶栏渐变顶部黑
constexpr float CTRLBAR_A0 = 0.62f; // 底栏渐变底部黑
constexpr float CTRLBAR_A1 = 0.25f; // 底栏渐变 75% 处

// ---- 尺寸/圆角 ----
constexpr int RADIUS_WINDOW = 14;
constexpr int RADIUS_PANEL = 10;
constexpr int RADIUS_BTN = 8;
constexpr int RADIUS_CHIP = 8;
constexpr int RADIUS_THUMB = 7;

constexpr int ICONBTN_SIZE = 34;   // 顶部/底部标准图标钮
constexpr int PLAYBTN_SIZE = 42;   // 播放大钮
constexpr int TOPBAR_H = 52;
constexpr int SEEKBAR_TRACK_H = 4;
constexpr int SEEKBAR_TRACK_H_HOVER = 6;
constexpr int SEEKTHUMB_D = 13;
constexpr int VOLSIDER_W = 80;

// 控制栏自动隐藏
constexpr Uint32 CTRLBAR_HIDE_MS = 2500;
// Toast 显示时长
constexpr Uint32 TOAST_MS = 1800;

} // namespace ui
