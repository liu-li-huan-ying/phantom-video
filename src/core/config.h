#pragma once
#include <map>
#include <string>

// M36: 历史条目元数据 (继续观看行需要时长+时间戳排序)
struct HistoryEntry {
    double pos = 0;            // 上次观看位置(秒)
    double dur = 0;            // 时长(秒), 0=未知
    long long lastPlayed = 0;  // 最后播放 Unix 时间戳(秒)
};

struct AppConfig {
    float volume = 0.8f;
    float speed = 1.0f;
    std::string lastFile;
    std::map<std::string, HistoryEntry> history;
    int playMode = 1;  // PlayMode: 0=Single 1=Loop 2=Shuffle
    int resume = 0;    // 0=打开时从头播放 1=从上次位置续播
    int subAutoLoad = 1;   // 字幕自动加载
    int thumbCache = 1;    // 缩略图磁盘缓存
    float subScale = 1.0f; // 字幕缩放系数
    int hwDecode = 1;      // 硬件解码
    int volNorm = 0;       // 音量标准化(loudnorm)
    int nightMode = 0;     // 夜间模式(acompressor 动态压缩)
    int audioExclusive = 0;// WASAPI 独占输出
    int motionInterp = 0;  // 运动插值(display-resample + oversample)
    int hiQScale = 0;      // 高质量缩放(ewa_lanczossharp, GPU 开销较高)
    int lang = 0;           // 语言: 0=中文 1=English
    // 窗口位置（物理像素；x==INVALID_POS 表示未记忆）
    static const int INVALID_POS = -32001;
    int posX = INVALID_POS, posY = INVALID_POS;
    int posW = 0, posH = 0;
};

std::string configPath();
std::string exeDir();   // exe 所在目录（带尾部分隔符），用于定位随包资源
bool loadConfig(const std::string& path, AppConfig& out);
bool saveConfig(const std::string& path, const AppConfig& cfg);