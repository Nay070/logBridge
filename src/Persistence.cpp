#include "Persistence.h"

#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>

namespace logbridge::storage {
namespace {

// 创建目标文件的父目录。
void ensureParentDirectory(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path(); // 目标文件所在目录。
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

// 将 data 全部写入文件描述符。
void writeAll(int fileFd, std::span<const std::uint8_t> data) {
    std::size_t writtenBytes = 0; // 已经成功写入的字节数。
    while (writtenBytes < data.size()) {
        const ssize_t result = ::write(
            fileFd,
            data.data() + writtenBytes,
            data.size() - writtenBytes);
        if (result > 0) {
            writtenBytes += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::system_error(
            result == 0 ? EIO : errno,
            std::generic_category(),
            "cannot write persistent file");
    }
}

// 刷新并关闭文件描述符，保留第一个错误。
void syncAndClose(int fileFd) {
    if (::fsync(fileFd) < 0) {
        const int error = errno; // fsync 失败的错误码。
        ::close(fileFd);
        throw std::system_error(
            error, std::generic_category(), "cannot sync persistent file");
    }
    if (::close(fileFd) < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot close persistent file");
    }
}

// 刷新目录元数据，确保 rename 在断电后仍然可见。
void syncParentDirectory(const std::filesystem::path& path) {
    const std::filesystem::path parent =
        path.parent_path().empty() ? "." : path.parent_path();
    const int directoryFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directoryFd < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot open parent directory");
    }
    syncAndClose(directoryFd);
}

} // 匿名命名空间

Bytes readFile(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return {};
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate); // 输入文件流。
    if (!file) {
        throw std::runtime_error("cannot open persistent file: " + path.string());
    }

    const std::streampos end = file.tellg(); // 文件末尾位置，也就是文件大小。
    if (end < 0) {
        throw std::runtime_error("cannot determine file size: " + path.string());
    }

    Bytes data(static_cast<std::size_t>(end)); // 保存读取出的全部字节。
    file.seekg(0, std::ios::beg);
    if (!data.empty()) {
        file.read(
            reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
        if (!file) {
            throw std::runtime_error("cannot read persistent file: " + path.string());
        }
    }
    return data;
}

void appendDurably(const std::filesystem::path& path,
                   std::span<const std::uint8_t> data) {
    ensureParentDirectory(path);
    const int fileFd = ::open(
        path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); // WAL 文件描述符。
    if (fileFd < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot open append file");
    }

    try {
        writeAll(fileFd, data);
    } catch (...) {
        ::close(fileFd);
        throw;
    }
    syncAndClose(fileFd);
    syncParentDirectory(path);
}

void writeAtomically(const std::filesystem::path& path,
                     std::span<const std::uint8_t> data) {
    ensureParentDirectory(path);
    const std::filesystem::path temporary = path.string() + ".tmp"; // 同目录临时文件。
    const int fileFd = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); // 临时文件描述符。
    if (fileFd < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot open temporary file");
    }

    try {
        writeAll(fileFd, data);
    } catch (...) {
        ::close(fileFd);
        std::error_code ignored; // 清理临时文件时忽略的错误。
        std::filesystem::remove(temporary, ignored);
        throw;
    }

    try {
        syncAndClose(fileFd);
        std::filesystem::rename(temporary, path);
        syncParentDirectory(path);
    } catch (...) {
        std::error_code ignored; // 清理临时文件时忽略的错误。
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // logbridge::storage 命名空间
