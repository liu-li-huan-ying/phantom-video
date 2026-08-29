#pragma once
// Win32 WndProc for parent window.
// Handles keyboard, mouse, window management messages.
// Extracted from main.cpp to reduce monolithic file size.

#include <windows.h>

// Parent window procedure
LRESULT CALLBACK parentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
