#include "TcpServer.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

// 将 text 转换为合法 TCP 端口，格式或范围错误时抛出异常。
std::uint16_t parsePort(const std::string& text) {
    std::size_t parsedLength = 0; // 保存 std::stoul 实际解析的字符数量。
    const unsigned long value = std::stoul(text, &parsedLength); // 转换后的无符号整数。

    if (parsedLength != text.size() || value == 0 ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("端口必须在 1 到 65535 之间");
    }

    return static_cast<std::uint16_t>(value);
}

} // namespace

// 服务端程序入口。
// argc 是命令行参数数量，argv[1] 可指定监听端口。
int main(int argc, char* argv[]) {
    try {
        // port 是经过格式和范围校验的监听端口。
        const std::uint16_t port = parsePort(argc > 1 ? argv[1] : "9000");
        logbridge::TcpServer server(port); // 管理监听 Socket 和当前客户端连接。

        std::cout << "LogBridge 服务端正在监听端口 "
                  << server.port() << '\n';

        // 当前采用阻塞模型，一次处理一个客户端；断开后继续等待下一个。
        while (true) {
            std::cout << "等待客户端连接...\n";
            server.acceptClient();
            std::cout << "客户端已连接\n";

            try {
                // message 保存本轮从 TCP 连接中解析出的一条日志。
                while (auto message = server.receiveLogMessage()) {
                    std::cout << '[' << message->id << "] "
                              << message->timestampMs << ' '
                              << message->source << " | "
                              << message->content << '\n';
                }
                std::cout << "客户端已断开\n";
            // exception 保存当前客户端发送非法数据或网络失败的原因。
            } catch (const std::exception& exception) {
                std::cerr << "处理客户端数据失败："
                          << exception.what() << '\n';
            }
        }
    // exception 保存端口错误或服务端 Socket 启动失败的原因。
    } catch (const std::exception& exception) {
        std::cerr << "服务端启动失败：" << exception.what() << '\n';
        return 1;
    }
}
