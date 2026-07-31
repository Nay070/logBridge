#include "TcpClient.h"
#include "TcpServer.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 检查 condition；失败时使用 message 抛出异常终止当前测试。
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 比较 actual 与 expected 的每个字段，确认网络传输没有改变消息。
void requireSameMessage(const LogMessage& actual,
                        const LogMessage& expected) {
    require(actual.id == expected.id, "message id mismatch");
    require(actual.timestampMs == expected.timestampMs,
            "timestamp mismatch");
    require(actual.source == expected.source, "source mismatch");
    require(actual.content == expected.content, "content mismatch");
}

// 在本机回环地址上验证批次发送和累计 ACK。
void testClientServerRoundTrip() {
    // 端口 0 表示让操作系统自动选择一个当前空闲端口。
    logbridge::TcpServer server(0);       // 管理测试使用的监听和客户端 Socket。
    std::vector<LogMessage> received;    // 保存服务端实际接收到的全部日志。
    std::exception_ptr serverError;      // 跨线程保存服务端出现的异常。

    // serverThread 模拟服务端，接收一个批次并确认最后一个消息 ID。
    std::thread serverThread([&] {
        try {
            server.acceptClient();
            auto batch = server.receiveLogBatch(); // 客户端发送的完整日志批次。
            if (!batch || batch->empty()) {
                throw std::runtime_error("server did not receive log batch");
            }

            received = std::move(*batch);
            server.sendAck(received.back().id);
        } catch (...) {
            serverError = std::current_exception();
        }
    });

    // sent 保存客户端应该发送、服务端最终应该完整收到的消息。
    const std::vector<LogMessage> sent{
        LogMessage{
            .id = 1,
            .timestampMs = 1000,
            .source = "/var/log/app.log",
            .content = "第一条日志",
        },
        LogMessage{
            .id = 2,
            .timestampMs = 2000,
            .source = "/var/log/app.log",
            // 较大的消息可验证 recv() 循环读取不完整数据。
            .content = std::string(256 * 1024, 'x'),
        },
        LogMessage{
            .id = 3,
            .timestampMs = 3000,
            .source = "worker.log",
            .content = "third message",
        },
    };

    std::uint64_t confirmedId = 0; // 客户端最终收到的累计确认 ID。
    {
        // client 连接测试服务端，并在作用域结束时主动关闭连接。
        logbridge::TcpClient client("127.0.0.1", server.port());
        confirmedId = client.sendLogBatchAndWaitAck(sent);
    } // 客户端收到 ACK 后析构并关闭连接。

    serverThread.join();
    if (serverError) {
        std::rethrow_exception(serverError);
    }

    require(received.size() == sent.size(),
            "received message count mismatch");
    require(confirmedId == sent.back().id,
            "cumulative ack id mismatch");
    // index 表示当前正在比较第几条发送和接收消息。
    for (std::size_t index = 0; index < sent.size(); ++index) {
        requireSameMessage(received[index], sent[index]);
    }
}

// 验证服务端确认了错误 ID 时，客户端不会把批次当作成功。
void testMismatchedAckRejected() {
    logbridge::TcpServer server(0); // 使用系统分配的独立测试端口。
    std::exception_ptr serverError; // 保存服务端线程中的异常。

    std::thread serverThread([&] {
        try {
            server.acceptClient();
            auto batch = server.receiveLogBatch(); // 客户端发送的一条测试日志。
            if (!batch || batch->empty()) {
                throw std::runtime_error("server did not receive test batch");
            }
            server.sendAck(batch->back().id + 1); // 故意返回错误确认 ID。
        } catch (...) {
            serverError = std::current_exception();
        }
    });

    const std::vector<LogMessage> sent{ // 用于错误 ACK 测试的单条消息批次。
        LogMessage{
            .id = 20,
            .timestampMs = 4000,
            .source = "app.log",
            .content = "ack mismatch",
        },
    };

    bool rejected = false; // 记录客户端是否正确拒绝错误 ACK。
    {
        logbridge::TcpClient client("127.0.0.1", server.port());
        try {
            client.sendLogBatchAndWaitAck(sent);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
    }

    serverThread.join();
    if (serverError) {
        std::rethrow_exception(serverError);
    }
    require(rejected, "client must reject mismatched ack id");
}

} // namespace

// 运行 TCP 端到端测试，并用退出码向 CTest 报告结果。
int main() {
    try {
        testClientServerRoundTrip();
        testMismatchedAckRejected();
        std::cout << "TCP transport tests passed\n";
        return 0;
    // exception 保存网络、协议或断言失败的具体原因。
    } catch (const std::exception& exception) {
        std::cerr << "TCP transport test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
