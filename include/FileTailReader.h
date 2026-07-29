#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 增量读取日志文件，只返回上次读取位置之后出现的完整行。
class FileTailReader {
public:
    explicit FileTailReader(std::string path);

    std::vector<std::string> readNewLines();

    [[nodiscard]] std::uint64_t offset() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;

private:
    std::string path_;
    std::uint64_t offset_{0};

    // 保存尚未遇到换行符的半行内容。
    std::string pending_;
};
