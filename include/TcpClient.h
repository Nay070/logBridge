#pragma once

#include "LogMessage.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace logbridge {

// 阻塞式 TCP 客户端，负责连接服务端并发送完整日志帧。
class TcpClient {
public:
    // 连接服务端，并为发送和 ACK 接收设置超时。
    TcpClient(
        const std::string& host,
        std::uint16_t port,
        std::chrono::milliseconds ioTimeout = std::chrono::seconds(3));

    // 关闭当前客户端持有的 Socket 文件描述符。
    ~TcpClient();

    // Socket 连接具有唯一所有权，因此禁止复制客户端。
    TcpClient(const TcpClient&) = delete;

    // 禁止通过赋值复制 Socket 文件描述符。
    TcpClient& operator=(const TcpClient&) = delete;

    // 将 message 序列化并完整发送到已经连接的服务端。
    void sendLogMessage(const LogMessage& message) const;

    // 将多条日志合并为一个批次协议帧并发送。
    void sendLogBatch(std::span<const LogMessage> messages) const;

    // 阻塞等待服务端 ACK，并返回其中的累计确认 ID。
    std::uint64_t receiveAck() const;

    // 发送整个批次并校验 ACK 必须等于批次最后一个消息 ID。
    std::uint64_t sendLogBatchAndWaitAck(
        std::span<const LogMessage> messages) const;

private:
    int socketFd_{-1}; // 与服务端通信的 Socket 文件描述符，-1 表示无效。
};

} // logbridge 命名空间
