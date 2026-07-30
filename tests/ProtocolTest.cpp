#include "Protocol.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using logbridge::protocol::ByteBuffer;
using logbridge::protocol::ProtocolError;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void requireProtocolError(Function&& function,
                          const std::string& message) {
    try {
        std::forward<Function>(function)();
    } catch (const ProtocolError&) {
        return;
    }

    throw std::runtime_error(message);
}

void testRoundTrip() {
    const LogMessage original{
        .id = 1001,
        .timestampMs = 1'785'331'995'346,
        .source = "/var/log/app.log",
        .content = "用户登录成功",
    };

    const ByteBuffer frame =
        logbridge::protocol::serializeLogMessage(original);
    const LogMessage restored =
        logbridge::protocol::deserializeLogMessage(frame);

    require(restored.id == original.id, "message id mismatch");
    require(restored.timestampMs == original.timestampMs,
            "timestamp mismatch");
    require(restored.source == original.source, "source mismatch");
    require(restored.content == original.content, "content mismatch");
}

void testHeaderEncoding() {
    const LogMessage message{
        .id = 1,
        .timestampMs = 2,
        .source = "",
        .content = "",
    };

    const ByteBuffer frame =
        logbridge::protocol::serializeLogMessage(message);

    require(frame.size() ==
                logbridge::protocol::FrameHeaderSize + 24,
            "unexpected empty-message frame size");
    require(frame[0] == 0x4C && frame[1] == 0x47 &&
                frame[2] == 0x42 && frame[3] == 0x31,
            "magic must use big-endian LGB1 bytes");
    require(frame[4] == logbridge::protocol::Version,
            "version byte mismatch");
    require(frame[5] == 1, "log message type mismatch");
    require(frame[6] == 0 && frame[7] == 0,
            "reserved bytes must be zero");

    const auto header = logbridge::protocol::parseFrameHeader(frame);
    require(header.payloadLength == 24,
            "payload length mismatch");
}

void testInvalidFrames() {
    const LogMessage message{
        .id = 7,
        .timestampMs = -10,
        .source = "app.log",
        .content = "hello",
    };

    const ByteBuffer valid =
        logbridge::protocol::serializeLogMessage(message);

    ByteBuffer truncated = valid;
    truncated.pop_back();
    requireProtocolError(
        [&] {
            logbridge::protocol::deserializeLogMessage(truncated);
        },
        "truncated frame should be rejected");

    ByteBuffer invalidMagic = valid;
    invalidMagic[0] ^= 0xFF;
    requireProtocolError(
        [&] {
            logbridge::protocol::deserializeLogMessage(invalidMagic);
        },
        "invalid magic should be rejected");

    ByteBuffer invalidVersion = valid;
    invalidVersion[4] = 99;
    requireProtocolError(
        [&] {
            logbridge::protocol::deserializeLogMessage(invalidVersion);
        },
        "unsupported version should be rejected");

    ByteBuffer invalidType = valid;
    invalidType[5] = 99;
    requireProtocolError(
        [&] {
            logbridge::protocol::deserializeLogMessage(invalidType);
        },
        "unknown message type should be rejected");

    ByteBuffer invalidReserved = valid;
    invalidReserved[6] = 1;
    requireProtocolError(
        [&] {
            logbridge::protocol::deserializeLogMessage(invalidReserved);
        },
        "non-zero reserved bits should be rejected");
}

void testLengthLimits() {
    LogMessage oversizedSource{
        .id = 1,
        .timestampMs = 1,
        .source =
            std::string(logbridge::protocol::MaxSourceLength + 1, 's'),
        .content = "content",
    };

    requireProtocolError(
        [&] {
            logbridge::protocol::serializeLogMessage(oversizedSource);
        },
        "oversized source should be rejected");

    LogMessage oversizedContent{
        .id = 1,
        .timestampMs = 1,
        .source = "app.log",
        .content =
            std::string(logbridge::protocol::MaxContentLength + 1, 'c'),
    };

    requireProtocolError(
        [&] {
            logbridge::protocol::serializeLogMessage(oversizedContent);
        },
        "oversized content should be rejected");
}

} // namespace

int main() {
    try {
        testRoundTrip();
        testHeaderEncoding();
        testInvalidFrames();
        testLengthLimits();

        std::cout << "Protocol tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Protocol test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
