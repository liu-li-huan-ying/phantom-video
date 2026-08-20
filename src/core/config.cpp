#include "core/config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

std::string configPath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring ws(buf, n);
    std::size_t slash = ws.find_last_of(L"\\/");
    if (slash != std::wstring::npos) ws.resize(slash + 1);
    ws += L"vplayer.ini";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
#else
    return "vplayer.ini";
#endif
}

bool loadConfig(const std::string& path, AppConfig& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("volume=", 0) == 0) {
            out.volume = (float)std::atof(line.c_str() + 7);
        } else if (line.rfind("last=", 0) == 0) {
            out.lastFile = line.substr(5);
        } else if (line.rfind("playmode=", 0) == 0) {
            int m = std::atoi(line.c_str() + 9);
            if (m >= 0 && m <= 2) out.playMode = m;
        } else if (line.rfind("resume=", 0) == 0) {
            out.resume = std::atoi(line.c_str() + 7) != 0 ? 1 : 0;
        } else if (line.rfind("hist=", 0) == 0) {
            std::size_t tab = line.find('\t', 5);
            if (tab != std::string::npos) {
                std::string p = line.substr(5, tab - 5);
                double pos = std::atof(line.c_str() + tab + 1);
                if (!p.empty()) out.history[p] = pos;
            }
        }
    }
    return true;
}

bool saveConfig(const std::string& path, const AppConfig& cfg) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "# vplayer config (UTF-8)\n";
    out << "volume=" << cfg.volume << "\n";
    out << "last=" << cfg.lastFile << "\n";
    out << "playmode=" << cfg.playMode << "\n";
    out << "resume=" << cfg.resume << "\n";
    for (const auto& kv : cfg.history) {
        out << "hist=" << kv.first << "\t" << kv.second << "\n";
    }
    return true;
}