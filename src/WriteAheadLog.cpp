#include "WriteAheadLog.h"

#include "Persistence.h"
#include "Protocol.h"

#include <algorithm>
#include <span>
#include <stdexcept>

namespace logbridge {

WriteAheadLog::WriteAheadLog(std::filesystem::path path)
    : path_(std::move(path)) {
    if (path_.empty()) {
        throw std::invalid_argument("WAL path cannot be empty");
    }
    load();
}

void WriteAheadLog::append(const LogMessage& message) {
    const protocol::ByteBuffer frame = // 可独立恢复的一条 WAL 记录。
        protocol::serializeLogMessage(message);

    std::lock_guard lock(mutex_);
    storage::appendDurably(path_, frame);
    pending_.push_back(message);
}

void WriteAheadLog::confirm(std::uint64_t confirmedId) {
    std::lock_guard lock(mutex_);
    const auto firstPending = std::remove_if(
        pending_.begin(),
        pending_.end(),
        [confirmedId](const LogMessage& message) {
            return message.id <= confirmedId;
        });
    if (firstPending == pending_.end()) {
        return;
    }

    pending_.erase(firstPending, pending_.end());
    rewriteLocked();
}

std::vector<LogMessage> WriteAheadLog::pending() const {
    std::lock_guard lock(mutex_);
    return pending_;
}

std::uint64_t WriteAheadLog::maxMessageId() const {
    std::lock_guard lock(mutex_);
    std::uint64_t maximum = 0; // 当前找到的最大消息 ID。
    for (const LogMessage& message : pending_) {
        maximum = std::max(maximum, message.id);
    }
    return maximum;
}

void WriteAheadLog::load() {
    const storage::Bytes data = storage::readFile(path_); // WAL 的全部原始字节。
    std::size_t offset = 0; // 下一条记录在 data 中的起点。
    bool hasTruncatedTail = false; // 是否发现崩溃造成的不完整尾部。

    while (offset < data.size()) {
        const std::size_t remaining = data.size() - offset; // 尚未解析的字节数。
        if (remaining < protocol::FrameHeaderSize) {
            hasTruncatedTail = true;
            break;
        }

        const std::span<const std::uint8_t> remainingData(
            data.data() + offset, remaining); // 从当前记录开始的字节视图。
        const protocol::FrameHeader header =
            protocol::parseFrameHeader(remainingData);
        if (header.type != protocol::MessageType::Log) {
            throw std::runtime_error("WAL contains a non-log frame");
        }

        const std::size_t frameSize = // 当前 WAL 记录的完整长度。
            protocol::FrameHeaderSize + header.payloadLength;
        if (remaining < frameSize) {
            hasTruncatedTail = true;
            break;
        }

        const std::span<const std::uint8_t> frame(
            data.data() + offset, frameSize); // 当前完整协议帧。
        LogMessage message = protocol::deserializeLogMessage(frame);
        if (!pending_.empty() && message.id <= pending_.back().id) {
            throw std::runtime_error("WAL message IDs are not increasing");
        }
        if (!pending_.empty() &&
            message.clientId != pending_.front().clientId) {
            throw std::runtime_error("WAL contains multiple client IDs");
        }
        pending_.push_back(std::move(message));
        offset += frameSize;
    }

    if (hasTruncatedTail) {
        rewriteLocked();
    }
}

void WriteAheadLog::rewriteLocked() {
    storage::Bytes data; // 压缩后 WAL 的完整字节内容。
    for (const LogMessage& message : pending_) {
        const protocol::ByteBuffer frame =
            protocol::serializeLogMessage(message);
        data.insert(data.end(), frame.begin(), frame.end());
    }
    storage::writeAtomically(path_, data);
}

} // logbridge 命名空间
