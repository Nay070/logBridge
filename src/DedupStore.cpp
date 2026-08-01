#include "DedupStore.h"

#include "Persistence.h"

#include <sstream>
#include <stdexcept>

namespace logbridge {
namespace {

constexpr const char* DedupMagic = "LGBDEDUP1"; // 去重状态文件格式标识。

// 将字符串转换成持久化接口需要的字节视图。
std::span<const std::uint8_t> asBytes(const std::string& text) {
    return {
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size(),
    };
}

} // 匿名命名空间

DedupStore::DedupStore(std::filesystem::path path)
    : path_(std::move(path)) {
    if (path_.empty()) {
        throw std::invalid_argument("dedup state path cannot be empty");
    }
    load();
}

std::uint64_t DedupStore::highestConfirmed(
    const std::string& clientId) const {
    std::lock_guard lock(mutex_);
    const auto position = confirmed_.find(clientId); // 客户端在确认表中的位置。
    return position == confirmed_.end() ? 0 : position->second;
}

std::uint64_t DedupStore::confirm(const std::string& clientId,
                                  std::uint64_t confirmedId) {
    if (clientId.empty() || confirmedId == 0) {
        throw std::invalid_argument("client ID and confirmed ID cannot be empty");
    }

    std::lock_guard lock(mutex_);
    const auto currentPosition = confirmed_.find(clientId); // 当前客户端的已有水位。
    const std::uint64_t current =
        currentPosition == confirmed_.end() ? 0 : currentPosition->second;
    if (confirmedId <= current) {
        return current;
    }

    auto updated = confirmed_; // 持久化成功前使用的候选状态副本。
    updated[clientId] = confirmedId;
    persist(updated);
    confirmed_ = std::move(updated);
    return confirmedId;
}

void DedupStore::load() {
    if (!std::filesystem::exists(path_)) {
        return;
    }

    const storage::Bytes data = storage::readFile(path_); // 状态文件原始字节。
    const std::string text(data.begin(), data.end()); // 用于流式解析的文本。
    std::istringstream input(text); // 文本输入流。
    std::string magic; // 文件格式标识。
    if (!std::getline(input, magic) || magic != DedupMagic) {
        throw std::runtime_error("invalid dedup state file");
    }

    std::string clientId; // 当前解析的客户端 ID。
    std::uint64_t confirmedId = 0; // 当前解析的确认水位。
    while (input >> clientId >> confirmedId) {
        if (clientId.empty() || confirmedId == 0 || confirmed_.contains(clientId)) {
            throw std::runtime_error("invalid dedup state record");
        }
        confirmed_.emplace(clientId, confirmedId);
    }
    if (!input.eof()) {
        throw std::runtime_error("cannot parse dedup state file");
    }
}

void DedupStore::persist(
    const std::map<std::string, std::uint64_t>& values) const {
    std::ostringstream output; // 去重状态文本输出流。
    output << DedupMagic << '\n';
    for (const auto& [clientId, confirmedId] : values) {
        output << clientId << ' ' << confirmedId << '\n';
    }
    const std::string text = output.str(); // 完整状态文件内容。
    storage::writeAtomically(path_, asBytes(text));
}

} // logbridge 命名空间
