#include "ClientState.h"

#include "Persistence.h"

#include <algorithm>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

namespace logbridge {
namespace {

constexpr const char* StateMagic = "LGBSTATE2"; // 当前客户端状态文件格式标识。
constexpr const char* LegacyStateMagic = "LGBSTATE1"; // 不含文件身份的旧格式标识。

// 生成 128 位随机客户端 ID，并编码为 32 个十六进制字符。
std::string generateClientId() {
    constexpr char Hex[] = "0123456789abcdef"; // 十六进制字符表。
    std::random_device random; // 操作系统随机数来源。
    std::string id(32, '0'); // 最终生成的客户端 ID。
    for (char& character : id) {
        character = Hex[random() & 0x0F];
    }
    return id;
}

// 将字符串作为字节视图交给持久化函数。
std::span<const std::uint8_t> asBytes(const std::string& text) {
    return {
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size(),
    };
}

// 严格解析无符号 64 位整数。
std::uint64_t parseUint64(const std::string& text,
                          const std::string& fieldName) {
    if (text.empty() || text.front() == '-') {
        throw std::runtime_error("invalid client state field: " + fieldName);
    }
    std::size_t parsed = 0; // 成功转换的字符数量。
    const unsigned long long value = std::stoull(text, &parsed);
    if (parsed != text.size()) {
        throw std::runtime_error("invalid client state field: " + fieldName);
    }
    return static_cast<std::uint64_t>(value);
}

} // 匿名命名空间

ClientState::ClientState(
    std::filesystem::path path,
    std::string sourcePath,
    std::span<const LogMessage> recoveredMessages)
    : path_(std::move(path)) {
    if (path_.empty() || sourcePath.empty()) {
        throw std::invalid_argument("client state path and source cannot be empty");
    }
    const std::string requestedSource = std::move(sourcePath); // 本次运行的日志路径。

    const bool stateExists = std::filesystem::exists(path_); // 是否已有状态文件。
    if (stateExists) {
        load();
    } else {
        clientId_ = recoveredMessages.empty()
            ? generateClientId()
            : recoveredMessages.front().clientId;
    }

    bool changed = !stateExists; // 是否需要重新保存修正后的状态。
    if (sourcePath_ != requestedSource) {
        sourcePath_ = requestedSource;
        fileOffset_ = 0;
        fileDeviceId_ = 0;
        fileInode_ = 0;
        changed = true;
    }

    std::uint64_t maximumRecoveredId = 0; // WAL 中最大的消息 ID。
    for (const LogMessage& message : recoveredMessages) {
        if (message.clientId != clientId_) {
            throw std::runtime_error("client state does not match WAL client ID");
        }
        maximumRecoveredId = std::max(maximumRecoveredId, message.id);
    }
    if (maximumRecoveredId == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("message ID space is exhausted");
    }
    if (nextId_ <= maximumRecoveredId) {
        nextId_ = maximumRecoveredId + 1;
        changed = true;
    }

    if (changed) {
        persist();
    }
}

const std::string& ClientState::clientId() const noexcept {
    return clientId_;
}

std::uint64_t ClientState::nextMessageId() {
    if (nextId_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("message ID space is exhausted");
    }

    const std::uint64_t allocated = nextId_; // 本次分配给消息的 ID。
    ++nextId_;
    try {
        persist();
    } catch (...) {
        nextId_ = allocated;
        throw;
    }
    return allocated;
}

std::uint64_t ClientState::fileOffset() const noexcept {
    return fileOffset_;
}

std::uint64_t ClientState::fileDeviceId() const noexcept {
    return fileDeviceId_;
}

std::uint64_t ClientState::fileInode() const noexcept {
    return fileInode_;
}

void ClientState::updateFileOffset(std::uint64_t offset) {
    updateFileCheckpoint(offset, fileDeviceId_, fileInode_);
}

void ClientState::updateFileCheckpoint(std::uint64_t offset,
                                       std::uint64_t deviceId,
                                       std::uint64_t inode) {
    if (offset == fileOffset_ &&
        deviceId == fileDeviceId_ &&
        inode == fileInode_) {
        return;
    }

    const std::uint64_t previousOffset = fileOffset_; // 更新失败时恢复的旧偏移量。
    const std::uint64_t previousDeviceId = fileDeviceId_; // 更新失败时恢复的旧设备编号。
    const std::uint64_t previousInode = fileInode_; // 更新失败时恢复的旧 inode。
    fileOffset_ = offset;
    fileDeviceId_ = deviceId;
    fileInode_ = inode;
    try {
        persist();
    } catch (...) {
        fileOffset_ = previousOffset;
        fileDeviceId_ = previousDeviceId;
        fileInode_ = previousInode;
        throw;
    }
}

void ClientState::load() {
    const storage::Bytes data = storage::readFile(path_); // 状态文件原始字节。
    const std::string text(data.begin(), data.end()); // 便于逐行解析的文本。
    std::istringstream input(text); // 状态文本输入流。

    std::string magic; // 文件格式标识。
    std::string nextIdText; // 下一个消息 ID 文本。
    std::string fileOffsetText; // 文件偏移量文本。
    std::string fileDeviceIdText; // 文件设备编号文本。
    std::string fileInodeText; // 文件 inode 文本。
    std::string storedSource; // 状态文件中记录的日志路径。
    if (!std::getline(input, magic) ||
        !std::getline(input, clientId_) ||
        !std::getline(input, nextIdText) ||
        !std::getline(input, fileOffsetText) || clientId_.empty()) {
        throw std::runtime_error("invalid client state file");
    }

    if (magic == StateMagic) {
        if (!std::getline(input, fileDeviceIdText) ||
            !std::getline(input, fileInodeText) ||
            !std::getline(input, storedSource)) {
            throw std::runtime_error("invalid client state file");
        }
        fileDeviceId_ = parseUint64(fileDeviceIdText, "fileDeviceId");
        fileInode_ = parseUint64(fileInodeText, "fileInode");
    } else if (magic == LegacyStateMagic) {
        if (!std::getline(input, storedSource)) {
            throw std::runtime_error("invalid client state file");
        }
        fileDeviceId_ = 0;
        fileInode_ = 0;
    } else {
        throw std::runtime_error("invalid client state file");
    }

    nextId_ = parseUint64(nextIdText, "nextId");
    fileOffset_ = parseUint64(fileOffsetText, "fileOffset");
    sourcePath_ = std::move(storedSource);
    if (nextId_ == 0) {
        throw std::runtime_error("invalid next message ID in client state");
    }
}

void ClientState::persist() const {
    std::ostringstream output; // 保存待写入状态文件的文本。
    output << StateMagic << '\n'
           << clientId_ << '\n'
           << nextId_ << '\n'
           << fileOffset_ << '\n'
           << fileDeviceId_ << '\n'
           << fileInode_ << '\n'
           << sourcePath_ << '\n';
    const std::string text = output.str(); // 完整状态文件内容。
    storage::writeAtomically(path_, asBytes(text));
}

} // logbridge 命名空间
