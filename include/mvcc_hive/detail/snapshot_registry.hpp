#pragma once

#include <mvcc_hive/detail/layout.hpp>

#include <mutex>
#include <set>

namespace mvcc_hive::detail {

class SnapshotRegistry {
public:
    SnapshotRegistry() = default;
    SnapshotRegistry(const SnapshotRegistry&) = delete;
    SnapshotRegistry& operator=(const SnapshotRegistry&) = delete;

    void add(CommitId commit_id) {
        std::lock_guard lock{mutex_};
        active_commits_.insert(commit_id);
    }

    void remove(CommitId commit_id) noexcept {
        std::lock_guard lock{mutex_};
        const auto it = active_commits_.find(commit_id);
        if (it != active_commits_.end()) {
            active_commits_.erase(it);
        }
    }

    [[nodiscard]] bool oldest(CommitId& commit_id) const noexcept {
        std::lock_guard lock{mutex_};
        if (active_commits_.empty()) {
            return false;
        }
        commit_id = *active_commits_.begin();
        return true;
    }

    [[nodiscard]] bool has_above(CommitId commit_id) const noexcept {
        std::lock_guard lock{mutex_};
        return active_commits_.upper_bound(commit_id) !=
               active_commits_.end();
    }

    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard lock{mutex_};
        return active_commits_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::multiset<CommitId> active_commits_;
};

}  // namespace mvcc_hive::detail
