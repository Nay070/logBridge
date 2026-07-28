#include "BoundedBlockingQueue.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    // 容量为 3；生产快于消费，用于触发队列满时的阻塞。
    BoundedBlockingQueue<int> queue(3);

    // 模拟日志读取线程。
    std::thread producer([&queue] {
        for (int number = 1; number <= 50; ++number) {
            if (!queue.push(number)) {
                std::cout << "队列已关闭，生产者停止写入\n";
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }

        queue.close();
    });

    // 模拟网络发送线程。
    std::thread consumer([&queue] {
        while (true) {
            std::optional<int> value = queue.pop();

            if (!value.has_value()) {
                break;
            }

            std::cout << "消费者处理消息：" << *value << '\n';

            // 消费较慢，故意制造队列满的情况。
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "队列已关闭，所有消息处理完毕\n";
    });

    // 等待工作线程结束。
    producer.join();
    consumer.join();

    std::cout << "LogBridge 队列测试完成\n";
    return 0;
}
