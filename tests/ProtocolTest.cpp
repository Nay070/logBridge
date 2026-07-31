#include "Protocol.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using logbridge::protocol::ByteBuffer;    // 简化测试中字节数组类型的书写。
using logbridge::protocol::ProtocolError; // 简化测试中协议异常类型的书写。

// 检查 condition；失败时使用 message 抛出异常终止当前测试。
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
// 执行 function 并确认它抛出 ProtocolError，否则使用 message 报错。
void requireProtocolError(Function&& function,
                          const std::string& message) {
    try {
        std::forward<Function>(function)();
    } catch (const ProtocolError&) {
        return;
    }

    throw std::runtime_error(message);
}

// 验证日志经过序列化再反序列化后，全部字段保持不变。
void testRoundTrip() {
    // original 是序列化前的原始日志，包含中文以验证 UTF-8 数据。
    const LogMessage original{
        .id = 1001,
        .timestampMs = 1'785'331'995'346,
        .source = "/var/log/app.log",
        .content = "用户登录成功",
    };

    // frame 保存 original 编码后的完整协议帧。
    const ByteBuffer frame =
        logbridge::protocol::serializeLogMessage(original);
    // restored 保存从 frame 反序列化得到的日志。
    const LogMessage restored =
        logbridge::protocol::deserializeLogMessage(frame);

    require(restored.id == original.id, "message id mismatch");
    require(restored.timestampMs == original.timestampMs,
            "timestamp mismatch");
    require(restored.source == original.source, "source mismatch");
    require(restored.content == original.content, "content mismatch");
}

// 验证固定帧头各字段的位置、字节序和 Payload 长度。
void testHeaderEncoding() {
    // message 使用空字符串，使 Payload 只保留 24 字节固定字段。
    const LogMessage message{
        .id = 1,
        .timestampMs = 2,
        .source = "",
        .content = "",
    };

    // frame 保存用于逐字节检查帧头的序列化结果。
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

    // header 保存通过公开接口解析出的固定帧头字段。
    const auto header = logbridge::protocol::parseFrameHeader(frame);
    require(header.payloadLength == 24,
            "payload length mismatch");
}

// 验证多条日志可以封装进一个批次帧并按原顺序恢复。
void testBatchRoundTrip() {
    const std::vector<LogMessage> original{ // 准备放入同一批次的三条日志。
        LogMessage{
            .id = 10,
            .timestampMs = 100,
            .source = "app.log",
            .content = "第一条",
        },
        LogMessage{
            .id = 11,
            .timestampMs = 101,
            .source = "app.log",
            .content = "second",
        },
        LogMessage{
            .id = 12,
            .timestampMs = 102,
            .source = "worker.log",
            .content = "third",
        },
    };

    const ByteBuffer frame = // original 编码后的完整批次帧。
        logbridge::protocol::serializeLogBatch(original);
    const auto header = // 用于确认批次消息类型的帧头。
        logbridge::protocol::parseFrameHeader(frame);
    const std::vector<LogMessage> restored = // 从批次帧恢复出的日志。
        logbridge::protocol::deserializeLogBatch(frame);

    require(header.type == logbridge::protocol::MessageType::LogBatch,
            "batch frame type mismatch");
    require(restored.size() == original.size(),
            "batch message count mismatch");
    for (std::size_t index = 0; index < original.size(); ++index) {
        require(restored[index].id == original[index].id,
                "batch message id mismatch");
        require(restored[index].timestampMs == original[index].timestampMs,
                "batch timestamp mismatch");
        require(restored[index].source == original[index].source,
                "batch source mismatch");
        require(restored[index].content == original[index].content,
                "batch content mismatch");
    }
}

// 验证累计确认 ID 可以经过 ACK 帧往返而保持不变。
void testAckRoundTrip() {
    constexpr std::uint64_t ConfirmedId = 9876; // 服务端准备确认的最大消息 ID。
    const ByteBuffer frame = // ConfirmedId 编码后的 ACK 帧。
        logbridge::protocol::serializeAck(ConfirmedId);
    const auto header = // ACK 帧解析出的固定头部。
        logbridge::protocol::parseFrameHeader(frame);
    const auto ack = // 从 ACK Payload 中恢复出的确认消息。
        logbridge::protocol::deserializeAck(frame);

    require(header.type == logbridge::protocol::MessageType::Ack,
            "ack frame type mismatch");
    require(header.payloadLength == 8,
            "ack payload length mismatch");
    require(ack.confirmedId == ConfirmedId,
            "ack confirmed id mismatch");
}

