#include "core/thumbnail_worker.h"
#include "core/thumbnail_extractor.h"
#include "core/logger.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
}

// ---- ThumbnailCache ----

ThumbnailCache::Entry* ThumbnailCache::get(double time, int toleranceMs) {
    for (auto& e : entries_) {
        if (std::abs(e.time - time) * 1000.0 < toleranceMs) {
            e.lastAccess = SDL_GetTicks();
            return &e;
        }
    }
    return nullptr;
}

void ThumbnailCache::put(double time, SDL_Texture* tex, int w, int h) {
    Entry e;
    e.tex = tex;
    e.w = w;
    e.h = h;
    e.time = time;
    e.lastAccess = SDL_GetTicks();
    entries_.push_front(std::move(e));
    evict();
}

void ThumbnailCache::evict() {
    while ((int)entries_.size() > maxEntries_) {
        auto& back = entries_.back();
        if (back.tex) SDL_DestroyTexture(back.tex);
        entries_.pop_back();
    }
}

void ThumbnailCache::clear() {
    for (auto& e : entries_)
        if (e.tex) SDL_DestroyTexture(e.tex);
    entries_.clear();
}

// ---- ThumbnailWorker ----

ThumbnailWorker::~ThumbnailWorker() { stop(); }

void ThumbnailWorker::start(ThumbnailExtractor* extractor, ThumbnailCache* cache,
                            SDL_Renderer* renderer) {
    stop();
    extractor_ = extractor;
    cache_ = cache;
    renderer_ = renderer;
    running_.store(true);
    thread_ = std::thread(&ThumbnailWorker::workerLoop, this);
    LOG_INFO("ALOOP", "ThumbnailWorker started");
}

void ThumbnailWorker::stop() {
    running_.store(false);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    extractor_ = nullptr;
    cache_ = nullptr;
}

void ThumbnailWorker::request(double time) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentSeq_++;
    pending_.push_back({ time, currentSeq_ });
    cv_.notify_one();
}

SDL_Texture* ThumbnailWorker::getLatest() {
    std::lock_guard<std::mutex> lock(resultMutex_);
    if (resultTime_ < 0) return nullptr;
    if (!cache_) return nullptr;
    return cache_->get(resultTime_, 500) ? cache_->get(resultTime_, 500)->tex : nullptr;
}

void ThumbnailWorker::workerLoop() {
    while (running_.load()) {
        Request req;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return !pending_.empty() || !running_.load(); });
            if (!running_.load()) break;
            req = pending_.front();
            pending_.pop_front();
            // 跳过过期请求（只处理最新的）
            while (!pending_.empty() && pending_.front().seq > req.seq) {
                req = pending_.front();
                pending_.pop_front();
            }
        }

        if (!extractor_ || !cache_) continue;

        uint8_t* pixels = nullptr;
        int w = 0, h = 0;
        if (extractor_->getFrame(req.time, &pixels, w, h) && pixels && w > 0 && h > 0) {
            SDL_Texture* tex = SDL_CreateTexture(
                renderer_, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
            if (tex) {
                void* tbits = nullptr;
                int pitch = 0;
                if (SDL_LockTexture(tex, nullptr, &tbits, &pitch) == 0) {
                    for (int row = 0; row < h; ++row)
                        memcpy((Uint8*)tbits + row * pitch, pixels + row * w * 3, w * 3);
                    SDL_UnlockTexture(tex);
                    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                    cache_->put(req.time, tex, w, h);
                    {
                        std::lock_guard<std::mutex> lock(resultMutex_);
                        resultTime_ = req.time;
                        completedSeq_ = req.seq;
                    }
                } else {
                    SDL_DestroyTexture(tex);
                }
            }
            av_free(pixels);
        }
    }
}
