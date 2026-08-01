#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 增量读取日志文件，只返回上次读取位置之后出现的完整行。
class FileTailReader {
public:
    // 创建读取器，并从 initialOffset 指定的位置继续读取。
    explicit FileTailReader(std::string path,
                            std::uint64_t initialOffset = 0,
                            std::uint64_t expectedDeviceId = 0,
                            std::uint64_t expectedInode = 0);

    // 关闭当前日志文件描述符。
    ~FileTailReader();

    // 文件描述符具有唯一所有权，因此禁止复制读取器。
    FileTailReader(const FileTailReader&) = delete;

    // 禁止通过赋值复制文件描述符和读取进度。
    FileTailReader& operator=(const FileTailReader&) = delete;

    // 读取自上次调用后新增的完整日志行；没有完整新行时返回空数组。
    std::vector<std::string> readNewLines();

    // 返回已经从文件中读取过的字节数。
    [[nodiscard]] std::uint64_t offset() const noexcept;

    // 返回最后一条完整日志行结束后的安全检查点。
    [[nodiscard]] std::uint64_t committedOffset() const noexcept;

    // 返回当前读取器监听的日志文件路径。
    [[nodiscard]] const std::string& path() const noexcept;

    // 返回当前打开文件所在设备的编号。
    [[nodiscard]] std::uint64_t deviceId() const noexcept;

    // 返回当前打开文件的 inode 编号。
    [[nodiscard]] std::uint64_t inode() const noexcept;

private:
    // 打开 path_ 当前指向的文件，并根据预期身份决定是否沿用 initialOffset。
    void openCurrentFile(std::uint64_t initialOffset,
                         std::uint64_t expectedDeviceId,
                         std::uint64_t expectedInode);

    // 从当前文件描述符增量读取数据并拆分完整日志行。
    std::vector<std::string> readFromOpenFile();

    // 关闭当前文件描述符。
    void closeCurrentFile() noexcept;

    std::string path_;          // 被监听日志文件的路径。
    int fileFd_{-1};            // 当前打开日志文件的文件描述符。
    std::uint64_t offset_{0};   // 下一次读取应开始的文件字节偏移量。
    std::string pending_;       // 尚未遇到换行符的不完整日志内容。
    std::uint64_t deviceId_{0}; // 当前文件所在设备的编号。
    std::uint64_t inode_{0};    // 当前文件的 inode 编号。
    bool switchOnNextRead_{false}; // 下一次读取前是否需要切换到轮转后的新文件。
};
