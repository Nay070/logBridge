#include "Protocol.h"

#include <bit>
#include <string>
#include <utility>

namespace logbridge::protocol {
namespace {

constexpr std::uint32_t LogPayloadFixedSize = 8 + 8 + 4 + 4;
constexpr std::uint32_t MaxPayloadLength =
    LogPayloadFixedSize + MaxSourceLength + MaxContentLength;

void appendUint16(ByteBuffer& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void appendUint32(ByteBuffer& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendUint64(ByteBuffer& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void requireBytes(std::span<const std::uint8_t> input,
                  std::size_t offset,
                  std::size_t count) {
    // 使用减法检查，避免 offset + count 发生整数溢出。
    if (offset > input.size() || count > input.size() - offset) {
        throw ProtocolError("unexpected end of protocol data");
    }
}

std::uint8_t readUint8(std::span<const std::uint8_t> input,
                       std::size_t& offset) {
    requireBytes(input, offset, 1);
    return input[offset++];
}

std::uint16_t readUint16(std::span<const std::uint8_t> input,
                         std::size_t& offset) {
    requireBytes(input, offset, 2);

    const auto value =
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(input[offset]) << 8) |
        static_cast<std::uint16_t>(input[offset + 1]);

    offset += 2;
    return value;
}

std::uint32_t readUint32(std::span<const std::uint8_t> input,
                         std::size_t& offset) {
    requireBytes(input, offset, 4);

    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value = (value << 8) | input[offset + index];
    }

    offset += 4;
    return value;
}

std::uint64_t readUint64(std::span<const std::uint8_t> input,
                         std::size_t& offset) {
    requireBytes(input, offset, 8);

    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | input[offset + index];
    }

    offset += 8;
    return value;
}

std::string readString(std::span<const std::uint8_t> input,
                       std::size_t& offset,
                       std::uint32_t length) {
    requireBytes(input, offset, length);

    const char* begin =
        reinterpret_cast<const char*>(input.data() + offset);
    std::string value(begin, length);
    offset += length;
    return value;
}

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

FrameHeader parseFrameHeader(std::span<const std::uint8_t> header) {
    requireBytes(header, 0, FrameHeaderSize);

    std::size_t offset = 0;
    const std::uint32_t magic = readUint32(header, offset);
    const std::uint8_t version = readUint8(header, offset);
    const MessageType type = parseMessageType(readUint8(header, offset));
    const std::uint16_t reserved = readUint16(header, offset);
    const std::uint32_t payloadLength = readUint32(header, offset);

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

ByteBuffer serializeLogMessage(const LogMessage& message) {
    if (message.source.size() > MaxSourceLength) {
        throw ProtocolError("log source is too long");
    }

    if (message.content.size() > MaxContentLength) {
        throw ProtocolError("log content is too long");
    }

    const auto sourceLength =
        static_cast<std::uint32_t>(message.source.size());
    const auto contentLength =
        static_cast<std::uint32_t>(message.content.size());
    const std::uint32_t payloadLength =
        LogPayloadFixedSize + sourceLength + contentLength;

    ByteBuffer output;
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

LogMessage deserializeLogMessage(
    std::span<const std::uint8_t> frame) {
    const FrameHeader header = parseFrameHeader(frame);

    if (header.type != MessageType::Log) {
        throw ProtocolError("frame is not a log message");
    }

    const std::size_t expectedFrameSize =
        FrameHeaderSize + header.payloadLength;
    if (frame.size() != expectedFrameSize) {
        throw ProtocolError("frame length does not match header");
    }

    std::size_t offset = FrameHeaderSize;
    const std::uint64_t id = readUint64(frame, offset);
    const std::int64_t timestampMs =
        std::bit_cast<std::int64_t>(readUint64(frame, offset));
    const std::uint32_t sourceLength = readUint32(frame, offset);
    const std::uint32_t contentLength = readUint32(frame, offset);

    if (sourceLength > MaxSourceLength) {
        throw ProtocolError("log source is too long");
    }

    if (contentLength > MaxContentLength) {
        throw ProtocolError("log content is too long");
    }

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
