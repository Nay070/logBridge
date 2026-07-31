#include "TcpClient.h"
#include "TcpServer.h"

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

// 在本机回环地址上启动真实 TCP 客户端和服务端，验证多条消息传输。
void testClientServerRoundTrip() {
    // 端口 0 表示让操作系统自动选择一个当前空闲端口。
    logbridge::TcpServer server(0);       // 管理测试使用的监听和客户端 Socket。
    std::vector<LogMessage> received;    // 保存服务端实际接收到的全部日志。
    std::exception_ptr serverError;      // 跨线程保存服务端出现的异常。

    // serverThread 模拟服务端，接收消息直到客户端关闭连接。
    std::thread serverThread([&] {
        try {
            server.acceptClient();
            // message 保存服务端本轮解析出的一条日志。
            while (auto message = server.receiveLogMessage()) {
                received.push_back(std::move(*message));
            }
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

    {
        // client 连接测试服务端，并在作用域结束时主动关闭连接。
        logbridge::TcpClient client("127.0.0.1", server.port());
        // message 表示客户端本轮准备发送的一条日志。
        for (const auto& message : sent) {
            client.sendLogMessage(message);
        }
    } // 客户端析构并关闭连接，服务端随后收到 EOF。

    serverThread.join();
    if (serverError) {
        std::rethrow_exception(serverError);
    }

    require(received.size() == sent.size(),
            "received message count mismatch");
    // index 表示当前正在比较第几条发送和接收消息。
    for (std::size_t index = 0; index < sent.size(); ++index) {
        requireSameMessage(received[index], sent[index]);
    }
}

} // namespace

// 运行 TCP 端到端测试，并用退出码向 CTest 报告结果。
int main() {
    try {
        testClientServerRoundTrip();
        std::cout << "TCP transport tests passed\n";
        return 0;
    // exception 保存网络、协议或断言失败的具体原因。
    } catch (const std::exception& exception) {
        std::cerr << "TCP transport test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
