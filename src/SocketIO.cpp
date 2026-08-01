#include "SocketIO.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>

namespace logbridge::net {
namespace {

// 循环读取并填满 output；读取任何字节前收到 EOF 时返回 false。
bool receiveExact(int socketFd, std::span<std::uint8_t> output) {
    std::size_t receivedBytes = 0; // 已经放入 output 的字节数。

    while (receivedBytes < output.size()) {
        const ssize_t result = // 本次 recv() 实际收到的字节数。
            ::recv(
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
            errno, std::generic_category(), "cannot receive protocol data");
    }

    return true;
}

} // 匿名命名空间

void sendAll(int socketFd, std::span<const std::uint8_t> data) {
    std::size_t sentBytes = 0; // 已经成功交给内核的字节数。

    while (sentBytes < data.size()) {
        const ssize_t result = // 本次 send() 实际发送的字节数。
            ::send(
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

        const int error = result == 0 ? EPIPE : errno; // 用于异常的系统错误码。
        throw std::system_error(
            error, std::generic_category(), "cannot send protocol data");
    }
}

std::optional<protocol::ByteBuffer> receiveFrame(int socketFd) {
    // headerBytes 保存从 TCP 流中读取的固定帧头。
    std::array<std::uint8_t, protocol::FrameHeaderSize> headerBytes{};
    if (!receiveExact(socketFd, headerBytes)) {
        return std::nullopt;
    }

    const protocol::FrameHeader header = // 解析出的消息类型和 Payload 长度。
        protocol::parseFrameHeader(headerBytes);
    protocol::ByteBuffer frame( // 为一帧完整数据分配连续内存。
        protocol::FrameHeaderSize + header.payloadLength);
    std::copy(headerBytes.begin(), headerBytes.end(), frame.begin());

    std::span<std::uint8_t> payload( // 指向 frame 中等待填充的 Payload 区域。
        frame.data() + protocol::FrameHeaderSize,
        header.payloadLength);
    if (!receiveExact(socketFd, payload)) {
        throw std::runtime_error(
            "connection closed before the frame payload");
    }

    return frame;
}

} // logbridge::net 命名空间
