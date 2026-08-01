#include "BoundedBlockingQueue.h"
#include "ClientState.h"
#include "FileTailReader.h"
#include "ShutdownSignal.h"
#include "TcpClient.h"
#include "WriteAheadLog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t SendBatchSize = 10; // 每个网络批次最多包含的日志条数。
constexpr std::chrono::milliseconds SendBatchMaxWait{100}; // 组批最长等待时间。
constexpr std::chrono::seconds AckTimeout{3}; // 网络发送和 ACK 接收超时。
constexpr std::chrono::milliseconds InitialRetryDelay{500}; // 首次重连等待时间。
constexpr std::chrono::seconds MaxRetryDelay{8}; // 重连等待时间上限。
constexpr std::chrono::seconds MetricsInterval{5}; // 运行指标输出间隔。

// 保存客户端运行期间的累计指标。
struct ClientMetrics {
    std::atomic<std::uint64_t> readMessages{0}; // 从日志文件读取的完整日志数。
    std::atomic<std::uint64_t> walMessages{0}; // 本次运行新写入 WAL 的消息数。
    std::atomic<std::uint64_t> confirmedMessages{0}; // 收到正确 ACK 的消息数。
    std::atomic<std::uint64_t> confirmedBatches{0}; // 收到正确 ACK 的批次数。
    std::atomic<std::uint64_t> retries{0}; // 网络失败后的重试次数。
};

// 输出客户端当前累计指标和积压情况。
void printMetrics(const ClientMetrics& metrics,
                  std::size_t queueSize,
                  std::size_t walPending) {
    std::cout << "[指标] 已读取=" << metrics.readMessages.load()
              << "，已写 WAL=" << metrics.walMessages.load()
              << "，已确认=" << metrics.confirmedMessages.load()
              << "，确认批次=" << metrics.confirmedBatches.load()
              << "，重试=" << metrics.retries.load()
              << "，队列积压=" << queueSize
              << "，WAL 待确认=" << walPending << '\n';
}

// 返回当前毫秒级 Unix 时间戳。
std::int64_t currentTimestampMs() {
    const auto now = std::chrono::system_clock::now(); // 当前系统时间点。
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
}

// 将文本转换为合法 TCP 端口。
std::uint16_t parsePort(const std::string& text) {
    std::size_t parsedLength = 0; // 成功解析的字符数。
    const unsigned long value = std::stoul(text, &parsedLength); // 解析出的端口值。
    if (parsedLength != text.size() || value == 0 ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("端口必须在 1 到 65535 之间");
    }
    return static_cast<std::uint16_t>(value);
}

// 分段等待重试，使停止请求可以及时中断退避。
bool waitForRetry(const std::atomic<bool>& running,
                  std::chrono::milliseconds delay) {
    const auto deadline = std::chrono::steady_clock::now() + delay; // 结束等待时间。
    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto remaining = deadline - std::chrono::steady_clock::now(); // 剩余时间。
        const auto maximumSlice = // 单次睡眠上限，使用与 steady_clock 相同的精度。
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(100));
        std::this_thread::sleep_for(
            std::min(remaining, maximumSlice));
    }
    return running.load();
}

// 发送同一批消息直到收到正确 ACK，或程序要求停止。
bool deliverWithRetry(
    std::optional<logbridge::TcpClient>& client,
    std::span<const LogMessage> batch,
    const std::string& host,
    std::uint16_t port,
    logbridge::WriteAheadLog& wal,
    const std::atomic<bool>& running,
    ClientMetrics& metrics) {
    auto retryDelay = InitialRetryDelay; // 当前失败后需要等待的时间。

    while (running.load()) {
        try {
            if (!client) {
                client.emplace(host, port, AckTimeout);
                std::cout << "已连接服务端：" << host << ':' << port << '\n';
            }

            const std::uint64_t confirmedId =
                client->sendLogBatchAndWaitAck(batch); // 服务端确认的批次末尾 ID。
            wal.confirm(confirmedId);
            metrics.confirmedMessages.fetch_add(
                batch.size(), std::memory_order_relaxed);
            metrics.confirmedBatches.fetch_add(1, std::memory_order_relaxed);
            std::cout << "批次已确认：" << batch.size()
                      << " 条，最大消息 ID=" << confirmedId << '\n';
            return true;
        } catch (const std::exception& exception) {
            client.reset();
            metrics.retries.fetch_add(1, std::memory_order_relaxed);
            std::cerr << "发送失败，将在 " << retryDelay.count()
                      << "ms 后重试：" << exception.what() << '\n';
            if (!waitForRetry(running, retryDelay)) {
                return false;
            }
            retryDelay = std::min(
                retryDelay * 2,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    MaxRetryDelay));
        }
    }
    return false;
}

} // 匿名命名空间

