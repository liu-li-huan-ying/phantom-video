#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>

struct SizeCount {
    template <typename T>
    size_t operator()(const T&) const { return 1; }
};

template <typename T, typename SizeOf = SizeCount>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t maxTotal, SizeOf sz = {})
        : max_(maxTotal), sizeOf_(sz) {}
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        size_t sz = sizeOf_(item);
        notFull_.wait(lock, [&] { return closed_ || total_ + sz <= max_; });
        if (closed_) return false;
        queue_.push_back(std::move(item));
        total_ += sz;
        notEmpty_.notify_one();
        return true;
    }

    bool tryPush(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t sz = sizeOf_(item);
        if (closed_) return false;
        if (total_ + sz > max_) return false;
        queue_.push_back(std::move(item));
        total_ += sz;
        notEmpty_.notify_one();
        return true;
    }

    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        total_ -= sizeOf_(out);
        notFull_.notify_one();
        return true;
    }

    bool tryPop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        total_ -= sizeOf_(out);
        notFull_.notify_one();
        return true;
    }

    bool peek(T& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = queue_.front();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        total_ = 0;
        notFull_.notify_all();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void reopen() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = false;
        queue_.clear();
        total_ = 0;
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable notFull_, notEmpty_;
    std::deque<T> queue_;
    size_t max_;
    size_t total_ = 0;
    bool closed_ = false;
    SizeOf sizeOf_;
};