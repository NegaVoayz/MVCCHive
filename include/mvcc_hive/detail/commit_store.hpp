#pragma once

#include <mvcc_hive/diff.hpp>
#include <mvcc_hive/detail/blocked_vector.hpp>
#include <mvcc_hive/detail/layout.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace mvcc_hive::detail {

struct alignas(32) DiffRecord {
    CommitId commit_id;
    VirtualId virtual_id;
    TargetId previous_target;
    TargetId current_target;
};

static_assert(sizeof(DiffRecord) == 32);
static_assert(alignof(DiffRecord) == 32);
static_assert(std::is_trivially_copyable_v<DiffRecord>);

struct CommitRecord {
    CommitId id;
    CommitId parent;
    std::size_t first_diff;
    std::size_t diff_count;
};

static_assert(sizeof(CommitRecord) == 32);
static_assert(
    sizeof(CommitRecord) *
        BlockedVector<CommitRecord>::block_capacity ==
    64 * 1024);

struct PrunedCommitState {
    BlockedVector<CommitRecord> commits;
    BlockedVector<DiffRecord> diffs;
    CommitId first_commit_id = no_commit;
    bool cleared_floor = false;
};

struct TruncatedCommitState {
    std::size_t retained_commits = 0;
    std::size_t retained_diffs = 0;
    std::size_t commit_capacity = 0;
    std::size_t diff_capacity = 0;
    CommitId first_commit_id = no_commit;
    CommitId tip_commit = no_commit;
    bool changed = false;
};

class CommitStore {
public:
    CommitStore() = default;
    CommitStore(const CommitStore&) = delete;
    CommitStore& operator=(const CommitStore&) = delete;

    void reserve(
        std::size_t additional_commits,
        std::size_t additional_diffs) {
        commits_.reserve(additional_commits);
        diffs_.reserve(additional_diffs);
    }

    void reserve_size(
        std::size_t commit_target,
        std::size_t diff_target) {
        commits_.reserve(
            commit_target > commits_.size()
                ? commit_target - commits_.size()
                : 0);
        diffs_.reserve(
            diff_target > diffs_.size()
                ? diff_target - diffs_.size()
                : 0);
    }

    // No allocation or user code runs here.
    void append_prepared(
        CommitId id,
        CommitId parent,
        std::span<const DiffRecord> diffs) noexcept {
        assert(id != no_commit);
        assert(commits_.capacity() >= commits_.size() + 1);
        assert(diffs_.capacity() >= diffs_.size() + diffs.size());

        const CommitRecord record{
            .id = id,
            .parent = parent,
            .first_diff = diffs_.size(),
            .diff_count = diffs.size(),
        };
        commits_.append_prepared(record);
        for (const DiffRecord& diff : diffs) {
            diffs_.append_prepared(diff);
        }
        if (commits_.size() == 1) {
            first_commit_id_ = id;
            cleared_floor_ = false;
        }
        max_commit_id_ = std::max(max_commit_id_, id);
    }

    void publish_tip(CommitId id) noexcept {
        tip_commit_.store(id, std::memory_order_release);
    }

    [[nodiscard]] CommitId tip_commit() const noexcept {
        return tip_commit_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t commit_count() const noexcept {
        return commits_.size();
    }

    [[nodiscard]] std::size_t diff_count() const noexcept {
        return diffs_.size();
    }

    [[nodiscard]] std::size_t commit_capacity() const noexcept {
        return commits_.capacity();
    }

    [[nodiscard]] std::size_t diff_capacity() const noexcept {
        return diffs_.capacity();
    }

    [[nodiscard]] CommitId first_commit_id() const noexcept {
        return first_commit_id_;
    }

    [[nodiscard]] CommitId max_commit_id() const noexcept {
        return max_commit_id_;
    }

    [[nodiscard]] bool cleared_floor() const noexcept {
        return cleared_floor_;
    }

    [[nodiscard]] std::optional<std::size_t> commit_index(
        CommitId id) const noexcept {
        const std::size_t count = commits_.size();
        if (count == 0) {
            return std::nullopt;
        }

        const CommitId first = commits_[0].id;
        const CommitId last = commits_[count - 1].id;
        if (id < first || id > last) {
            return std::nullopt;
        }

        // Contiguous timelines retain the original O(1) lookup. Explicit
        // commit IDs may leave gaps, in which case the fallback is O(log n).
        const CommitId offset = id - first;
        if (offset < static_cast<CommitId>(count) &&
            commits_[static_cast<std::size_t>(offset)].id == id) {
            return static_cast<std::size_t>(offset);
        }

        std::size_t lo = 0;
        std::size_t hi = count;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (commits_[mid].id < id) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == count || commits_[lo].id != id) {
            return std::nullopt;
        }
        return lo;
    }

    [[nodiscard]] bool contains_commit(CommitId id) const noexcept {
        return id == no_commit || commit_index(id).has_value();
    }

