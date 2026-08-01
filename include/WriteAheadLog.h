#pragma once

#include "LogMessage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

namespace logbridge {

// 持久化尚未收到 ACK 的日志消息。
class WriteAheadLog {
public:
    // 打开 WAL，并恢复其中所有完整记录。
    explicit WriteAheadLog(std::filesystem::path path);

    // 持久化一条消息，成功返回后才允许进入发送队列。
    void append(const LogMessage& message);

    // 删除 ID 不大于 confirmedId 的已确认消息。
    void confirm(std::uint64_t confirmedId);

    // 返回当前所有待确认消息的副本。
    [[nodiscard]] std::vector<LogMessage> pending() const;

    // 返回 WAL 中最大的消息 ID；没有记录时返回 0。
    [[nodiscard]] std::uint64_t maxMessageId() const;

    // 返回当前尚未收到 ACK 的消息数量。
    [[nodiscard]] std::size_t pendingCount() const;

private:
    // 从磁盘加载完整记录，并清理崩溃留下的不完整尾部。
    void load();

    // 将 pending_ 原子重写到 WAL 文件。
    void rewriteLocked();

    std::filesystem::path path_; // WAL 文件路径。
    std::vector<LogMessage> pending_; // 内存中的未确认消息。
    mutable std::mutex mutex_; // 保护 pending_ 与 WAL 重写过程。
};

} // logbridge 命名空间
