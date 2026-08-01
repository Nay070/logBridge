#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace logbridge::storage {

using Bytes = std::vector<std::uint8_t>; // 文件的原始字节内容。

// 读取整个文件；文件不存在时返回空数组。
Bytes readFile(const std::filesystem::path& path);

// 追加数据并调用 fsync，返回前保证数据已交给磁盘。
void appendDurably(const std::filesystem::path& path,
                   std::span<const std::uint8_t> data);

// 通过“临时文件 + fsync + rename”原子替换目标文件。
void writeAtomically(const std::filesystem::path& path,
                     std::span<const std::uint8_t> data);

} // logbridge::storage 命名空间
