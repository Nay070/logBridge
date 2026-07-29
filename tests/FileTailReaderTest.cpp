#include "FileTailReader.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void append(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::app);
    require(static_cast<bool>(file), "cannot append test log");
    file << content;
}

} // namespace

int main() {
    const auto uniqueId =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("logbridge-reader-" + std::to_string(uniqueId) + ".log");

    try {
        // 创建空日志文件，并从文件开头开始监听。
        std::ofstream(path, std::ios::binary | std::ios::trunc).close();
        FileTailReader reader(path.string());

        append(path, "first line\npartial");
        const std::vector<std::string> firstRead = reader.readNewLines();
        require(firstRead == std::vector<std::string>{"first line"},
                "first read should return only the complete line");

        // 补全上一轮留下的半行，并写入一条 CRLF 日志。
        append(path, " line\nwindows line\r\n");
        const std::vector<std::string> secondRead = reader.readNewLines();
        require(secondRead ==
                    std::vector<std::string>{"partial line", "windows line"},
                "second read should complete pending and CRLF lines");

        require(reader.readNewLines().empty(),
                "unchanged file should not return duplicate lines");

        // 文件被截断后，读取器从新文件开头重新读取。
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << "after truncate\n";
        }

        const std::vector<std::string> afterTruncate = reader.readNewLines();
        require(afterTruncate ==
                    std::vector<std::string>{"after truncate"},
                "truncated file should be read from the beginning");

        std::filesystem::remove(path);
        std::cout << "FileTailReader tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::filesystem::remove(path);
        std::cerr << "FileTailReader test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
