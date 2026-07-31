#include "TcpClient.h"

#include "Protocol.h"

#include <cerrno>
#include <cstddef>
#include <netdb.h>
#include <span>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>

namespace logbridge {
namespace {

// 解析 host 和 port，依次尝试候选地址并返回已连接的 Socket 描述符。
int connectToServer(const std::string& host, std::uint16_t port) {
    addrinfo hints{}; // 向 getaddrinfo 描述需要查找哪种地址。
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr; // 指向 getaddrinfo 返回的候选地址链表。
    const std::string service = std::to_string(port); // 端口对应的服务字符串。
    // result 保存域名解析函数的返回状态，0 表示成功。
    const int result =
        ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (result != 0) {
        throw std::runtime_error(
            "cannot resolve server address: " +
            std::string(::gai_strerror(result)));
    }

    int connectedFd = -1;          // 最终成功连接的 Socket，-1 表示尚未成功。
    int lastError = ECONNREFUSED;  // 保存最后一次连接失败的 errno。

    // 域名可能解析出多个地址，依次尝试直到连接成功。
    // address 指向当前正在尝试的候选地址。
    for (addrinfo* address = addresses;
         address != nullptr;
         address = address->ai_next) {
        // socketFd 是为当前候选地址创建的临时 Socket。
        const int socketFd = ::socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol);
        if (socketFd < 0) {
            lastError = errno;
            continue;
        }

        int connectResult = -1; // 保存 connect() 的结果，0 表示连接成功。
        do {
            connectResult = ::connect(
                socketFd, address->ai_addr, address->ai_addrlen);
        } while (connectResult < 0 && errno == EINTR);

        if (connectResult == 0) {
            connectedFd = socketFd;
            break;
        }

        lastError = errno;
        ::close(socketFd);
    }

    ::freeaddrinfo(addresses);

    if (connectedFd < 0) {
        throw std::system_error(
            lastError, std::generic_category(), "cannot connect to server");
    }

    return connectedFd;
}

// 通过 socketFd 发送 data 中的全部字节，中途被信号打断时继续发送。
void sendAll(int socketFd, std::span<const std::uint8_t> data) {
    std::size_t sentBytes = 0; // 记录已经成功交给内核的字节数。

    // send() 可能只发送一部分数据，因此必须循环直到整帧发完。
    while (sentBytes < data.size()) {
        // result 是本次 send() 实际发送的字节数，负数表示失败。
        const ssize_t result = ::send(
            socketFd,
            data.data() + sentBytes,
            data.size() - sentBytes,
            MSG_NOSIGNAL);

        if (result > 0) {
            sentBytes += static_cast<std::size_t>(result);
            continue;
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        // error 保存用于构造异常的系统错误码。
        const int error = result == 0 ? EPIPE : errno;
        throw std::system_error(
            error, std::generic_category(), "cannot send log frame");
    }
}

} // namespace

// 连接 host:port，并取得该连接的唯一所有权。
TcpClient::TcpClient(const std::string& host, std::uint16_t port)
    : socketFd_(connectToServer(host, port)) {
}

// 对象销毁时关闭 Socket，避免文件描述符泄漏。
TcpClient::~TcpClient() {
    if (socketFd_ >= 0) {
        ::close(socketFd_);
    }
}

// 将 message 序列化成协议帧，再确保所有字节都发送完成。
void TcpClient::sendLogMessage(const LogMessage& message) const {
    // frame 保存 message 序列化后可直接发送的完整字节流。
    const protocol::ByteBuffer frame =
        protocol::serializeLogMessage(message);
    sendAll(socketFd_, frame);
}

} // namespace logbridge
