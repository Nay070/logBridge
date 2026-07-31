#pragma once

#include "LogMessage.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace logbridge::protocol {

using ByteBuffer = std::vector<std::uint8_t>; // 保存网络帧原始字节的动态数组。

inline constexpr std::uint32_t Magic = 0x4C474231; // 协议标识，对应 ASCII 字符串 "LGB1"。
inline constexpr std::uint8_t Version = 1;         // 当前协议版本号。
inline constexpr std::size_t FrameHeaderSize = 12; // 每个网络帧固定头部的字节数。

inline constexpr std::uint32_t MaxSourceLength = 4 * 1024;       // 日志来源允许的最大字节数。
inline constexpr std::uint32_t MaxContentLength = 1024 * 1024;  // 日志正文允许的最大字节数。

// 表示网络帧中承载的消息种类。
enum class MessageType : std::uint8_t {
    Log = 1, // 日志消息。
    Ack = 2, // 服务端确认消息，为后续阶段预留。
};

// 保存从固定帧头中解析出的信息。
struct FrameHeader {
    std::uint8_t version{};          // 发送方使用的协议版本。
    MessageType type{};              // 当前帧的消息类型。
    std::uint32_t payloadLength{};   // 帧头之后 Payload 的字节数。
};

// 表示协议数据不完整、格式错误或超过限制。
class ProtocolError : public std::runtime_error {
public:
    // 直接复用 std::runtime_error 的构造函数保存错误说明。
    using std::runtime_error::runtime_error;
};

// 将一条日志编码为完整网络帧，所有整数使用大端字节序。
// message 是待编码的日志；返回可直接交给 Socket 发送的字节数组。
ByteBuffer serializeLogMessage(const LogMessage& message);

// 从完整网络帧恢复日志。
// frame 是一帧完整数据；数据不完整或不合法时抛出 ProtocolError。
LogMessage deserializeLogMessage(std::span<const std::uint8_t> frame);

// 单独解析固定头部，供 TCP 接收端确定后续 Payload 的长度。
// header 至少包含 FrameHeaderSize 个字节；返回解析后的头部字段。
FrameHeader parseFrameHeader(std::span<const std::uint8_t> header);

} // namespace logbridge::protocol
