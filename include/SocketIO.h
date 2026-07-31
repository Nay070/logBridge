#pragma once

#include "Protocol.h"

#include <cstdint>
#include <optional>
#include <span>

namespace logbridge::net {

// 循环调用 send()，直到 data 中的全部字节发送完成。
void sendAll(int socketFd, std::span<const std::uint8_t> data);

// 从 TCP 字节流中读取一个完整协议帧；对端正常断开时返回 nullopt。
std::optional<protocol::ByteBuffer> receiveFrame(int socketFd);

} // namespace logbridge::net
