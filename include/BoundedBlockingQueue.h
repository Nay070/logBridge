#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

// 线程安全的有界阻塞队列。
// T 表示队列中保存的元素类型；队列满时生产者等待，队列空时消费者等待。
template <typename T>
class BoundedBlockingQueue {
public:
    // 创建指定容量的队列。
    // capacity 表示队列最多可以保存多少个元素，不能为 0。
    explicit BoundedBlockingQueue(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument(
                "BoundedBlockingQueue capacity must be greater than zero");
        }
    }

    // 队列内部包含不可复制的互斥量和条件变量，因此禁止复制构造。
    BoundedBlockingQueue(const BoundedBlockingQueue&) = delete;

    // 禁止通过赋值复制队列，避免两个对象错误地共享同步状态。
    BoundedBlockingQueue& operator=(const BoundedBlockingQueue&) = delete;

    // 向队尾写入一个元素；队列满时等待，队列关闭时停止等待。
    // value 是准备写入的元素；成功返回 true，队列已关闭返回 false。
    bool push(T value) {
        std::unique_lock lock(mutex_); // 管理本次写操作持有的互斥锁。

        // 等待谓词：出现空位或队列关闭时结束等待。
        notFull_.wait(lock, [this] {
            return queue_.size() < capacity_ || closed_;
        });

        if (closed_) {
            return false;
        }

        queue_.push_back(std::move(value));

        // 先解锁再通知，减少被唤醒线程对 mutex_ 的无效竞争。
        lock.unlock();
        notEmpty_.notify_one();
        return true;
    }

    // 从队头取出一个元素；队列空时等待。
    // 队列关闭后仍会取完已有元素，完全为空时返回 nullopt。
    std::optional<T> pop() {
        std::unique_lock lock(mutex_); // 管理本次读操作持有的互斥锁。

        // 等待谓词：出现元素或队列关闭时结束等待。
        notEmpty_.wait(lock, [this] {
            return !queue_.empty() || closed_;
        });

        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front()); // 保存即将返回的队头元素。
        queue_.pop_front();

        lock.unlock();
        notFull_.notify_one();
        return value;
    }

    // 关闭队列，不再接收新元素，并唤醒所有正在等待的线程。
    void close() {
        {
            std::lock_guard lock(mutex_); // 修改关闭状态期间保护共享数据。

            if (closed_) {
                return;
            }

            closed_ = true;
        }

        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    // 返回当前元素数量；读取共享队列时同样需要加锁。
    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_); // 读取 queue_ 期间保护共享数据。
        return queue_.size();
    }

private:
    const std::size_t capacity_;       // 队列允许保存的最大元素数量。
    std::deque<T> queue_;              // 按先进先出顺序保存元素的底层容器。
    mutable std::mutex mutex_;         // 保护 queue_ 和 closed_ 的互斥量。
    std::condition_variable notEmpty_; // 通知消费者“队列已有数据或已关闭”。
    std::condition_variable notFull_;  // 通知生产者“队列已有空位或已关闭”。
    bool closed_{false};               // 标记队列是否已经停止接收新元素。
};
