#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

// 线程安全的有界阻塞队列：队列满时生产者等待，队列空时消费者等待。
template <typename T>
class BoundedBlockingQueue {
public:
    // 容量为 0 会使生产者永久等待，因此直接拒绝。
    explicit BoundedBlockingQueue(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument(
                "BoundedBlockingQueue capacity must be greater than zero");
        }
    }

    // mutex 和 condition_variable 不可复制，因此队列也禁止复制。
    BoundedBlockingQueue(const BoundedBlockingQueue&) = delete;
    BoundedBlockingQueue& operator=(const BoundedBlockingQueue&) = delete;

    // 队列满时等待；成功返回 true，队列已关闭返回 false。
    bool push(T value) {
        std::unique_lock lock(mutex_);

        notFull_.wait(lock, [this] {
            return queue_.size() < capacity_ || closed_;
        });

        if (closed_) {
            return false;
        }

        queue_.push_back(std::move(value));

        // 先解锁再通知，减少被唤醒线程对 mutex 的无效竞争。
        lock.unlock();
        notEmpty_.notify_one();
        return true;
    }

    // 队列空时等待；关闭后取完剩余元素，再返回 nullopt。
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);

        notEmpty_.wait(lock, [this] {
            return !queue_.empty() || closed_;
        });

        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop_front();

        lock.unlock();
        notFull_.notify_one();
        return value;
    }

    // 关闭后不再接收新元素，并唤醒所有等待线程。
    void close() {
        {
            std::lock_guard lock(mutex_);

            if (closed_) {
                return;
            }

            closed_ = true;
        }

        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    // 读取共享队列也需要加锁。
    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    const std::size_t capacity_;
    std::deque<T> queue_;

    // mutex 保护 queue_ 和 closed_。
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    bool closed_{false};
};
