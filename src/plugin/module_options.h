#pragma once
// Helpers for the modules' registry tables (src/<module>/*_api.cpp): many modules take their settings as one Options struct
// (RequestOptions / Request(..., options)) that the game thread publishes back in the next Snapshot. Two property sets in one
// frame would both read the old Snapshot and the second would undo the first, so the table keeps the options it sent last and
// reads from them until the Snapshot has had time to follow.
#include <windows.h>
#include <mutex>

namespace ModuleOptions {
template <class Options> class Pending {
public:
    // The options now: the last ones sent (for 1.5 s), else what `snapshot()` has.
    template <class Snapshot> Options Now(Snapshot&& snapshot) const {
        std::lock_guard<std::mutex> hold(lock_);
        if (have_ && GetTickCount64() < until_) return value_;
        return snapshot();
    }
    void Sent(const Options& options) { std::lock_guard<std::mutex> hold(lock_); value_ = options; have_ = true; until_ = GetTickCount64() + 1500; }
private:
    mutable std::mutex lock_;
    Options value_{};
    bool have_ = false;
    ULONGLONG until_ = 0;
};
}  // namespace ModuleOptions