// 启动日志采集、持久化和可靠发送流程。
int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf); // 及时刷新状态输出，便于服务管理和测试观察。
    const std::string logPath = argc > 1 ? argv[1] : "app.log"; // 日志文件路径。
    const std::string serverHost = argc > 2 ? argv[2] : "127.0.0.1"; // 服务端地址。
    const std::filesystem::path dataDirectory =
        argc > 4 ? argv[4] : ".logbridge-data"; // 客户端持久化目录。

    std::uint16_t serverPort = 9000; // 服务端端口。
    try {
        serverPort = parsePort(argc > 3 ? argv[3] : "9000");
    } catch (const std::exception& exception) {
        std::cerr << "端口参数错误：" << exception.what() << '\n';
        return 1;
    }

    if (!std::filesystem::exists(logPath)) {
        std::cerr << "日志文件不存在：" << logPath << '\n'
                  << "请先执行：touch " << logPath << '\n';
        return 1;
    }

    try {
        const std::filesystem::path walPath =
            dataDirectory / "pending.wal"; // 未确认消息 WAL。
        const std::filesystem::path statePath =
            dataDirectory / "client.state"; // 客户端身份和检查点文件。

        logbridge::WriteAheadLog wal(walPath);
        const std::vector<LogMessage> recovered = wal.pending(); // 启动时恢复的消息。
        logbridge::ClientState state(statePath, logPath, recovered);
        logbridge::ShutdownSignal shutdownSignal; // 同步接收 Ctrl+C 和 SIGTERM。
        BoundedBlockingQueue<LogMessage> queue(100); // 文件线程到发送线程的队列。
        std::atomic<bool> running{true}; // 跨线程停止标志。
        ClientMetrics metrics; // 客户端本次运行的累计指标。

        // 生产者先持久化日志，再更新文件检查点并写入发送队列。
        std::thread producer([&] {
            try {
                FileTailReader reader(
                    logPath,
                    state.fileOffset(),
                    state.fileDeviceId(),
                    state.fileInode());
                while (running.load()) {
                    auto lines = reader.readNewLines(); // 本轮新增的完整日志行。
                    metrics.readMessages.fetch_add(
                        lines.size(), std::memory_order_relaxed);
                    std::vector<LogMessage> messages; // 已写入 WAL、等待入队的消息。
                    messages.reserve(lines.size());

                    for (std::string& line : lines) {
                        LogMessage message{
                            .id = state.nextMessageId(),
                            .clientId = state.clientId(),
                            .timestampMs = currentTimestampMs(),
                            .source = reader.path(),
                            .content = std::move(line),
                        };
                        wal.append(message);
                        metrics.walMessages.fetch_add(
                            1, std::memory_order_relaxed);
                        messages.push_back(std::move(message));
                    }

                    state.updateFileCheckpoint(
                        reader.committedOffset(),
                        reader.deviceId(),
                        reader.inode());
                    for (LogMessage& message : messages) {
                        if (!queue.push(std::move(message))) {
                            return;
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } catch (const std::exception& exception) {
                std::cerr << "日志采集失败：" << exception.what() << '\n';
                running.store(false);
            }
            queue.close();
        });

        // 消费者先重放 WAL，再发送运行期间产生的新消息。
        std::thread consumer([&] {
            std::optional<logbridge::TcpClient> client; // 可在失败后重建的连接。

            auto sendMessages = [&](std::span<const LogMessage> messages) {
                std::size_t offset = 0; // 下一批待发送消息的起点。
                while (offset < messages.size()) {
                    const std::size_t count =
                        std::min(SendBatchSize, messages.size() - offset); // 本批数量。
                    if (!deliverWithRetry(
                            client,
                            messages.subspan(offset, count),
                            serverHost,
                            serverPort,
                            wal,
                            running,
                            metrics)) {
                        return false;
                    }
                    offset += count;
                }
                return true;
            };

            if (!recovered.empty()) {
                std::cout << "从 WAL 恢复 " << recovered.size()
                          << " 条未确认日志\n";
                if (!sendMessages(recovered)) {
                    queue.close();
                    return;
                }
            }

            while (running.load()) {
                auto batch = queue.popBatch(
                    SendBatchSize, SendBatchMaxWait); // 数量或时间触发的批次。
                if (batch.empty()) {
                    break;
                }
                if (!sendMessages(batch)) {
                    queue.close();
                    return;
                }
            }
        });

        // 指标线程定期展示吞吐和积压，不参与业务处理。
        std::thread metricsReporter([&] {
            while (waitForRetry(running, MetricsInterval)) {
                printMetrics(metrics, queue.size(), wal.pendingCount());
            }
        });

        std::cout << "客户端 ID：" << state.clientId() << '\n'
                  << "正在监听日志文件：" << logPath << '\n'
                  << "持久化目录：" << dataDirectory << '\n'
                  << "按 Ctrl+C 停止 LogBridge\n";

        while (running.load()) {
            if (shutdownSignal.waitFor(std::chrono::milliseconds(100))) {
                std::cout << "收到退出信号，正在安全停止...\n";
                running.store(false);
                break;
            }
        }

        running.store(false);
        queue.close();
        producer.join();
        consumer.join();
        metricsReporter.join();

        printMetrics(metrics, queue.size(), wal.pendingCount());
        std::cout << "LogBridge 已停止，未确认日志会在下次启动时恢复\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "LogBridge 启动失败：" << exception.what() << '\n';
        return 1;
    }
}
