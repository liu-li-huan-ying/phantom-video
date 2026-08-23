#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL.h>

struct AVFrame;

class ThumbnailExtractor;

// LRU 缩略图缓存：避免重复 seek 提取同一时间点
class ThumbnailCache {
public:
    struct Entry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
        double time = 0.0;
        Uint32 lastAccess = 0;
    };

    explicit ThumbnailCache(SDL_Renderer* r) : renderer_(r) {}
    ~ThumbnailCache() { clear(); }

    // 查找缓存：toleranceMs 内的时间点视为命中
    Entry* get(double time, int toleranceMs = 500);
    // 存入缓存（接管 tex 所有权）
    void put(double time, SDL_Texture* tex, int w, int h);
    // 超过 maxEntries_ 时淘汰最旧
    void evict();
    // 切视频时清空
    void clear();

    void setMaxEntries(int n) { maxEntries_ = n; }

private:
    SDL_Renderer* renderer_ = nullptr;
    std::deque<Entry> entries_;
    int maxEntries_ = 30;
};

// 多线程缩略图提取器：不阻塞主循环
class ThumbnailWorker {
public:
    ThumbnailWorker() = default;
    ~ThumbnailWorker();

    // 启动 worker 线程
    void start(ThumbnailExtractor* extractor, ThumbnailCache* cache,
               SDL_Renderer* renderer);
    void stop();

    // 主线程调用：请求提取（自动取消旧请求）
    void request(double time);

    // 主线程调用：取最新提取的纹理（无缓存命中时返回 nullptr）
    // 返回的纹理所有权仍在 cache 中，调用者不要 free
    SDL_Texture* getLatest();

    bool isRunning() const { return running_.load(); }

private:
    void workerLoop();

    ThumbnailExtractor* extractor_ = nullptr;
    ThumbnailCache* cache_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::mutex extractorMutex_;  // 保护 extractor_ 并发访问

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{ false };

    struct Request {
        double time = 0.0;
        int seq = 0;
    };
    std::deque<Request> pending_;
    int currentSeq_ = 0;
    int completedSeq_ = -1;

    // 最新完成的结果（主线程读取）
    std::mutex resultMutex_;
    double resultTime_ = -1.0;
};
