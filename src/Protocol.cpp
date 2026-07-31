#include "Protocol.h"

#include <bit>
#include <string>
#include <utility>

namespace logbridge::protocol {
namespace {

constexpr std::uint32_t LogRecordFixedSize = 8 + 8 + 4 + 4; // 每条日志固定字段的字节数。
constexpr std::uint32_t AckPayloadSize = 8;                  // ACK 中确认 ID 的字节数。
constexpr std::uint32_t BatchPrefixSize = 4;                 // 批次中消息数量字段的字节数。

// 将 16 位整数 value 按大端字节序追加到 output。
void appendUint16(ByteBuffer& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

// 将 32 位整数 value 按大端字节序追加到 output。
void appendUint32(ByteBuffer& output, std::uint32_t value) {
    // shift 表示当前需要写入最低 8 位的右移位数。
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

// 将 64 位整数 value 按大端字节序追加到 output。
void appendUint64(ByteBuffer& output, std::uint64_t value) {
    // shift 表示当前需要写入最低 8 位的右移位数。
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

// 向 output 写入一个完整固定帧头。
void appendFrameHeader(ByteBuffer& output,
                       MessageType type,
                       std::uint32_t payloadLength) {
    appendUint32(output, Magic);
    output.push_back(Version);
    output.push_back(static_cast<std::uint8_t>(type));
    appendUint16(output, 0);
    appendUint32(output, payloadLength);
}

// 检查 input 从 offset 开始是否还包含 count 个字节。
void requireBytes(std::span<const std::uint8_t> input,
                  std::size_t offset,
                  std::size_t count) {
    // 使用减法检查，避免 offset + count 发生整数溢出。
    if (offset > input.size() || count > input.size() - offset) {
        throw ProtocolError("unexpected end of protocol data");
    }
}

// 从 input 的 offset 位置读取 8 位整数，并把 offset 向后移动 1 字节。
std::uint8_t readUint8(std::span<const std::uint8_t> input,
                       std::size_t& offset) {
    requireBytes(input, offset, 1);
    return input[offset++];
}

// 从 input 的 offset 位置按大端字节序读取 16 位整数，并更新 offset。
std::uint16_t readUint16(std::span<const std::uint8_t> input,
                         std::size_t& offset) {
    requireBytes(input, offset, 2);

    // value 保存两个网络字节组合而成的主机整数。
    const auto value =
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(input[offset]) << 8) |
        static_cast<std::uint16_t>(input[offset + 1]);

    offset += 2;
    return value;
}

// 从 input 的 offset 位置按大端字节序读取 32 位整数，并更新 offset。
std::uint32_t readUint32(std::span<const std::uint8_t> input,
                         std::size_t& offset) {
    requireBytes(input, offset, 4);

    std::uint32_t value = 0; // 保存逐字节拼接得到的最终整数。
    for (int index = 0; index < 4; ++index) {
        value = (value << 8) | input[offset + index];
    }

    offset += 4;
    return value;
}

// 从 input 的 offset 位置按大端字节序读取 64 位整数，并更新 offset。
std::uint64_t readUint64(std::span<const std::uint8_t> input,
                         std::size_t& offset) {
    requireBytes(input, offset, 8);

    std::uint64_t value = 0; // 保存逐字节拼接得到的最终整数。
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | input[offset + index];
    }

    offset += 8;
    return value;
}

// 从 input 的 offset 位置读取 length 个字节并构造字符串。
std::string readString(std::span<const std::uint8_t> input,
                       std::size_t& offset,
                       std::uint32_t length) {
    requireBytes(input, offset, length);

    const char* begin = // 指向字符串内容在字节数组中的首地址。
        reinterpret_cast<const char*>(input.data() + offset);
    std::string value(begin, length); // 保存恢复出的字符串。
    offset += length;
    return value;
}

// 将网络中的原始类型值转换成 MessageType，并拒绝未知类型。
MessageType parseMessageType(std::uint8_t rawType) {
    switch (rawType) {
        case static_cast<std::uint8_t>(MessageType::Log):
            return MessageType::Log;
        case static_cast<std::uint8_t>(MessageType::Ack):
            return MessageType::Ack;
        case static_cast<std::uint8_t>(MessageType::LogBatch):
            return MessageType::LogBatch;
        default:
            throw ProtocolError("unknown message type");
    }
}

// 校验日志字符串长度，并返回这条日志编码后的记录大小。
std::size_t logRecordSize(const LogMessage& message) {
    if (message.source.size() > MaxSourceLength) {
        throw ProtocolError("log source is too long");
    }
    if (message.content.size() > MaxContentLength) {
        throw ProtocolError("log content is too long");
    }

    return LogRecordFixedSize + message.source.size() +
           message.content.size();
}

// 将一条日志的字段追加到 output，不包含协议帧头。
void appendLogRecord(ByteBuffer& output, const LogMessage& message) {
    const auto sourceLength = // 日志来源字符串的 UTF-8 字节数。
        static_cast<std::uint32_t>(message.source.size());
    const auto contentLength = // 日志正文字符串的 UTF-8 字节数。
        static_cast<std::uint32_t>(message.content.size());

    appendUint64(output, message.id);
    appendUint64(
        output,
        std::bit_cast<std::uint64_t>(message.timestampMs));
    appendUint32(output, sourceLength);
    appendUint32(output, contentLength);
    output.insert(
        output.end(), message.source.begin(), message.source.end());
    output.insert(
        output.end(), message.content.begin(), message.content.end());
}

// 从 input 中读取一条不含帧头的日志记录，并更新 offset。
LogMessage readLogRecord(std::span<const std::uint8_t> input,
                         std::size_t& offset) {
    const std::uint64_t id = readUint64(input, offset); // 恢复出的消息编号。
    const std::int64_t timestampMs = // 恢复出的毫秒级时间戳。
        std::bit_cast<std::int64_t>(readUint64(input, offset));
    const std::uint32_t sourceLength = // 来源字符串的字节数。
        readUint32(input, offset);
    const std::uint32_t contentLength = // 正文字符串的字节数。
        readUint32(input, offset);

    if (sourceLength > MaxSourceLength) {
        throw ProtocolError("log source is too long");
    }
    if (contentLength > MaxContentLength) {
        throw ProtocolError("log content is too long");
    }

    return LogMessage{
        .id = id,
        .timestampMs = timestampMs,
        .source = readString(input, offset, sourceLength),
        .content = readString(input, offset, contentLength),
    };
}

// 检查 frame 的实际长度是否与帧头声明完全一致。
void requireFrameSize(std::span<const std::uint8_t> frame,
                      const FrameHeader& header) {
    const std::size_t expectedSize = // 当前帧根据头部声明应有的总长度。
        FrameHeaderSize + header.payloadLength;
    if (frame.size() != expectedSize) {
        throw ProtocolError("frame length does not match header");
    }
}

} // namespace

// 校验并解析固定 12 字节帧头。
FrameHeader parseFrameHeader(std::span<const std::uint8_t> header) {
    requireBytes(header, 0, FrameHeaderSize);

    std::size_t offset = 0; // 记录下一个帧头字段的读取位置。
    const std::uint32_t magic = readUint32(header, offset); // 协议固定标识。
    const std::uint8_t version = readUint8(header, offset); // 协议版本。
    const std::uint8_t rawType = readUint8(header, offset); // 原始消息类型值。
    const std::uint16_t reserved = readUint16(header, offset); // 当前必须为 0。
    const std::uint32_t payloadLength = // 帧头之后的数据字节数。
        readUint32(header, offset);

    if (magic != Magic) {
        throw ProtocolError("invalid protocol magic");
    }
    if (version != Version) {
        throw ProtocolError("unsupported protocol version");
    }
    if (reserved != 0) {
        throw ProtocolError("reserved header bits must be zero");
    }
    if (payloadLength > MaxFramePayloadLength) {
        throw ProtocolError("payload is too large");
    }

    return FrameHeader{
        .version = version,
        .type = parseMessageType(rawType),
        .payloadLength = payloadLength,
    };
}

// 把单条 LogMessage 编码成“固定帧头 + 日志记录”。
ByteBuffer serializeLogMessage(const LogMessage& message) {
    const std::size_t payloadSize = logRecordSize(message); // 日志记录总字节数。
    const auto payloadLength = static_cast<std::uint32_t>(payloadSize);

    ByteBuffer output; // 保存最终生成的完整网络帧。
    output.reserve(FrameHeaderSize + payloadLength);
    appendFrameHeader(output, MessageType::Log, payloadLength);
    appendLogRecord(output, message);
    return output;
}

// 校验单日志帧，并恢复其中的 LogMessage。
LogMessage deserializeLogMessage(
    std::span<const std::uint8_t> frame) {
    const FrameHeader header = parseFrameHeader(frame); // 解析出的帧头。
    if (header.type != MessageType::Log) {
        throw ProtocolError("frame is not a log message");
    }
    requireFrameSize(frame, header);

    std::size_t offset = FrameHeaderSize; // 日志记录开始位置。
    LogMessage message = readLogRecord(frame, offset); // 恢复出的日志。
    if (offset != frame.size()) {
        throw ProtocolError("log fields do not fill the payload");
    }
    return message;
}

// 将多条日志编码进一个具有明确边界的批次帧。
ByteBuffer serializeLogBatch(std::span<const LogMessage> messages) {
    if (messages.empty()) {
        throw ProtocolError("log batch cannot be empty");
    }
    if (messages.size() > MaxBatchMessageCount) {
        throw ProtocolError("too many messages in log batch");
    }

    std::size_t payloadSize = BatchPrefixSize; // 数量字段和所有日志记录的总大小。
    for (const LogMessage& message : messages) {
        const std::size_t recordSize = logRecordSize(message); // 当前日志记录大小。
        if (recordSize > MaxFramePayloadLength - payloadSize) {
            throw ProtocolError("log batch payload is too large");
        }
        payloadSize += recordSize;
    }

    const auto payloadLength = static_cast<std::uint32_t>(payloadSize);
    ByteBuffer output; // 保存完整批次协议帧。
    output.reserve(FrameHeaderSize + payloadLength);
    appendFrameHeader(output, MessageType::LogBatch, payloadLength);
    appendUint32(output, static_cast<std::uint32_t>(messages.size()));
    for (const LogMessage& message : messages) {
        appendLogRecord(output, message);
    }
    return output;
}

// 校验批次帧，并按原顺序恢复其中的全部日志。
std::vector<LogMessage> deserializeLogBatch(
    std::span<const std::uint8_t> frame) {
    const FrameHeader header = parseFrameHeader(frame); // 解析出的批次帧头。
    if (header.type != MessageType::LogBatch) {
        throw ProtocolError("frame is not a log batch");
    }
    requireFrameSize(frame, header);

    std::size_t offset = FrameHeaderSize; // 批次消息数量字段的起点。
    const std::uint32_t messageCount = readUint32(frame, offset); // 日志条数。
    if (messageCount == 0 || messageCount > MaxBatchMessageCount) {
        throw ProtocolError("invalid log batch message count");
    }

    std::vector<LogMessage> messages; // 保存从批次中恢复的日志。
    messages.reserve(messageCount);
    for (std::uint32_t index = 0; index < messageCount; ++index) {
        messages.push_back(readLogRecord(frame, offset));
    }

    if (offset != frame.size()) {
        throw ProtocolError("log batch fields do not fill the payload");
    }
    return messages;
}

// 将 confirmedId 编码成固定 8 字节 Payload 的 ACK 帧。
ByteBuffer serializeAck(std::uint64_t confirmedId) {
    ByteBuffer output; // 保存完整 ACK 协议帧。
    output.reserve(FrameHeaderSize + AckPayloadSize);
    appendFrameHeader(output, MessageType::Ack, AckPayloadSize);
    appendUint64(output, confirmedId);
    return output;
}

// 校验 ACK 帧并恢复服务端确认的消息 ID。
AckMessage deserializeAck(std::span<const std::uint8_t> frame) {
    const FrameHeader header = parseFrameHeader(frame); // 解析出的 ACK 帧头。
    if (header.type != MessageType::Ack) {
        throw ProtocolError("frame is not an ack message");
    }
    requireFrameSize(frame, header);
    if (header.payloadLength != AckPayloadSize) {
        throw ProtocolError("invalid ack payload length");
    }

    std::size_t offset = FrameHeaderSize; // confirmedId 在帧中的读取位置。
    return AckMessage{
        .confirmedId = readUint64(frame, offset),
    };
}

} // namespace logbridge::protocol