// 分别篡改或截断合法帧，验证协议层会拒绝异常数据。
void testInvalidFrames() {
    // message 是生成后续各种异常测试帧的基础日志。
    const LogMessage message{
        .id = 7,
        .timestampMs = -10,
        .source = "app.log",
        .content = "hello",
    };

    // valid 是未经篡改的合法协议帧，每个异常用例都从它复制。
    const ByteBuffer valid =
        logbridge::protocol::serializeLogMessage(message);

    ByteBuffer truncated = valid; // 模拟 Payload 末尾缺少一个字节的帧。
    truncated.pop_back();
    requireProtocolError(
        [&] {
            // 该 Lambda 尝试解析缺少末尾字节的帧。
            logbridge::protocol::deserializeLogMessage(truncated);
        },
        "truncated frame should be rejected");

    ByteBuffer invalidMagic = valid; // 模拟协议 Magic 不匹配的帧。
    invalidMagic[0] ^= 0xFF;
    requireProtocolError(
        [&] {
            // 该 Lambda 尝试解析协议标识被篡改的帧。
            logbridge::protocol::deserializeLogMessage(invalidMagic);
        },
        "invalid magic should be rejected");

    ByteBuffer invalidVersion = valid; // 模拟接收方不支持的协议版本。
    invalidVersion[4] = 99;
    requireProtocolError(
        [&] {
            // 该 Lambda 尝试解析版本号不受支持的帧。
            logbridge::protocol::deserializeLogMessage(invalidVersion);
        },
        "unsupported version should be rejected");

    ByteBuffer invalidType = valid; // 模拟协议未定义的消息类型。
    invalidType[5] = 99;
    requireProtocolError(
        [&] {
            // 该 Lambda 尝试解析消息类型未知的帧。
            logbridge::protocol::deserializeLogMessage(invalidType);
        },
        "unknown message type should be rejected");

    ByteBuffer invalidReserved = valid; // 模拟当前不允许使用的保留位。
    invalidReserved[6] = 1;
    requireProtocolError(
        [&] {
            // 该 Lambda 尝试解析保留字段非零的帧。
            logbridge::protocol::deserializeLogMessage(invalidReserved);
        },
        "non-zero reserved bits should be rejected");
}

// 验证来源和正文超过协议上限时，序列化会立即失败。
void testLengthLimits() {
    // oversizedSource 的来源字段比协议上限多一个字节。
    LogMessage oversizedSource{
        .id = 1,
        .timestampMs = 1,
        .source =
            std::string(logbridge::protocol::MaxSourceLength + 1, 's'),
        .content = "content",
    };

    requireProtocolError(
        [&] {
            // 该 Lambda 尝试序列化来源字段超长的日志。
            logbridge::protocol::serializeLogMessage(oversizedSource);
        },
        "oversized source should be rejected");

    // oversizedContent 的正文比协议上限多一个字节。
    LogMessage oversizedContent{
        .id = 1,
        .timestampMs = 1,
        .source = "app.log",
        .content =
            std::string(logbridge::protocol::MaxContentLength + 1, 'c'),
    };

    requireProtocolError(
        [&] {
            // 该 Lambda 尝试序列化正文超长的日志。
            logbridge::protocol::serializeLogMessage(oversizedContent);
        },
        "oversized content should be rejected");

    const std::vector<LogMessage> oversizedBatch( // 比批次数量上限多一条。
        logbridge::protocol::MaxBatchMessageCount + 1);
    requireProtocolError(
        [&] {
            // 该 Lambda 尝试序列化包含过多日志的批次。
            logbridge::protocol::serializeLogBatch(oversizedBatch);
        },
        "oversized batch should be rejected");

    const std::vector<LogMessage> emptyBatch; // 不含任何日志的非法批次。
    requireProtocolError(
        [&] {
            // 该 Lambda 尝试序列化空批次。
            logbridge::protocol::serializeLogBatch(emptyBatch);
        },
        "empty batch should be rejected");
}

} // namespace

// 按顺序运行所有协议测试，任一异常都会令测试程序失败。
int main() {
    try {
        testRoundTrip();
        testHeaderEncoding();
        testBatchRoundTrip();
        testAckRoundTrip();
        testInvalidFrames();
        testLengthLimits();

        std::cout << "Protocol tests passed\n";
        return 0;
    // exception 保存断言失败或意外协议异常的具体原因。
    } catch (const std::exception& exception) {
        std::cerr << "Protocol test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
