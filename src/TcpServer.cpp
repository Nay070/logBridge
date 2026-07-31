#include "TcpServer.h"

#include "Protocol.h"
#include "SocketIO.h"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace logbridge {
namespace {

// 关闭 socketFd 并将它重置为 -1；无效描述符会被直接忽略。
void closeSocket(int& socketFd) noexcept {
    if (socketFd >= 0) {
        ::close(socketFd);
        socketFd = -1;
    }
}

} // namespace

// 创建 IPv4 监听 Socket，绑定 port 并开始监听连接。
TcpServer::TcpServer(std::uint16_t port) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot create server socket");
    }

    try {
        // reuseAddress 用来开启 SO_REUSEADDR，方便程序重启后立即复用端口。
        const int reuseAddress = 1;
        if (::setsockopt(
                listenFd_,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuseAddress,
                sizeof(reuseAddress)) < 0) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "cannot configure server socket");
        }

        sockaddr_in address{}; // 保存服务端要绑定的 IPv4 地址与端口。
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);

        if (::bind(
                listenFd_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) < 0) {
            throw std::system_error(
                errno, std::generic_category(), "cannot bind server socket");
        }

        if (::listen(listenFd_, 16) < 0) {
            throw std::system_error(
                errno, std::generic_category(), "cannot listen on socket");
        }

        // 测试传入 0 时由系统选择空闲端口，所以需要读取实际端口。
        // addressLength 告诉 getsockname() address 缓冲区的大小。
        socklen_t addressLength = sizeof(address);
        if (::getsockname(
                listenFd_,
                reinterpret_cast<sockaddr*>(&address),
                &addressLength) < 0) {
            throw std::system_error(
                errno, std::generic_category(), "cannot read server port");
        }
        port_ = ntohs(address.sin_port);
    } catch (...) {
        closeSocket(listenFd_);
        throw;
    }
}

// 先关闭当前客户端，再关闭监听 Socket，释放所有网络资源。
TcpServer::~TcpServer() {
    closeSocket(clientFd_);
    closeSocket(listenFd_);
}

// 阻塞等待客户端；如果旧连接仍存在，先关闭旧连接。
void TcpServer::acceptClient() {
    closeSocket(clientFd_);

    do {
        clientFd_ = ::accept(listenFd_, nullptr, nullptr);
    } while (clientFd_ < 0 && errno == EINTR);

    if (clientFd_ < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot accept client");
    }
}

// 接收一个单日志协议帧并反序列化。
std::optional<LogMessage> TcpServer::receiveLogMessage() const {
    if (clientFd_ < 0) {
        throw std::logic_error("no client has been accepted");
    }

    const auto frame = net::receiveFrame(clientFd_); // 客户端发送的完整协议帧。
    if (!frame) {
        return std::nullopt;
    }
    return protocol::deserializeLogMessage(*frame);
}

// 同时兼容旧的单日志帧和新的批次帧。
std::optional<std::vector<LogMessage>>
TcpServer::receiveLogBatch() const {
    if (clientFd_ < 0) {
        throw std::logic_error("no client has been accepted");
    }

    const auto frame = net::receiveFrame(clientFd_); // 客户端发送的完整协议帧。
    if (!frame) {
        return std::nullopt;
    }

    const protocol::FrameHeader header = // 用于判断当前是单条还是批次消息。
        protocol::parseFrameHeader(*frame);
    if (header.type == protocol::MessageType::Log) {
        std::vector<LogMessage> messages; // 将单条日志包装成统一的数组结果。
        messages.push_back(protocol::deserializeLogMessage(*frame));
        return messages;
    }
    if (header.type == protocol::MessageType::LogBatch) {
        return protocol::deserializeLogBatch(*frame);
    }

    throw protocol::ProtocolError("expected log or log batch frame");
}

// 将服务端已经处理到的最大消息 ID 返回给客户端。
void TcpServer::sendAck(std::uint64_t confirmedId) const {
    if (clientFd_ < 0) {
        throw std::logic_error("no client has been accepted");
    }

    const protocol::ByteBuffer frame = // confirmedId 对应的完整 ACK 帧。
        protocol::serializeAck(confirmedId);
    net::sendAll(clientFd_, frame);
}

// 返回 bind 后的实际端口；测试使用端口 0 时也能取得系统分配值。
std::uint16_t TcpServer::port() const noexcept {
    return port_;
}

} // namespace logbridge
