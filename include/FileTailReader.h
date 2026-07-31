#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 增量读取日志文件，只返回上次读取位置之后出现的完整行。
class FileTailReader {
public:
    // 创建文件读取器。
    // path 是需要持续读取的日志文件路径，不能为空。
    explicit FileTailReader(std::string path);

    // 读取自上次调用后新增的完整日志行；没有完整新行时返回空数组。
    std::vector<std::string> readNewLines();

    // 返回已经从文件中读取过的字节数。
    [[nodiscard]] std::uint64_t offset() const noexcept;

    // 返回当前读取器监听的日志文件路径。
    [[nodiscard]] const std::string& path() const noexcept;

private:
    std::string path_;          // 被监听日志文件的路径。
    std::uint64_t offset_{0};   // 下一次读取应开始的文件字节偏移量。
    std::string pending_;       // 尚未遇到换行符的不完整日志内容。
};
