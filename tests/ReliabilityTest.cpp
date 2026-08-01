#include "ClientState.h"
#include "DedupStore.h"
#include "WriteAheadLog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// 为每次测试创建并自动清理独立临时目录。
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto unique = // 避免并行测试发生目录重名。
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("logbridge-reliability-" + std::to_string(unique));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored; // 清理失败不影响测试异常传播。
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_; // 本次测试使用的临时目录。
};

// 检查条件，失败时抛出测试异常。
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 创建具有指定身份和编号的测试日志。
LogMessage makeMessage(const std::string& clientId, std::uint64_t id) {
    return LogMessage{
        .id = id,
        .clientId = clientId,
        .timestampMs = static_cast<std::int64_t>(id * 100),
        .source = "app.log",
        .content = "message-" + std::to_string(id),
    };
}

// 验证客户端身份、消息 ID 和文件偏移量可以跨重启恢复。
void testClientStateRecovery() {
    TemporaryDirectory temporary; // 测试状态文件所在目录。
    const auto statePath = temporary.path() / "client.state"; // 状态文件路径。
    std::string clientId; // 第一次运行生成的客户端 ID。

    {
        logbridge::ClientState state(statePath, "app.log", {});
        clientId = state.clientId();
        require(state.nextMessageId() == 1, "first message ID must be one");
        state.updateFileCheckpoint(42, 7, 99);
    }

    {
        logbridge::ClientState state(statePath, "app.log", {});
        require(state.clientId() == clientId, "client ID must survive restart");
        require(state.nextMessageId() == 2, "message ID must continue after restart");
        require(state.fileOffset() == 42, "file offset must survive restart");
        require(state.fileDeviceId() == 7,
                "file device ID must survive restart");
        require(state.fileInode() == 99,
                "file inode must survive restart");
    }

    const std::vector<LogMessage> recovered{makeMessage(clientId, 50)};
    logbridge::ClientState repaired(statePath, "app.log", recovered);
    require(repaired.nextMessageId() == 51,
            "WAL maximum ID must repair stale client state");
}

// 验证 WAL 追加、恢复、累计确认和不完整尾部清理。
void testWalRecoveryAndCompaction() {
    TemporaryDirectory temporary; // 测试 WAL 所在目录。
    const auto walPath = temporary.path() / "pending.wal"; // WAL 文件路径。
    const std::string clientId = "wal-test-client"; // WAL 中消息所属客户端。

    {
        logbridge::WriteAheadLog wal(walPath);
        wal.append(makeMessage(clientId, 1));
        wal.append(makeMessage(clientId, 2));
        wal.append(makeMessage(clientId, 3));
    }

    {
        logbridge::WriteAheadLog wal(walPath);
        require(wal.pending().size() == 3, "WAL must recover all messages");
        wal.confirm(2);
        require(wal.pending().size() == 1, "ACK must compact confirmed messages");
        require(wal.pending().front().id == 3, "WAL must retain unconfirmed message");
    }

    {
        std::ofstream tail(walPath, std::ios::binary | std::ios::app); // 模拟崩溃残留。
        tail.write("broken", 6);
    }

    logbridge::WriteAheadLog recovered(walPath);
    require(recovered.pending().size() == 1,
            "truncated WAL tail must not lose complete records");
    require(recovered.pending().front().id == 3,
            "WAL recovery returned wrong message");
}

// 验证服务端确认水位持久化且只能单调增加。
void testDedupStore() {
    TemporaryDirectory temporary; // 去重状态所在目录。
    const auto dedupPath = temporary.path() / "dedup.state"; // 去重文件路径。

    {
        logbridge::DedupStore store(dedupPath);
        require(store.confirm("client-a", 10) == 10,
                "dedup watermark must advance");
        require(store.confirm("client-a", 8) == 10,
                "dedup watermark must not move backward");
        store.confirm("client-b", 3);
    }

    logbridge::DedupStore recovered(dedupPath);
    require(recovered.highestConfirmed("client-a") == 10,
            "client-a watermark must survive restart");
    require(recovered.highestConfirmed("client-b") == 3,
            "client-b watermark must survive restart");
    require(recovered.highestConfirmed("unknown") == 0,
            "unknown client watermark must be zero");
}

} // 匿名命名空间

// 运行可靠性持久化测试。
int main() {
    try {
        testClientStateRecovery();
        testWalRecoveryAndCompaction();
        testDedupStore();
        std::cout << "Reliability tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Reliability test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
