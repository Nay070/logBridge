#include "Protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t TotalMessages = 200'000; // 每组测试序列化的日志总数。

// 使用指定批量大小测试协议序列化吞吐量。
void runBenchmark(std::size_t batchSize) {
    std::vector<LogMessage> batch; // 每轮重复序列化的一批测试消息。
    batch.reserve(batchSize);
    for (std::size_t index = 0; index < batchSize; ++index) { // index 是批次内消息序号。
        batch.push_back(LogMessage{
            .id = index + 1,
            .clientId = "benchmark-client",
            .timestampMs = 1'785'331'995'346,
            .source = "/var/log/example/app.log",
            .content = std::string(200, 'x'),
        });
    }

    const std::size_t iterations = TotalMessages / batchSize; // 实际执行的序列化次数。
    std::size_t encodedBytes = 0; // 所有生成协议帧的累计字节数。
    const auto begin = std::chrono::steady_clock::now(); // 当前测试的开始时间。
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        encodedBytes +=
            logbridge::protocol::serializeLogBatch(batch).size();
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin; // 总耗时。
    const double seconds = // 便于计算吞吐量的秒数。
        std::chrono::duration<double>(elapsed).count();
    const double messagesPerSecond = // 每秒完成序列化的消息数量。
        static_cast<double>(iterations * batchSize) / seconds;
    const double megabytesPerSecond = // 每秒生成的协议帧 MiB 数量。
        static_cast<double>(encodedBytes) / (1024.0 * 1024.0) / seconds;

    std::cout << "批量=" << std::setw(3) << batchSize
              << "，消息/秒=" << std::fixed << std::setprecision(0)
              << messagesPerSecond
              << "，编码吞吐 MiB/秒=" << std::setprecision(2)
              << megabytesPerSecond << '\n';
}

} // 匿名命名空间

// 分别测试不同批量大小的协议序列化性能。
int main() {
    std::cout << "LogBridge 协议序列化基准（每条正文 200 字节）\n";
    runBenchmark(1);
    runBenchmark(10);
    runBenchmark(100);
    return 0;
}
