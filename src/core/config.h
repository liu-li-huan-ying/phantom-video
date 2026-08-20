#pragma once
#include <map>
#include <string>

struct AppConfig {
    float volume = 0.8f;
    std::string lastFile;
    std::map<std::string, double> history;
    int playMode = 1;  // PlayMode: 0=Single 1=Loop 2=Shuffle
};

std::string configPath();
bool loadConfig(const std::string& path, AppConfig& out);
bool saveConfig(const std::string& path, const AppConfig& cfg);