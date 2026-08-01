#pragma once

#include <chrono>
#include <csignal>

namespace logbridge {

// 通过同步等待 SIGINT 和 SIGTERM，为工作线程提供安全的退出通知。
class ShutdownSignal {
public:
    // 在当前线程中屏蔽退出信号，之后创建的线程会继承该设置。
    ShutdownSignal();

    // 恢复创建对象之前的线程信号掩码。
    ~ShutdownSignal();

    // 信号掩码属于创建对象的线程，因此禁止复制。
    ShutdownSignal(const ShutdownSignal&) = delete;

    // 禁止通过赋值复制线程信号掩码状态。
    ShutdownSignal& operator=(const ShutdownSignal&) = delete;

    // 在 timeout 内等待退出信号；收到 SIGINT 或 SIGTERM 时返回 true。
    [[nodiscard]] bool waitFor(std::chrono::milliseconds timeout) const;

private:
    sigset_t signals_{}; // 需要同步等待的退出信号集合。
    sigset_t previousMask_{}; // 创建对象前的线程信号掩码。
};

} // logbridge 命名空间
