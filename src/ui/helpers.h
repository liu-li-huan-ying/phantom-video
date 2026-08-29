#pragma once
// Pure utility functions with no UI state dependencies.
// Extracted from main.cpp to reduce monolithic file size.

#include <string>
#include <cstddef>

// Format time as HH:MM:SS or MM:SS
void formatTime(char* buf, size_t n, double sec);

// UTF-8 <-> UTF-16 conversion (Windows Wide char)
std::wstring Utf8ToWide(const std::string& u8);
std::string  WideToUtf8(const std::wstring& ws);

// Safely extract filename from UTF-8 path (avoids ANSI misinterpretation)
std::string fileNameOf(const std::string& utf8path);
