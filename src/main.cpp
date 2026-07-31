#include "BoundedBlockingQueue.h"
#include "FileTailReader.h"
#include "LogMessage.h"
#include "TcpClient.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr std::size_t SendBatchSize = 10; // 每个网络批次最多包含的日志条数。
constexpr std::chrono::milliseconds SendBatchMaxWait{100}; // 不足一批时的最长等待时间。

// 返回当前系统时间距离 Unix 时间原点的毫秒数。
std::int64_t currentTimestampMs() {
    const auto now = std::chrono::system_clock::now(); // 获取当前系统时钟时间点。
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
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

} // namespace

// 客户端程序入口。
// argc 是命令行参数数量，argv 依次保存日志路径、服务端地址和端口。
int main(int argc, char* argv[]) {
    // logPath 是需要持续读取的日志文件路径。
    const std::string logPath = argc > 1 ? argv[1] : "app.log";
    // serverHost 是日志接收服务端的主机名或 IP 地址。
    const std::string serverHost = argc > 2 ? argv[2] : "127.0.0.1";

    std::uint16_t serverPort = 9000; // 服务端 TCP 端口，默认使用 9000。
    try {
        serverPort = parsePort(argc > 3 ? argv[3] : "9000");
    // exception 保存端口解析失败的具体原因。
    } catch (const std::exception& exception) {
        std::cerr << "端口参数错误：" << exception.what() << '\n';
        return 1;
    }

    if (!std::filesystem::exists(logPath)) {
        std::cerr << "日志文件不存在：" << logPath << '\n'
                  << "请先执行：touch " << logPath << '\n';
        return 1;
    }

    // 在启动日志线程前建立连接，连接失败时可以立即退出。
    // client 保存已连接客户端；optional 允许捕获连接异常后直接退出。
    std::optional<logbridge::TcpClient> client;
    try {
        client.emplace(serverHost, serverPort);
    // exception 保存地址解析或连接失败的具体原因。
    } catch (const std::exception& exception) {
        std::cerr << "连接服务端失败：" << exception.what() << '\n'
                  << "请先启动 logbridge_server\n";
        return 1;
    }

    BoundedBlockingQueue<LogMessage> queue(100); // 在读取线程和发送线程之间传递日志。
    std::atomic<bool> running{true};             // 跨线程通知程序是否继续运行。

    // 生产者持续读取新增日志，并转成 LogMessage。
    // producer 是负责文件读取和消息生产的后台线程。
    std::thread producer([&queue, &running, &logPath] {
        try {
            FileTailReader reader(logPath); // 记录文件偏移并增量读取完整日志行。
            std::uint64_t nextId = 1;       // 分配给下一条日志的本地消息编号。

            while (running.load()) {
                auto lines = reader.readNewLines(); // 保存本轮读取到的完整日志行。

                // line 表示本轮准备转换为消息的一条日志正文。
                for (auto& line : lines) {
                    // message 保存进入线程安全队列的一条结构化日志。
                    LogMessage message{
                        .id = nextId++,
                        .timestampMs = currentTimestampMs(),
                        .source = reader.path(),
                        .content = std::move(line),
                    };

                    if (!queue.push(std::move(message))) {
                        return;
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        // exception 保存打开或读取日志文件失败的原因。
        } catch (const std::exception& exception) {
            std::cerr << "日志读取失败：" << exception.what() << '\n';
            running.store(false);
        }

        queue.close();
    });

    // 消费者按“最多 10 条或等待 100ms”组批，发送后等待累计 ACK。
    // consumer 是负责批量取出消息、网络发送和确认校验的后台线程。
    std::thread consumer([&queue, &running, &client] {
        try {
            while (true) {
                auto batch = // 本轮按数量或等待时间组成的日志批次。
                    queue.popBatch(SendBatchSize, SendBatchMaxWait);
                if (batch.empty()) {
                    break;
                }

                const std::uint64_t confirmedId = // 服务端实际返回的累计确认 ID。
                    client->sendLogBatchAndWaitAck(batch);

                std::cout << "批次已确认：" << batch.size()
                          << " 条，最大消息 ID=" << confirmedId << '\n';
            }
        // exception 保存组批、发送、接收 ACK 或确认校验失败的原因。
        } catch (const std::exception& exception) {
            std::cerr << "日志批量发送失败：" << exception.what() << '\n';
            running.store(false);
            queue.close();
        }
    });

    std::cout << "已连接服务端：" << serverHost << ':' << serverPort << '\n'
              << "正在监听日志文件：" << logPath << '\n'
              << "按 Enter 键停止 LogBridge\n";
    std::cin.get();

    running.store(false);
    producer.join();
    consumer.join();

    std::cout << "LogBridge 已停止\n";
    return 0;
}
