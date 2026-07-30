#pragma once

#include "LogMessage.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace logbridge::protocol {

using ByteBuffer = std::vector<std::uint8_t>;

// 0x4C474231 对应 ASCII 字符串 "LGB1"。
inline constexpr std::uint32_t Magic = 0x4C474231;
inline constexpr std::uint8_t Version = 1;
inline constexpr std::size_t FrameHeaderSize = 12;

inline constexpr std::uint32_t MaxSourceLength = 4 * 1024;
inline constexpr std::uint32_t MaxContentLength = 1024 * 1024;

enum class MessageType : std::uint8_t {
    Log = 1,
    Ack = 2,
};

struct FrameHeader {
    std::uint8_t version{};
    MessageType type{};
    std::uint32_t payloadLength{};
};

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 将一条日志编码为完整网络帧，所有整数使用大端字节序。
ByteBuffer serializeLogMessage(const LogMessage& message);

// 从完整网络帧恢复日志；数据不完整或不合法时抛出 ProtocolError。
LogMessage deserializeLogMessage(std::span<const std::uint8_t> frame);

// 单独解析固定头部，下一阶段接收 TCP 数据时用于确定 Payload 长度。
FrameHeader parseFrameHeader(std::span<const std::uint8_t> header);

} // namespace logbridge::protocol
