#include "DedupStore.h"
#include "ShutdownSignal.h"
#include "TcpServer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

// 保存服务端运行期间的累计指标。
struct ServerMetrics {
    std::uint64_t connections{0}; // 已接受的客户端连接数。
    std::uint64_t batches{0}; // 已确认的日志批次数。
    std::uint64_t receivedMessages{0}; // 收到的日志消息总数。
    std::uint64_t processedMessages{0}; // 实际处理的新日志数量。
    std::uint64_t duplicateMessages{0}; // 被去重机制跳过的日志数量。
};

// 输出服务端当前累计指标。
void printMetrics(const ServerMetrics& metrics) {
    std::cout << "[指标] 连接=" << metrics.connections
              << "，批次=" << metrics.batches
              << "，收到=" << metrics.receivedMessages
              << "，有效=" << metrics.processedMessages
              << "，重复=" << metrics.duplicateMessages << '\n';
}

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

} // 匿名命名空间

// 服务端程序入口。
// argc 是命令行参数数量，argv[1] 可指定监听端口。
int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf); // 及时刷新服务端输出，便于监控和测试观察。
    try {
        // port 是经过格式和范围校验的监听端口。
        const std::uint16_t port = parsePort(argc > 1 ? argv[1] : "9000");
        const std::string dedupPath = // 服务端持久化确认水位的文件。
            argc > 2 ? argv[2] : ".logbridge-server/dedup.state";
        logbridge::ShutdownSignal shutdownSignal; // 同步接收 Ctrl+C 和 SIGTERM。
        logbridge::TcpServer server(port); // 管理监听 Socket 和当前客户端连接。
        logbridge::DedupStore dedupStore(dedupPath); // 管理客户端去重水位。
        std::atomic<bool> running{true}; // 跨线程停止标志。
        ServerMetrics metrics; // 服务端本次运行的累计指标。

        // 信号线程收到退出请求后关闭 Socket，使 accept 或 recv 立即返回。
        std::jthread signalThread([&](std::stop_token stopToken) {
            try {
                while (!stopToken.stop_requested()) {
                    if (shutdownSignal.waitFor(std::chrono::milliseconds(100))) {
                        running.store(false);
                        server.stop();
                        return;
                    }
                }
            // exception 保存同步等待信号失败的原因。
            } catch (const std::exception& exception) {
                std::cerr << "等待退出信号失败：" << exception.what() << '\n';
                running.store(false);
                server.stop();
            }
        });

        std::cout << "LogBridge 服务端正在监听端口 "
                  << server.port() << '\n';

        // 当前采用阻塞模型，一次处理一个客户端；断开后继续等待下一个。
        while (running.load()) {
            std::cout << "等待客户端连接...\n";
            if (!server.acceptClient()) {
                break;
            }
            ++metrics.connections;
            std::cout << "客户端已连接\n";

            try {
                // messages 保存本轮从单日志帧或批次帧中解析出的日志。
                while (running.load()) {
                    auto messages = server.receiveLogBatch();
                    if (!messages) {
                        break;
                    }
                    metrics.receivedMessages += messages->size();
                    const std::string& clientId = // 当前批次所属的客户端。
                        messages->front().clientId;
                    const std::uint64_t previousConfirmed = // 服务端已有确认水位。
                        dedupStore.highestConfirmed(clientId);
                    std::uint64_t latestProcessed = previousConfirmed; // 本批次处理后的水位。

                    // message 表示当前正在处理的一条日志。
                    for (const LogMessage& message : *messages) {
                        if (message.id <= previousConfirmed) {
                            ++metrics.duplicateMessages;
                            std::cout << "跳过重复日志：client=" << clientId
                                      << " id=" << message.id << '\n';
                            continue;
                        }

                        std::cout << '[' << message.id << "] "
                                  << message.timestampMs << ' '
                                  << message.source << " | "
                                  << message.content << '\n';
                        ++metrics.processedMessages;
                        latestProcessed = message.id;
                    }

                    // 先持久化去重水位，再返回 ACK。
                    const std::uint64_t persistedWatermark =
                        dedupStore.confirm(clientId, latestProcessed);
                    const std::uint64_t confirmedId =
                        messages->back().id; // ACK 精确对应客户端当前批次。
                    server.sendAck(confirmedId);
                    ++metrics.batches;
                    std::cout << "已确认批次：" << messages->size()
                              << " 条，消息 ID=" << confirmedId
                              << "，去重水位=" << persistedWatermark << '\n';
                    printMetrics(metrics);
                }
                if (running.load()) {
                    std::cout << "客户端已断开\n";
                }
            // exception 保存当前客户端发送非法数据或网络失败的原因。
            } catch (const std::exception& exception) {
                if (running.load()) {
                    std::cerr << "处理客户端数据失败："
                              << exception.what() << '\n';
                }
            }
        }

        signalThread.request_stop();
        printMetrics(metrics);
        std::cout << "LogBridge 服务端已安全停止\n";
        return 0;
    // exception 保存端口错误或服务端 Socket 启动失败的原因。
    } catch (const std::exception& exception) {
        std::cerr << "服务端启动失败：" << exception.what() << '\n';
        return 1;
    }
}
