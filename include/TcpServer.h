#pragma once

#include "LogMessage.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

namespace logbridge {

// 使用阻塞 Socket，一次处理一个客户端连接，并支持从其他线程停止阻塞操作。
class TcpServer {
public:
    // 创建、绑定并监听指定端口；port 为 0 时由操作系统选择空闲端口。
    explicit TcpServer(std::uint16_t port);

    // 关闭客户端连接和监听 Socket。
    ~TcpServer();

    // Socket 具有唯一所有权，因此禁止复制服务端。
    TcpServer(const TcpServer&) = delete;

    // 禁止通过赋值复制 Socket 文件描述符。
    TcpServer& operator=(const TcpServer&) = delete;

    // 阻塞等待一个客户端连接；服务器已停止时返回 false。
    bool acceptClient();

    // 关闭监听和客户端 Socket，使阻塞中的网络操作及时结束。
    void stop() noexcept;

    // 接收并解析一条完整日志；客户端正常断开时返回 nullopt。
    std::optional<LogMessage> receiveLogMessage() const;

    // 接收单日志帧或批次帧，并统一返回一个日志数组。
    std::optional<std::vector<LogMessage>> receiveLogBatch() const;

    // 向当前客户端发送累计确认 ID。
    void sendAck(std::uint64_t confirmedId) const;

    // 返回服务端实际监听的端口号。
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    std::atomic<int> listenFd_{-1}; // 负责监听新连接的 Socket 文件描述符。
    std::atomic<int> clientFd_{-1}; // 当前已连接客户端的 Socket 文件描述符。
    std::atomic<bool> stopped_{false}; // 标记服务器是否已收到停止请求。
    std::uint16_t port_{};   // bind 成功后实际使用的端口号。
};

} // logbridge 命名空间
