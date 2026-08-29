#pragma once
// Win32 file/folder/URL dialog functions.
// Extracted from main.cpp to reduce monolithic file size.

#include <string>
#include <windows.h>

// Open file dialog (video files)
std::string openFileDialog(HWND hwnd);

// Open subtitle file dialog
std::string openSubtitleDialog(HWND hwnd);

// URL input dialog (modal, pure Win32)
std::string openUrlDialog(HWND hwnd);

// Folder selection dialog (SHBrowseForFolderW)
std::string openFolderDialog(HWND hwnd);
