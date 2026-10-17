#pragma once

#include <mvcc_hive/detail/row_index.hpp>
#include <mvcc_hive/detail/snapshot_registry.hpp>
#include <mvcc_hive/detail/version_store.hpp>

#include <cassert>
#include <memory>
#include <utility>

namespace mvcc_hive::detail {

// One immutable generation of the data visible to snapshots. The hive and
// every snapshot guard from this generation share ownership, so clear() can
// invalidate handles immediately without destroying storage under an in-flight
// reader.
template<class T>
struct HiveState {
    VersionStore<T> store;
    RowIndex rows;
};

template<class T>
class SnapshotGuard {
public:
    SnapshotGuard(
        std::weak_ptr<SnapshotRegistry> registry,
        CommitId commit_id,
        std::shared_ptr<const HiveState<T>> state) noexcept
        : registry_(std::move(registry)),
          state_(std::move(state)),
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

    [[nodiscard]] const HiveState<T>& state() const noexcept {
        assert(state_);
        return *state_;
    }

private:
    std::weak_ptr<SnapshotRegistry> registry_;
    std::shared_ptr<const HiveState<T>> state_;
    CommitId commit_id_;
    bool registered_ = false;
};

}  // namespace mvcc_hive::detail
