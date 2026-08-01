#include "FileTailReader.h"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

FileTailReader::FileTailReader(std::string path,
                               std::uint64_t initialOffset)
    : path_(std::move(path)), offset_(initialOffset) {
    if (path_.empty()) {
        throw std::invalid_argument("log file path cannot be empty");
    }
}

std::vector<std::string> FileTailReader::readNewLines() {
    // file 表示本次打开的输入文件流；二进制模式可以避免系统转换换行符。
    std::ifstream file(path_, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open log file: " + path_);
    }

    // endPosition 保存本次观察到的文件末尾位置。
    file.seekg(0, std::ios::end);
    const std::streampos endPosition = file.tellg();
    if (endPosition < 0) {
        throw std::runtime_error("cannot determine log file size: " + path_);
    }

    // fileSize 是本次读取开始时日志文件的总字节数。
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

    // unreadSize 表示 offset_ 之后还有多少字节没有读取。
    const std::uint64_t unreadSize = fileSize - offset_;
    if (unreadSize >
        static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("unread log data is too large: " + path_);
    }

    file.seekg(static_cast<std::streamoff>(offset_), std::ios::beg);

    // chunk 暂存本次从文件中新读取到的原始字节。
    std::string chunk(static_cast<std::size_t>(unreadSize), '\0');
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));

    // bytesRead 是 file.read() 实际成功读取的字节数。
    const std::streamsize bytesRead = file.gcount();
    if (file.bad()) {
        throw std::runtime_error("cannot read log file: " + path_);
    }

    chunk.resize(static_cast<std::size_t>(bytesRead));
    offset_ += static_cast<std::uint64_t>(bytesRead);
    pending_.append(chunk);

    std::vector<std::string> lines; // 保存本次找到的所有完整日志行。
    std::size_t lineStart = 0;      // 当前待解析日志行在 pending_ 中的起点。

    while (true) {
        // newline 保存从 lineStart 开始找到的下一个换行符位置。
        const std::size_t newline = pending_.find('\n', lineStart);
        if (newline == std::string::npos) {
            break;
        }

        // line 保存当前换行符之前的一条完整日志。
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

std::uint64_t FileTailReader::committedOffset() const noexcept {
    return offset_ - static_cast<std::uint64_t>(pending_.size());
}

const std::string &FileTailReader::path() const noexcept {
    return path_;
}
