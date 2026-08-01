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
void closeSocket(std::atomic<int>& socketFd) noexcept {
    const int descriptor = socketFd.exchange(-1); // 当前线程取得并负责关闭的描述符。
    if (descriptor >= 0) {
        ::shutdown(descriptor, SHUT_RDWR);
        ::close(descriptor);
    }
}

} // 匿名命名空间

TcpServer::TcpServer(std::uint16_t port) {
    listenFd_.store(::socket(AF_INET, SOCK_STREAM, 0));
    if (listenFd_.load() < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot create server socket");
    }

    try {
        // reuseAddress 用来开启 SO_REUSEADDR，方便程序重启后立即复用端口。
        const int reuseAddress = 1;
        if (::setsockopt(
                listenFd_.load(),
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
                listenFd_.load(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) < 0) {
            throw std::system_error(
                errno, std::generic_category(), "cannot bind server socket");
        }

        if (::listen(listenFd_.load(), 16) < 0) {
            throw std::system_error(
                errno, std::generic_category(), "cannot listen on socket");
        }

        // 测试传入 0 时由系统选择空闲端口，所以需要读取实际端口。
        // addressLength 告诉 getsockname() address 缓冲区的大小。
        socklen_t addressLength = sizeof(address);
        if (::getsockname(
                listenFd_.load(),
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

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::acceptClient() {
    closeSocket(clientFd_);
    if (stopped_.load()) {
        return false;
    }

    const int listenFd = listenFd_.load(); // 本次 accept 使用的监听描述符。
    int acceptedFd = -1; // 本次接收到的客户端描述符。
    do {
        acceptedFd = ::accept(listenFd, nullptr, nullptr);
    } while (acceptedFd < 0 && errno == EINTR && !stopped_.load());

    if (acceptedFd < 0) {
        if (stopped_.load()) {
            return false;
        }
        throw std::system_error(
            errno, std::generic_category(), "cannot accept client");
    }

    clientFd_.store(acceptedFd);
    if (stopped_.load()) {
        closeSocket(clientFd_);
        return false;
    }
    return true;
}

void TcpServer::stop() noexcept {
    stopped_.store(true);
    closeSocket(clientFd_);
    closeSocket(listenFd_);
}

std::optional<LogMessage> TcpServer::receiveLogMessage() const {
    const int clientFd = clientFd_.load(); // 当前接收操作使用的客户端描述符。
    if (clientFd < 0) {
        throw std::logic_error("no client has been accepted");
    }

    const auto frame = net::receiveFrame(clientFd); // 客户端发送的完整协议帧。
    if (!frame) {
        return std::nullopt;
    }
    return protocol::deserializeLogMessage(*frame);
}

std::optional<std::vector<LogMessage>>
TcpServer::receiveLogBatch() const {
    const int clientFd = clientFd_.load(); // 当前接收操作使用的客户端描述符。
    if (clientFd < 0) {
        throw std::logic_error("no client has been accepted");
    }

    const auto frame = net::receiveFrame(clientFd); // 客户端发送的完整协议帧。
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

void TcpServer::sendAck(std::uint64_t confirmedId) const {
    const int clientFd = clientFd_.load(); // 当前发送 ACK 使用的客户端描述符。
    if (clientFd < 0) {
        throw std::logic_error("no client has been accepted");
    }

    const protocol::ByteBuffer frame = // confirmedId 对应的完整 ACK 帧。
        protocol::serializeAck(confirmedId);
    net::sendAll(clientFd, frame);
}

std::uint16_t TcpServer::port() const noexcept {
    return port_;
}

} // logbridge 命名空间
