#pragma once
// Menu and popup click handling extracted from wndproc.cpp.
// Each function handles clicks for a specific popup menu and returns true if the click was consumed.

#include <windows.h>

// Handle clicks on popup menus (speed/quality/EQ/image/sub/audio/chapter).
// Returns true if the click was consumed (caller should return 0).
bool handleMenuClicks(HWND hwnd, int mx, int my);

// Handle clicks on the settings panel.
// Returns true if the click was consumed.
bool handleSettingsClick(HWND hwnd, int mx, int my);

// Handle clicks on control bar buttons (prev/play/next/sub/speed/quality/audio/chapter/ab/eq/img/set/full).
// Returns true if the click was consumed.
bool handleControlBarClick(HWND hwnd, int mx, int my);

// Handle welcome page clicks (hero buttons, continue watching, grid).
// Returns true if the click was consumed.
bool handleWelcomeClick(HWND hwnd, int mx, int my);
