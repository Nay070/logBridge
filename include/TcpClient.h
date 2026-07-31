#pragma once

#include "LogMessage.h"

#include <cstdint>
#include <string>

namespace logbridge {

// 阻塞式 TCP 客户端，负责连接服务端并发送完整日志帧。
class TcpClient {
public:
    // 连接指定服务端；host 是主机名或 IP，port 是服务端端口。
    TcpClient(const std::string& host, std::uint16_t port);

    // 关闭当前客户端持有的 Socket 文件描述符。
    ~TcpClient();

    // Socket 连接具有唯一所有权，因此禁止复制客户端。
    TcpClient(const TcpClient&) = delete;

    // 禁止通过赋值复制 Socket 文件描述符。
    TcpClient& operator=(const TcpClient&) = delete;

    // 将 message 序列化并完整发送到已经连接的服务端。
    void sendLogMessage(const LogMessage& message) const;

private:
    int socketFd_{-1}; // 与服务端通信的 Socket 文件描述符，-1 表示无效。
};

} // namespace logbridge
