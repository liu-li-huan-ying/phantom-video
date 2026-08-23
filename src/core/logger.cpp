#include "core/logger.h"

#include <chrono>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

Logger& Logger::instance() {
    static Logger s;
    return s;
}

Logger::~Logger() {
    if (file_) fclose(file_);
}

std::string Logger::getExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    auto pos = path.find_last_of("\\/");
    return pos != std::string::npos ? path.substr(0, pos) : ".";
#else
    char buf[4096] = {0};
    readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string path(buf);
    auto pos = path.find_last_of('/');
    return pos != std::string::npos ? path.substr(0, pos) : ".";
#endif
}

static void makeDir(const char* path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

void Logger::cleanupOldLogs(int keepDays) {
    if (keepDays <= 0) return;

#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    std::string pattern = logDir_ + "\\vplayer_*.log";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME stNow;
    GetLocalTime(&stNow);

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        if (name.size() < 20) continue;
        if (name.substr(0, 8) != "vplayer_") continue;
        std::string dateStr = name.substr(8, 10);
        int y, m, d;
        if (sscanf(dateStr.c_str(), "%d-%d-%d", &y, &m, &d) != 3) continue;

        SYSTEMTIME stLog = {};
        stLog.wYear = (WORD)y;
        stLog.wMonth = (WORD)m;
        stLog.wDay = (WORD)d;
        stLog.wHour = 12;

        FILETIME ftLog, ftNow;
        SystemTimeToFileTime(&stLog, &ftLog);
        GetLocalTime(&stNow);
        stNow.wHour = 12; stNow.wMinute = 0; stNow.wSecond = 0; stNow.wMilliseconds = 0;
        SystemTimeToFileTime(&stNow, &ftNow);

        // diff in days
        ULARGE_INTEGER ulLog = {{ftLog.dwLowDateTime, ftLog.dwHighDateTime}};
        ULARGE_INTEGER ulNow = {{ftNow.dwLowDateTime, ftNow.dwHighDateTime}};
        long long diffDays = (long long)(ulNow.QuadPart - ulLog.QuadPart) / (10000000LL * 86400);

        if (diffDays > keepDays) {
            std::string full = logDir_ + "\\" + name;
            DeleteFileA(full.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
}

void Logger::init(const char* appName, int keepDays) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) { fclose(file_); file_ = nullptr; }

    logDir_ = getExeDir() + "\\logs";
    makeDir(logDir_.c_str());

    cleanupOldLogs(keepDays);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char filename[512];
    snprintf(filename, sizeof(filename), "%s\\%s_%04d-%02d-%02d_%02d%02d%02d.log",
        logDir_.c_str(), appName,
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    file_ = fopen(filename, "w");
}

void Logger::setLevel(LogLevel level) {
    level_ = level;
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) fflush(file_);
}

void Logger::log(LogLevel level, const char* module, const char* fmt, ...) {
    if (level < level_) return;
    if (!file_) return;

    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    const char* lvlStr;
    switch (level) {
        case LogLevel::Trace: lvlStr = "T"; break;
        case LogLevel::Debug: lvlStr = "D"; break;
        case LogLevel::Info:  lvlStr = "I"; break;
        case LogLevel::Warn:  lvlStr = "W"; break;
        case LogLevel::Error: lvlStr = "E"; break;
        default:              lvlStr = "?"; break;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    fprintf(file_, "[%lld.%03d] [%s] [%s] ",
        (long long)(ms / 1000), (int)(ms % 1000), lvlStr, module);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(file_, fmt, ap);
    va_end(ap);

    fputc('\n', file_);

    // M32g.2: 每行都 fflush 在 --debug 海量日志下会让音频/解码线程
    // 周期性卡在磁盘刷写上(表现为播放每1~2秒冻结一两秒)。
    // 改为: WARN 及以上立即刷新; 其余攒 64 条批量刷新。
    static int s_sinceFlush = 0;
    if (level >= LogLevel::Warn || ++s_sinceFlush >= 64) {
        fflush(file_);
        s_sinceFlush = 0;
    }
}
