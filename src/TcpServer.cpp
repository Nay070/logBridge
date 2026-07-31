#include "TcpServer.h"

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <system_error>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace logbridge {
namespace {

// 从 socketFd 循环读取数据，直到填满 output。
// 返回 false 表示对端在本次读取任何字节前正常关闭了连接。
bool receiveExact(int socketFd, std::span<std::uint8_t> output) {
    std::size_t receivedBytes = 0; // 记录当前已经放入 output 的字节数。

    // TCP 是字节流，recv() 一次不保证得到完整帧。
    while (receivedBytes < output.size()) {
        // result 是本次 recv() 收到的字节数，0 表示对端正常关闭。
        const ssize_t result = ::recv(
            socketFd,
            output.data() + receivedBytes,
            output.size() - receivedBytes,
            0);

        if (result > 0) {
            receivedBytes += static_cast<std::size_t>(result);
            continue;
        }

        if (result == 0) {
            if (receivedBytes == 0) {
                return false;
            }
            throw std::runtime_error(
                "connection closed in the middle of a frame");
        }

        if (errno == EINTR) {
            continue;
        }

        throw std::system_error(
            errno, std::generic_category(), "cannot receive log frame");
    }

    return true;
}

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

// 先读取固定帧头，再按头部声明的长度读取 Payload 并反序列化。
std::optional<LogMessage> TcpServer::receiveLogMessage() const {
    if (clientFd_ < 0) {
        throw std::logic_error("no client has been accepted");
    }

    // headerBytes 保存从 TCP 流中读取的固定 12 字节帧头。
    std::array<std::uint8_t, protocol::FrameHeaderSize> headerBytes{};
    if (!receiveExact(clientFd_, headerBytes)) {
        return std::nullopt;
    }

    // 先读取固定帧头，再根据其中的长度读取恰好一个 Payload。
    // header 保存解析后的版本、消息类型和 Payload 长度。
    const protocol::FrameHeader header =
        protocol::parseFrameHeader(headerBytes);
    // frame 为帧头和 Payload 预留空间，最终保存一帧完整数据。
    protocol::ByteBuffer frame(
        protocol::FrameHeaderSize + header.payloadLength);
    std::copy(headerBytes.begin(), headerBytes.end(), frame.begin());

    // payload 是 frame 中帧头之后的可写区域，不拥有独立内存。
    std::span<std::uint8_t> payload(
        frame.data() + protocol::FrameHeaderSize,
        header.payloadLength);
    if (!receiveExact(clientFd_, payload)) {
        throw std::runtime_error(
            "connection closed before the frame payload");
    }

    return protocol::deserializeLogMessage(frame);
}

// 返回 bind 后的实际端口；测试使用端口 0 时也能取得系统分配值。
std::uint16_t TcpServer::port() const noexcept {
    return port_;
}

} // namespace logbridge
