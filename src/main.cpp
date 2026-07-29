#include "BoundedBlockingQueue.h"
#include "FileTailReader.h"
#include "LogMessage.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

std::int64_t currentTimestampMs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch())
        .count();
}

} // namespace

int main(int argc, char* argv[]) {
    const std::string logPath = argc > 1 ? argv[1] : "app.log";

    if (!std::filesystem::exists(logPath)) {
        std::cerr << "日志文件不存在：" << logPath << '\n'
                  << "请先执行：touch " << logPath << '\n';
        return 1;
    }

    BoundedBlockingQueue<LogMessage> queue(100);
    std::atomic<bool> running{true};

    // 生产者持续读取新增日志，并转成 LogMessage。
    std::thread producer([&queue, &running, &logPath] {
        try {
            FileTailReader reader(logPath);
            std::uint64_t nextId = 1;

            while (running.load()) {
                auto lines = reader.readNewLines();

                for (auto& line : lines) {
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
        } catch (const std::exception& exception) {
            std::cerr << "日志读取失败：" << exception.what() << '\n';
        }

        queue.close();
    });

    // 消费者暂时打印消息，下一阶段再替换为网络发送。
    std::thread consumer([&queue] {
        while (auto message = queue.pop()) {
            std::cout << '[' << message->id << "] "
                      << message->timestampMs << ' '
                      << message->source << " | "
                      << message->content << '\n';
        }
    });

    std::cout << "正在监听日志文件：" << logPath << '\n'
              << "按 Enter 键停止 LogBridge\n";
    std::cin.get();

    running.store(false);
    producer.join();
    consumer.join();

    std::cout << "LogBridge 已停止\n";
    return 0;
}
