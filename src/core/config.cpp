#include "core/config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

std::string exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring ws(buf, n);
    std::size_t slash = ws.find_last_of(L"\\/");
    if (slash != std::wstring::npos) ws.resize(slash + 1);
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
#else
    return "";
#endif
}

std::string configPath() {
#ifdef _WIN32
    return exeDir() + "phantom.ini";
#else
    return "phantom.ini";
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
        } else if (line.rfind("speed=", 0) == 0) {
            float s = (float)std::atof(line.c_str() + 6);
            if (s >= 0.25f && s <= 4.0f) out.speed = s;
        } else if (line.rfind("subautoload=", 0) == 0) {
            out.subAutoLoad = std::atoi(line.c_str() + 12) != 0 ? 1 : 0;
        } else if (line.rfind("thumbcache=", 0) == 0) {
            out.thumbCache = std::atoi(line.c_str() + 11) != 0 ? 1 : 0;
        } else if (line.rfind("hwdecode=", 0) == 0) {
            out.hwDecode = std::atoi(line.c_str() + 9) != 0 ? 1 : 0;
        } else if (line.rfind("volnorm=", 0) == 0) {
            out.volNorm = std::atoi(line.c_str() + 8) != 0 ? 1 : 0;
        } else if (line.rfind("nightmode=", 0) == 0) {
            out.nightMode = std::atoi(line.c_str() + 10) != 0 ? 1 : 0;
        } else if (line.rfind("audioexcl=", 0) == 0) {
            out.audioExclusive = std::atoi(line.c_str() + 10) != 0 ? 1 : 0;
        } else if (line.rfind("motioninterp=", 0) == 0) {
            out.motionInterp = std::atoi(line.c_str() + 13) != 0 ? 1 : 0;
        } else if (line.rfind("hiqscale=", 0) == 0) {
            out.hiQScale = std::atoi(line.c_str() + 9) != 0 ? 1 : 0;
        } else if (line.rfind("subscale=", 0) == 0) {
            float s = (float)std::atof(line.c_str() + 9);
            if (s >= 0.5f && s <= 3.0f) out.subScale = s;
        } else if (line.rfind("lang=", 0) == 0) {
            int l = std::atoi(line.c_str() + 5);
            if (l >= 0 && l <= 1) out.lang = l;
        } else if (line.rfind("pos=", 0) == 0) {
            int x, y, w, h;
            if (sscanf(line.c_str() + 4, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 &&
                x > -32000 && y > -32000 && w > 200 && h > 150 &&
                w < 20000 && h < 20000) {
                out.posX = x; out.posY = y; out.posW = w; out.posH = h;
            }
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
    out << "# phantom config (UTF-8)\n";
    out << "volume=" << cfg.volume << "\n";
    out << "speed=" << cfg.speed << "\n";
    out << "last=" << cfg.lastFile << "\n";
    out << "playmode=" << cfg.playMode << "\n";
    out << "resume=" << cfg.resume << "\n";
    out << "subautoload=" << cfg.subAutoLoad << "\n";
    out << "thumbcache=" << cfg.thumbCache << "\n";
    out << "hwdecode=" << cfg.hwDecode << "\n";
    out << "volnorm=" << cfg.volNorm << "\n";
    out << "nightmode=" << cfg.nightMode << "\n";
    out << "audioexcl=" << cfg.audioExclusive << "\n";
    out << "motioninterp=" << cfg.motionInterp << "\n";
    out << "hiqscale=" << cfg.hiQScale << "\n";
    out << "subscale=" << cfg.subScale << "\n";
    out << "lang=" << cfg.lang << "\n";
    if (cfg.posX != AppConfig::INVALID_POS && cfg.posW > 0)
        out << "pos=" << cfg.posX << "," << cfg.posY << "," << cfg.posW << "," << cfg.posH << "\n";
    for (const auto& kv : cfg.history) {
        out << "hist=" << kv.first << "\t" << kv.second << "\n";
    }
    return true;
}