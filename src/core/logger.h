#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5
};

class Logger {
public:
    static Logger& instance();

    // 初始化日志：创建 logs/ 目录，按日期生成文件名，清理过期日志
    void init(const char* appName, int keepDays = 7);
    void setLevel(LogLevel level);
    LogLevel level() const { return level_; }

    void log(LogLevel level, const char* module, const char* fmt, ...);
    void flush();

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void cleanupOldLogs(int keepDays);
    std::string getExeDir();

    FILE* file_ = nullptr;
    LogLevel level_ = LogLevel::Trace;
    std::mutex mutex_;
    std::string logDir_;
};

#define LOG_TRACE(mod, fmt, ...) Logger::instance().log(LogLevel::Trace, mod, fmt, ##__VA_ARGS__)
#define LOG_DBG(mod, fmt, ...)  Logger::instance().log(LogLevel::Debug, mod, fmt, ##__VA_ARGS__)
#define LOG_INFO(mod, fmt, ...) Logger::instance().log(LogLevel::Info,  mod, fmt, ##__VA_ARGS__)
#define LOG_WARN(mod, fmt, ...) Logger::instance().log(LogLevel::Warn,  mod, fmt, ##__VA_ARGS__)
#define LOG_ERROR(mod, fmt, ...) Logger::instance().log(LogLevel::Error, mod, fmt, ##__VA_ARGS__)
