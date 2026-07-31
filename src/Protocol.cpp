#include "Protocol.h"

#include <bit>
#include <string>
#include <utility>

namespace logbridge::protocol {
namespace {

// Log Payload 中 id、时间戳和两个字符串长度字段占用的固定字节数。
constexpr std::uint32_t LogPayloadFixedSize = 8 + 8 + 4 + 4;

// 一条合法日志 Payload 允许占用的最大总字节数。
constexpr std::uint32_t MaxPayloadLength =
    LogPayloadFixedSize + MaxSourceLength + MaxContentLength;

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

// 检查 input 从 offset 开始是否还包含 count 个字节。
// 字节不足时抛出 ProtocolError，防止后续读取越界。
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
    // index 表示当前正在读取四个字节中的第几个字节。
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
    // index 表示当前正在读取八个字节中的第几个字节。
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | input[offset + index];
    }

    offset += 8;
    return value;
}

// 从 input 的 offset 位置读取 length 个字节并构造字符串，同时更新 offset。
std::string readString(std::span<const std::uint8_t> input,
                       std::size_t& offset,
                       std::uint32_t length) {
    requireBytes(input, offset, length);

    // begin 指向字符串内容在字节数组中的首地址。
    const char* begin =
        reinterpret_cast<const char*>(input.data() + offset);
    std::string value(begin, length); // 保存从原始字节恢复出的字符串。
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
        default:
            throw ProtocolError("unknown message type");
    }
}

} // namespace

// 校验并解析固定 12 字节帧头，返回后续接收 Payload 所需的信息。
FrameHeader parseFrameHeader(std::span<const std::uint8_t> header) {
    requireBytes(header, 0, FrameHeaderSize);

    std::size_t offset = 0; // 记录下一个帧头字段的读取位置。
    const std::uint32_t magic = readUint32(header, offset); // 协议固定标识。
    const std::uint8_t version = readUint8(header, offset); // 发送方协议版本。
    const MessageType type = // 当前帧承载的消息类型。
        parseMessageType(readUint8(header, offset));
    const std::uint16_t reserved = // 为未来扩展预留的字段，当前必须为 0。
        readUint16(header, offset);
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

    if (payloadLength > MaxPayloadLength) {
        throw ProtocolError("payload is too large");
    }

    return FrameHeader{
        .version = version,
        .type = type,
        .payloadLength = payloadLength,
    };
}

// 把 LogMessage 的字段依次编码成“固定帧头 + 日志 Payload”。
ByteBuffer serializeLogMessage(const LogMessage& message) {
    if (message.source.size() > MaxSourceLength) {
        throw ProtocolError("log source is too long");
    }

    if (message.content.size() > MaxContentLength) {
        throw ProtocolError("log content is too long");
    }

    // sourceLength 是日志来源字符串的 UTF-8 字节数。
    const auto sourceLength =
        static_cast<std::uint32_t>(message.source.size());
    // contentLength 是日志正文字符串的 UTF-8 字节数。
    const auto contentLength =
        static_cast<std::uint32_t>(message.content.size());
    // payloadLength 是当前日志 Payload 的完整字节数。
    const std::uint32_t payloadLength =
        LogPayloadFixedSize + sourceLength + contentLength;

    ByteBuffer output; // 保存最终生成的完整网络帧。
    output.reserve(FrameHeaderSize + payloadLength);

    // 固定头部：Magic、版本、类型、预留字段和 Payload 长度。
    appendUint32(output, Magic);
    output.push_back(Version);
    output.push_back(static_cast<std::uint8_t>(MessageType::Log));
    appendUint16(output, 0);
    appendUint32(output, payloadLength);

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

    return output;
}

// 校验完整网络帧，并把其中各字段恢复为 LogMessage。
LogMessage deserializeLogMessage(
    std::span<const std::uint8_t> frame) {
    // header 保存从 frame 固定头部解析出的元数据。
    const FrameHeader header = parseFrameHeader(frame);

    if (header.type != MessageType::Log) {
        throw ProtocolError("frame is not a log message");
    }

    // expectedFrameSize 是根据帧头计算出的整帧应有字节数。
    const std::size_t expectedFrameSize =
        FrameHeaderSize + header.payloadLength;
    if (frame.size() != expectedFrameSize) {
        throw ProtocolError("frame length does not match header");
    }

    std::size_t offset = FrameHeaderSize; // 跳过帧头后，下一个字段的读取位置。
    const std::uint64_t id = readUint64(frame, offset); // 恢复出的消息编号。
    const std::int64_t timestampMs = // 恢复出的毫秒级时间戳。
        std::bit_cast<std::int64_t>(readUint64(frame, offset));
    const std::uint32_t sourceLength = // 即将读取的来源字符串字节数。
        readUint32(frame, offset);
    const std::uint32_t contentLength = // 即将读取的正文字符串字节数。
        readUint32(frame, offset);

    if (sourceLength > MaxSourceLength) {
        throw ProtocolError("log source is too long");
    }

    if (contentLength > MaxContentLength) {
        throw ProtocolError("log content is too long");
    }

    // expectedPayloadLength 根据字符串字段长度重新计算 Payload 大小。
    const std::uint32_t expectedPayloadLength =
        LogPayloadFixedSize + sourceLength + contentLength;
    if (header.payloadLength != expectedPayloadLength) {
        throw ProtocolError("log field lengths do not match payload");
    }

    return LogMessage{
        .id = id,
        .timestampMs = timestampMs,
        .source = readString(frame, offset, sourceLength),
        .content = readString(frame, offset, contentLength),
    };
}

} // namespace logbridge::protocol
