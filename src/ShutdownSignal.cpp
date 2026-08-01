#include "ShutdownSignal.h"

#include <cerrno>
#include <pthread.h>
#include <stdexcept>
#include <system_error>

namespace logbridge {

ShutdownSignal::ShutdownSignal() {
    if (::sigemptyset(&signals_) != 0 ||
        ::sigaddset(&signals_, SIGINT) != 0 ||
        ::sigaddset(&signals_, SIGTERM) != 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot prepare shutdown signals");
    }

    const int result = ::pthread_sigmask(
        SIG_BLOCK, &signals_, &previousMask_); // 屏蔽信号后由 sigtimedwait 同步接收。
    if (result != 0) {
        throw std::system_error(
            result, std::generic_category(), "cannot block shutdown signals");
    }
}

ShutdownSignal::~ShutdownSignal() {
    ::pthread_sigmask(SIG_SETMASK, &previousMask_, nullptr);
}

bool ShutdownSignal::waitFor(std::chrono::milliseconds timeout) const {
    if (timeout.count() < 0) {
        throw std::invalid_argument("signal wait timeout cannot be negative");
    }

    const auto seconds = // 等待时间中的完整秒数。
        std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto nanoseconds = // 去掉完整秒后剩余的纳秒数。
        std::chrono::duration_cast<std::chrono::nanoseconds>(
        timeout - seconds);
    const timespec waitTime{
        .tv_sec = static_cast<time_t>(seconds.count()),
        .tv_nsec = static_cast<long>(nanoseconds.count()),
    }; // sigtimedwait 使用的相对等待时间。

    const int result = // 收到的信号编号，超时或中断时为 -1。
        ::sigtimedwait(&signals_, nullptr, &waitTime);
    if (result == SIGINT || result == SIGTERM) {
        return true;
    }
    if (result < 0 && (errno == EAGAIN || errno == EINTR)) {
        return false;
    }
    if (result < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot wait for shutdown signal");
    }
    return false;
}

} // logbridge 命名空间