    [[nodiscard]] std::optional<CommitId> next_commit(
        CommitId parent) const noexcept {
        const std::size_t count = commits_.size();
        if (count == 0) {
            return std::nullopt;
        }
        if (parent == no_commit) {
            if (commits_[0].parent != no_commit) {
                return std::nullopt;
            }
            return commits_[0].id;
        }

        const auto index = commit_index(parent);
        if (!index || *index + 1 >= count) {
            return std::nullopt;
        }
        const CommitRecord& next = commits_[*index + 1];
        if (next.parent != parent) {
            return std::nullopt;
        }
        return next.id;
    }

    [[nodiscard]] const CommitRecord& commit(
        std::size_t index) const noexcept {
        assert(index < commits_.size());
        return commits_[index];
    }

    [[nodiscard]] const DiffRecord& diff(
        std::size_t index) const noexcept {
        assert(index < diffs_.size());
        return diffs_[index];
    }

    // Builds replacement vectors without changing visible history.
    [[nodiscard]] PrunedCommitState prepare_prune(
        CommitId first_retained) const {
        PrunedCommitState state;
        if (commits_.empty()) {
            return state;
        }

        const CommitId first =
            first_retained < first_commit_id_
                ? first_commit_id_
                : first_retained;
        const auto start = commit_index(first);
        if (!start) {
            throw std::out_of_range(
                "first retained commit is not available");
        }

        const std::size_t removed_diffs =
            commits_[*start].first_diff;
        state.commits.reserve(commits_.size() - *start);
        state.diffs.reserve(diffs_.size() - removed_diffs);

        for (std::size_t index = *start;
             index < commits_.size();
             ++index) {
            CommitRecord record = commits_[index];
            record.first_diff -= removed_diffs;
            state.commits.push_back(record);
        }
        for (std::size_t index = removed_diffs;
             index < diffs_.size();
             ++index) {
            state.diffs.push_back(diffs_[index]);
        }
        state.first_commit_id = first;
        state.cleared_floor = cleared_floor_;
        return state;
    }

    // The audit log is optional metadata. Keep only a head baseline record.
    [[nodiscard]] PrunedCommitState prepare_clear_history(
        CommitId baseline) const {
        PrunedCommitState state;
        if (commits_.empty()) {
            return state;
        }

        state.commits.push_back(CommitRecord{
            .id = baseline,
            .parent = no_commit,
            .first_diff = 0,
            .diff_count = 0,
        });
        state.first_commit_id = baseline;
        state.cleared_floor = true;
        return state;
    }

    // Builds the retained prefix without changing visible state. Capacity for
    // the next commit is reserved in the replacement logs.
    [[nodiscard]] TruncatedCommitState prepare_truncate_after(
        CommitId checkout,
        std::size_t additional_commits,
        std::size_t additional_diffs) {
        TruncatedCommitState state;
        state.tip_commit = checkout;
        if (commits_.empty() || checkout == tip_commit()) {
            return state;
        }

        if (checkout == no_commit) {
            reserve_size(additional_commits, additional_diffs);
            state.commit_capacity = additional_commits;
            state.diff_capacity = additional_diffs;
            state.changed = true;
            return state;
        }

        const auto index = commit_index(checkout);
        if (!index) {
            throw std::out_of_range(
                "truncate point is not a retained commit");
        }

        const CommitRecord& last = commits_[*index];
        state.retained_commits = *index + 1;
        state.retained_diffs = last.first_diff + last.diff_count;
        state.commit_capacity =
            state.retained_commits + additional_commits;
        state.diff_capacity =
            state.retained_diffs + additional_diffs;
        state.first_commit_id = first_commit_id_;
        reserve_size(
            state.commit_capacity,
            state.diff_capacity);
        state.changed = true;
        return state;
    }

    void apply_prune(PrunedCommitState&& state) noexcept {
        commits_.swap(state.commits);
        diffs_.swap(state.diffs);
        first_commit_id_ = state.first_commit_id;
        cleared_floor_ = state.cleared_floor;
    }

    void apply_truncate(TruncatedCommitState&& state) noexcept {
        if (!state.changed) {
            return;
        }
        commits_.truncate(
            state.retained_commits,
            state.commit_capacity);
        diffs_.truncate(
            state.retained_diffs,
            state.diff_capacity);
        first_commit_id_ = state.first_commit_id;
        if (state.retained_commits == 0) {
            cleared_floor_ = false;
        }
        tip_commit_.store(state.tip_commit, std::memory_order_release);
    }

    void clear() noexcept {
        commits_.clear();
        diffs_.clear();
        tip_commit_.store(no_commit, std::memory_order_release);
        first_commit_id_ = no_commit;
        max_commit_id_ = no_commit;
        cleared_floor_ = false;
    }

private:
    BlockedVector<CommitRecord> commits_;
    BlockedVector<DiffRecord> diffs_;
    std::atomic<CommitId> tip_commit_{no_commit};
    CommitId first_commit_id_ = no_commit;
    CommitId max_commit_id_ = no_commit;
    bool cleared_floor_ = false;
};

}  // namespace mvcc_hive::detail
