#pragma once

#include "LogMessage.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace logbridge {

// 持久化客户端身份、下一个消息 ID 和日志读取检查点。
class ClientState {
public:
    // 加载状态，并使用 recoveredMessages 修正可能落后的消息 ID。
    ClientState(std::filesystem::path path,
                std::string sourcePath,
                std::span<const LogMessage> recoveredMessages);

    // 返回稳定的客户端身份。
    [[nodiscard]] const std::string& clientId() const noexcept;

    // 持久化地占用并返回下一个消息 ID。
    std::uint64_t nextMessageId();

    // 返回上次已写入 WAL 的完整日志行末尾偏移量。
    [[nodiscard]] std::uint64_t fileOffset() const noexcept;

    // 返回检查点对应文件所在设备的编号。
    [[nodiscard]] std::uint64_t fileDeviceId() const noexcept;

    // 返回检查点对应文件的 inode 编号。
    [[nodiscard]] std::uint64_t fileInode() const noexcept;

    // 持久化新的完整日志行检查点。
    void updateFileOffset(std::uint64_t offset);

    // 同时持久化文件偏移量和文件身份，用于识别重启期间发生的轮转。
    void updateFileCheckpoint(std::uint64_t offset,
                              std::uint64_t deviceId,
                              std::uint64_t inode);

private:
    // 从状态文件加载字段。
    void load();

    // 原子保存当前状态。
    void persist() const;

    std::filesystem::path path_; // 客户端状态文件路径。
    std::string clientId_; // 跨重启保持不变的客户端 ID。
    std::uint64_t nextId_{1}; // 下一个可以分配的消息 ID。
    std::string sourcePath_; // 当前状态对应的日志文件路径。
    std::uint64_t fileOffset_{0}; // 已安全写入 WAL 的文件偏移量。
    std::uint64_t fileDeviceId_{0}; // 检查点对应文件所在设备的编号。
    std::uint64_t fileInode_{0}; // 检查点对应文件的 inode 编号。
};

} // logbridge 命名空间
