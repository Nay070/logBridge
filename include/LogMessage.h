#pragma once

#include <cstdint>
#include <string>

// LogBridge 内部传递的一条完整日志消息。
struct LogMessage {
    std::uint64_t id{}; //消息唯一编号，将来用于 ACK 和去重。
    std::int64_t timestampMs{}; //读取日志的时间。
    std::string source; //日志文件路径。
    std::string content; //具体日志内容。
};
