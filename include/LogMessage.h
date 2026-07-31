#pragma once

#include <cstdint>
#include <string>

// LogBridge 内部传递的一条完整日志消息。
struct LogMessage {
    std::uint64_t id{};          // 消息唯一编号，将来用于 ACK 和去重。
    std::int64_t timestampMs{};  // 读取到该日志时的毫秒级时间戳。
    std::string source;          // 产生该日志的文件路径或来源名称。
    std::string content;         // 一行完整的日志正文。
};
