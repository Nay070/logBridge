#include "FileTailReader.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

FileTailReader::FileTailReader(std::string path,
                               std::uint64_t initialOffset,
                               std::uint64_t expectedDeviceId,
                               std::uint64_t expectedInode)
    : path_(std::move(path)) {
    if (path_.empty()) {
        throw std::invalid_argument("log file path cannot be empty");
    }
    openCurrentFile(initialOffset, expectedDeviceId, expectedInode);
}

FileTailReader::~FileTailReader() {
    closeCurrentFile();
}

std::vector<std::string> FileTailReader::readNewLines() {
    if (switchOnNextRead_) {
        closeCurrentFile();
        openCurrentFile(0, 0, 0);
        switchOnNextRead_ = false;
    } else {
        struct stat pathStatus{}; // 当前路径所指向文件的身份信息。
        if (::stat(path_.c_str(), &pathStatus) == 0) {
            const bool replaced = // 路径是否已经指向轮转后的另一个文件。
                deviceId_ != static_cast<std::uint64_t>(pathStatus.st_dev) ||
                inode_ != static_cast<std::uint64_t>(pathStatus.st_ino);
            if (replaced) {
                std::vector<std::string> lines = // 轮转前旧文件剩余的日志行。
                    readFromOpenFile();
                if (!pending_.empty()) {
                    lines.push_back(std::move(pending_));
                    pending_.clear();
                }
                // 先返回旧文件尾部，下一轮再切换，避免一个检查点同时跨越两个文件。
                switchOnNextRead_ = true;
                return lines;
            }
        } else if (errno != ENOENT) {
            throw std::system_error(
                errno, std::generic_category(), "cannot inspect log file");
        }
    }

    return readFromOpenFile();
}

std::uint64_t FileTailReader::offset() const noexcept {
    return offset_;
}

std::uint64_t FileTailReader::committedOffset() const noexcept {
    return offset_ - static_cast<std::uint64_t>(pending_.size());
}

const std::string& FileTailReader::path() const noexcept {
    return path_;
}

std::uint64_t FileTailReader::deviceId() const noexcept {
    return deviceId_;
}

std::uint64_t FileTailReader::inode() const noexcept {
    return inode_;
}

void FileTailReader::openCurrentFile(std::uint64_t initialOffset,
                                     std::uint64_t expectedDeviceId,
                                     std::uint64_t expectedInode) {
    fileFd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fileFd_ < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot open log file: " + path_);
    }

    struct stat status{}; // 已打开文件的大小和稳定身份信息。
    if (::fstat(fileFd_, &status) != 0) {
        const int error = errno; // 在关闭描述符前保存的 fstat 错误码。
        closeCurrentFile();
        throw std::system_error(
            error, std::generic_category(), "cannot inspect open log file");
    }
    if (status.st_size < 0) {
        closeCurrentFile();
        throw std::runtime_error("log file has an invalid size: " + path_);
    }

    deviceId_ = static_cast<std::uint64_t>(status.st_dev);
    inode_ = static_cast<std::uint64_t>(status.st_ino);
    const bool identityMatches = // 持久化检查点是否属于当前打开的文件。
        expectedDeviceId == 0 || expectedInode == 0 ||
        (expectedDeviceId == deviceId_ && expectedInode == inode_);
    offset_ = identityMatches ? initialOffset : 0;
    if (offset_ > static_cast<std::uint64_t>(status.st_size)) {
        offset_ = 0;
    }
    pending_.clear();
}

std::vector<std::string> FileTailReader::readFromOpenFile() {
    struct stat status{}; // 当前打开文件本轮读取前的大小。
    if (::fstat(fileFd_, &status) != 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot inspect open log file");
    }
    if (status.st_size < 0) {
        throw std::runtime_error("log file has an invalid size: " + path_);
    }

    const std::uint64_t fileSize = static_cast<std::uint64_t>(status.st_size);
    if (fileSize < offset_) {
        offset_ = 0;
        pending_.clear();
    }
    if (fileSize == offset_) {
        return {};
    }

    const std::uint64_t unreadSize = fileSize - offset_; // 本轮需要读取的字节数。
    if (unreadSize > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("unread log data is too large: " + path_);
    }

    std::string chunk(static_cast<std::size_t>(unreadSize), '\0'); // 本轮新增的原始字节。
    std::size_t totalRead = 0; // 已经写入 chunk 的字节数。
    while (totalRead < chunk.size()) {
        const std::size_t remaining = // chunk 中尚未填充的字节数。
            chunk.size() - totalRead;
        const std::size_t requestSize = // 本次 pread 请求读取的字节数。
            std::min(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = // 本次 pread 实际读取的字节数。
            ::pread(
            fileFd_,
            chunk.data() + totalRead,
            requestSize,
            static_cast<off_t>(offset_ + totalRead));
        if (result > 0) {
            totalRead += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        throw std::system_error(
            errno, std::generic_category(), "cannot read log file: " + path_);
    }

    chunk.resize(totalRead);
    offset_ += totalRead;
    pending_.append(chunk);

    std::vector<std::string> lines; // 本轮解析出的完整日志行。
    std::size_t lineStart = 0; // 当前日志行在 pending_ 中的起始位置。
    while (true) {
        const std::size_t newline = // 下一处换行符在 pending_ 中的位置。
            pending_.find('\n', lineStart);
        if (newline == std::string::npos) {
            break;
        }

        std::string line = // 当前换行符之前的一条完整日志。
            pending_.substr(lineStart, newline - lineStart);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        lineStart = newline + 1;
    }

    pending_.erase(0, lineStart);
    return lines;
}

void FileTailReader::closeCurrentFile() noexcept {
    if (fileFd_ >= 0) {
        ::close(fileFd_);
        fileFd_ = -1;
    }
}
