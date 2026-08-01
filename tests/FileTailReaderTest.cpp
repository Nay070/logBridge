#include "FileTailReader.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// 检查 condition；失败时使用 message 抛出异常终止当前测试。
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 以二进制追加模式把 content 写入 path 指向的测试日志文件。
void append(const std::filesystem::path& path, const std::string& content) {
    // file 是本次向测试日志追加内容的输出文件流。
    std::ofstream file(path, std::ios::binary | std::ios::app);
    require(static_cast<bool>(file), "cannot append test log");
    file << content;
}

} // 匿名命名空间

// 创建临时日志文件，依次验证增量读取、半行、CRLF 和文件截断。
int main() {
    // uniqueId 使用单调时钟生成，避免并行测试时临时文件重名。
    const auto uniqueId =
        std::chrono::steady_clock::now().time_since_epoch().count();
    // path 是本次测试独占使用的临时日志文件路径。
    const auto path = std::filesystem::temp_directory_path() /
                      ("logbridge-reader-" + std::to_string(uniqueId) + ".log");
    const auto rotatedPath = path.string() + ".1"; // 轮转后旧文件使用的路径。

    try {
        // 创建空日志文件，并从文件开头开始监听。
        std::ofstream(path, std::ios::binary | std::ios::trunc).close();
        FileTailReader reader(path.string()); // 保存测试文件的增量读取状态。

        append(path, "first line\npartial");
        // firstRead 保存首次读取到的完整行，不应包含末尾半行。
        const std::vector<std::string> firstRead = reader.readNewLines();
        require(firstRead == std::vector<std::string>{"first line"},
                "first read should return only the complete line");
        require(reader.committedOffset() == 11,
                "checkpoint must stop before an incomplete line");

        // 补全上一轮留下的半行，并写入一条 CRLF 日志。
        append(path, " line\nwindows line\r\n");
        // secondRead 保存半行补全后和 CRLF 格式的两条日志。
        const std::vector<std::string> secondRead = reader.readNewLines();
        require(secondRead ==
                    std::vector<std::string>{"partial line", "windows line"},
                "second read should complete pending and CRLF lines");

        require(reader.readNewLines().empty(),
                "unchanged file should not return duplicate lines");

        FileTailReader resumed(path.string(), reader.committedOffset());
        require(resumed.readNewLines().empty(),
                "restored reader must not duplicate complete lines");

        // 文件被截断后，读取器从新文件开头重新读取。
        {
            // file 以 trunc 模式清空旧内容，并写入截断后的新日志。
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << "after truncate\n";
        }

        // afterTruncate 保存文件变小后从头读取出的日志。
        const std::vector<std::string> afterTruncate = reader.readNewLines();
        require(afterTruncate ==
                    std::vector<std::string>{"after truncate"},
                "truncated file should be read from the beginning");

        const std::uint64_t oldDeviceId = reader.deviceId(); // 轮转前文件所在设备编号。
        const std::uint64_t oldInode = reader.inode(); // 轮转前文件的 inode。
        const std::uint64_t oldOffset = reader.committedOffset(); // 轮转前检查点。

        std::filesystem::rename(path, rotatedPath);
        append(rotatedPath, "old file tail\n");
        {
            std::ofstream newFile(path, std::ios::binary | std::ios::trunc);
            newFile << "new file line\n";
        }

        const std::vector<std::string> oldTail = reader.readNewLines();
        require(oldTail == std::vector<std::string>{"old file tail"},
                "rotation must drain the old file before switching");
        require(reader.inode() == oldInode,
                "reader must keep the old identity while returning its tail");

        const std::vector<std::string> newFileLines = reader.readNewLines();
        require(newFileLines == std::vector<std::string>{"new file line"},
                "rotation must continue from the new file beginning");
        require(reader.inode() != oldInode || reader.deviceId() != oldDeviceId,
                "rotated file must have a different identity");

        FileTailReader restarted(
            path.string(), oldOffset, oldDeviceId, oldInode);
        require(restarted.readNewLines() ==
                    std::vector<std::string>{"new file line"},
                "stale checkpoint identity must not skip the replacement file");

        std::filesystem::remove(path);
        std::filesystem::remove(rotatedPath);
        std::cout << "FileTailReader tests passed\n";
        return 0;
    // exception 保存任意断言或文件操作失败的原因。
    } catch (const std::exception& exception) {
        std::filesystem::remove(path);
        std::filesystem::remove(rotatedPath);
        std::cerr << "FileTailReader test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
