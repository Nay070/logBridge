#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace logbridge {

// 持久化每个客户端已经处理到的最大消息 ID。
class DedupStore {
public:
    // 打开去重状态文件；文件不存在时从空状态开始。
    explicit DedupStore(std::filesystem::path path);

    // 返回指定客户端已确认的最大消息 ID。
    [[nodiscard]] std::uint64_t highestConfirmed(
        const std::string& clientId) const;

    // 将客户端确认水位单调推进到 confirmedId，并返回最新水位。
    std::uint64_t confirm(const std::string& clientId,
                          std::uint64_t confirmedId);

private:
    // 从磁盘加载所有客户端确认水位。
    void load();

    // 原子保存给定确认水位快照。
    void persist(const std::map<std::string, std::uint64_t>& values) const;

    std::filesystem::path path_; // 去重状态文件路径。
    std::map<std::string, std::uint64_t> confirmed_; // clientId 到最大确认 ID 的映射。
    mutable std::mutex mutex_; // 保护 confirmed_。
};

} // logbridge 命名空间
