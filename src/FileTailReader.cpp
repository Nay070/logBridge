#include "FileTailReader.h"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

FileTailReader::FileTailReader(std::string path)
    : path_(std::move(path)) {
    if (path_.empty()) {
        throw std::invalid_argument("log file path cannot be empty");
    }
}

std::vector<std::string> FileTailReader::readNewLines() {
    std::ifstream file(path_, std::ios::binary); //使用二进制模式可以避免系统自动转换换行符
    if (!file) {
        throw std::runtime_error("cannot open log file: " + path_);
    }

    // 先取得本次观察到的文件大小。
    file.seekg(0, std::ios::end);
    const std::streampos endPosition = file.tellg();
    if (endPosition < 0) {
        throw std::runtime_error("cannot determine log file size: " + path_);
    }

    const auto fileSize = static_cast<std::uint64_t>(endPosition);

    // 文件变小通常表示被截断，第一版从头重新读取。
    if (fileSize < offset_) {
        offset_ = 0;
        pending_.clear();
    }

    // 文件大小等于偏移量说明没有新增。
    if (fileSize == offset_) {
        return {};
    }

    // 计算未读数据量。
    const std::uint64_t unreadSize = fileSize - offset_;
    if (unreadSize >
        static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("unread log data is too large: " + path_);
    }

    file.seekg(static_cast<std::streamoff>(offset_), std::ios::beg);

    std::string chunk(static_cast<std::size_t>(unreadSize), '\0');
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));

    const std::streamsize bytesRead = file.gcount();
    if (file.bad()) {
        throw std::runtime_error("cannot read log file: " + path_);
    }

    chunk.resize(static_cast<std::size_t>(bytesRead));
    offset_ += static_cast<std::uint64_t>(bytesRead);
    pending_.append(chunk);

    std::vector<std::string> lines;
    std::size_t lineStart = 0;

    while (true) {
        const std::size_t newline = pending_.find('\n', lineStart);
        if (newline == std::string::npos) {
            break;
        }

        std::string line = pending_.substr(lineStart, newline - lineStart);

        // 兼容以 CRLF 结尾的日志文件。
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        lines.push_back(std::move(line));
        lineStart = newline + 1;
    }

    // 删除已返回的完整行，只留下最后一个不完整行。
    pending_.erase(0, lineStart);
    return lines;
}

std::uint64_t FileTailReader::offset() const noexcept {
    return offset_;
}

const std::string &FileTailReader::path() const noexcept {
    return path_;
}
