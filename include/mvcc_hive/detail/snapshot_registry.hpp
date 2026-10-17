#pragma once

#include <mvcc_hive/detail/layout.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

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

private:
    mutable std::mutex mutex_;
    std::multiset<CommitId> active_commits_;
};

class SnapshotGuard {
public:
    SnapshotGuard(
        std::weak_ptr<SnapshotRegistry> registry,
        CommitId commit_id) noexcept
        : registry_(std::move(registry)),
          commit_id_(commit_id) {}

    SnapshotGuard(const SnapshotGuard&) = delete;
    SnapshotGuard& operator=(const SnapshotGuard&) = delete;

    ~SnapshotGuard() {
        if (!registered_) {
            return;
        }
        if (const auto registry = registry_.lock()) {
            registry->remove(commit_id_);
        }
    }

    void mark_registered() noexcept {
        registered_ = true;
    }

    [[nodiscard]] bool valid() const noexcept {
        return !registry_.expired();
    }

private:
    std::weak_ptr<SnapshotRegistry> registry_;
    CommitId commit_id_;
    bool registered_ = false;
};

}  // namespace mvcc_hive::detail
