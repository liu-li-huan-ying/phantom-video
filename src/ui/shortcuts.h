#pragma once
// Keyboard shortcut handling extracted from wndproc.cpp.
// Returns true if the key was consumed.

#include <windows.h>

bool handleKeyboard(HWND hwnd, WPARAM wp, LPARAM lp);
