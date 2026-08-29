#include "ui/helpers.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>

void formatTime(char* buf, size_t n, double sec) {
    int s = (int)(sec + 0.5);
    if (s < 0) s = 0;
    int h = s / 3600, m = (s % 3600) / 60, ss = s % 60;
    if (h > 0) std::snprintf(buf, n, "%d:%02d:%02d", h, m, ss);
    else       std::snprintf(buf, n, "%02d:%02d", m, ss);
}

std::wstring Utf8ToWide(const std::string& u8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& ws) {
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string fileNameOf(const std::string& utf8path) {
    namespace fs = std::filesystem;
    try {
        fs::path p(Utf8ToWide(utf8path));
        return WideToUtf8(p.filename().wstring());
    } catch (...) { return utf8path; }
}
