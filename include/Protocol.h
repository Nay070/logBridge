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
inline constexpr std::uint32_t MaxBatchMessageCount = 100;      // 一个批次允许包含的最大日志条数。
inline constexpr std::uint32_t MaxFramePayloadLength =
    16 * 1024 * 1024; // 单个协议帧允许携带的最大 Payload 字节数。

// 表示网络帧中承载的消息种类。
enum class MessageType : std::uint8_t {
    Log = 1, // 日志消息。
    Ack = 2, // 服务端累计确认消息。
    LogBatch = 3, // 包含多条日志的批次消息。
};

// 保存从固定帧头中解析出的信息。
struct FrameHeader {
    std::uint8_t version{};          // 发送方使用的协议版本。
    MessageType type{};              // 当前帧的消息类型。
    std::uint32_t payloadLength{};   // 帧头之后 Payload 的字节数。
};

// 服务端返回的累计确认消息。
struct AckMessage {
    std::uint64_t confirmedId{}; // 表示该 ID 及之前的消息已经处理完成。
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

// 将 messages 编码为一个批次帧；空批次或批次过大时抛出 ProtocolError。
ByteBuffer serializeLogBatch(std::span<const LogMessage> messages);

// 从完整批次帧恢复多条日志；数据不合法时抛出 ProtocolError。
std::vector<LogMessage> deserializeLogBatch(
    std::span<const std::uint8_t> frame);

// 将累计确认 ID 编码为 ACK 帧。
ByteBuffer serializeAck(std::uint64_t confirmedId);

// 从完整 ACK 帧中恢复累计确认 ID。
AckMessage deserializeAck(std::span<const std::uint8_t> frame);

// 单独解析固定头部，供 TCP 接收端确定后续 Payload 的长度。
// header 至少包含 FrameHeaderSize 个字节；返回解析后的头部字段。
FrameHeader parseFrameHeader(std::span<const std::uint8_t> header);

} // namespace logbridge::protocol
