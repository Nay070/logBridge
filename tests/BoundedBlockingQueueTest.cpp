#include "BoundedBlockingQueue.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

// 检查 condition；失败时使用 message 抛出异常。
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 验证达到数量上限时无需等待超时便会立即返回。
void testBatchSizeTrigger() {
    BoundedBlockingQueue<int> queue(10); // 保存本测试使用的整数。
    queue.push(1);
    queue.push(2);
    queue.push(3);

    const auto start = std::chrono::steady_clock::now(); // 开始批量读取的时间。
    const std::vector<int> batch = queue.popBatch(3, 1s); // 应因凑满三项而返回。
    const auto elapsed = // 本次批量读取实际耗时。
        std::chrono::steady_clock::now() - start;

    require(batch == std::vector<int>{1, 2, 3},
            "batch must preserve FIFO order");
    require(elapsed < 500ms,
            "full batch should not wait for timeout");
}

// 验证不足最大数量时，会在等待期限到达后返回已有元素。
void testBatchTimeTrigger() {
    BoundedBlockingQueue<int> queue(10); // 只会放入一个元素的测试队列。
    queue.push(7);

    const auto start = std::chrono::steady_clock::now(); // 开始等待的时间。
    const std::vector<int> batch = queue.popBatch(3, 50ms); // 应由 50ms 超时触发。
    const auto elapsed = // 本次批量读取实际耗时。
        std::chrono::steady_clock::now() - start;

    require(batch == std::vector<int>{7},
            "timeout batch should contain available items");
    require(elapsed >= 30ms,
            "partial batch returned before its wait time");
}

// 验证队列关闭后会立即返回剩余元素，取完后返回空批次。
void testClosedQueueFlush() {
    BoundedBlockingQueue<int> queue(10); // 保存关闭前尚未消费的元素。
    queue.push(4);
    queue.push(5);
    queue.close();

    const std::vector<int> remaining = // 关闭后仍应被取出的剩余元素。
        queue.popBatch(10, 1s);
    const std::vector<int> empty = // 所有元素取完后的空结果。
        queue.popBatch(10, 1s);

    require(remaining == std::vector<int>{4, 5},
            "closed queue must flush remaining items");
    require(empty.empty(),
            "drained closed queue must return an empty batch");
}

} // 匿名命名空间

// 运行阻塞队列的批量读取测试。
int main() {
    try {
        testBatchSizeTrigger();
        testBatchTimeTrigger();
        testClosedQueueFlush();
        std::cout << "BoundedBlockingQueue tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "BoundedBlockingQueue test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
