#pragma once

#include <mvcc_hive/version_token.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace mvcc_hive::detail {

struct CommitRecord {
    CommitId id;
    CommitId parent;
    std::size_t first_diff;
    std::size_t diff_count;
};

struct PrunedCommitState {
    std::vector<CommitRecord> commits;
    std::vector<Diff> diffs;
    CommitId first_commit_id = no_commit;
};

class CommitStore {
public:
    CommitStore() = default;
    CommitStore(const CommitStore&) = delete;
    CommitStore& operator=(const CommitStore&) = delete;

    void reserve(
        std::size_t additional_commits,
        std::size_t additional_diffs) {
        reserve_amortized(commits_, additional_commits);
        reserve_amortized(diffs_, additional_diffs);
    }

    // No allocation or user code runs here.
    void append_prepared(
        CommitId id,
        CommitId parent,
        std::span<const Diff> diffs) noexcept {
        assert(commits_.capacity() >= commits_.size() + 1);
        assert(diffs_.capacity() >= diffs_.size() + diffs.size());

        const CommitRecord record{
            .id = id,
            .parent = parent,
            .first_diff = diffs_.size(),
            .diff_count = diffs.size(),
        };
        commits_.push_back(record);
        for (const Diff& diff : diffs) {
            diffs_.push_back(diff);
        }
        if (commits_.size() == 1) {
            first_commit_id_ = id;
        }
    }

    void publish_head(CommitId id) noexcept {
        head_commit_.store(id, std::memory_order_release);
    }

    [[nodiscard]] CommitId head_commit() const noexcept {
        return head_commit_.load(std::memory_order_acquire);
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

    [[nodiscard]] std::optional<std::size_t> commit_index(
        CommitId id) const noexcept {
        if (commits_.empty() ||
            id < first_commit_id_ ||
            id > head_commit()) {
            return std::nullopt;
        }

        const std::size_t index = static_cast<std::size_t>(
            id - first_commit_id_);
        if (index >= commits_.size() ||
            commits_[index].id != id) {
            return std::nullopt;
        }
        return index;
    }

    [[nodiscard]] bool contains_commit(CommitId id) const noexcept {
        return id == no_commit || commit_index(id).has_value();
    }

    [[nodiscard]] const CommitRecord& commit(
        std::size_t index) const noexcept {
        assert(index < commits_.size());
        return commits_[index];
    }

    [[nodiscard]] const Diff& diff(
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
        return state;
    }

    // The audit log is optional metadata. Keep only a head baseline record.
    [[nodiscard]] PrunedCommitState prepare_clear_history() const {
        PrunedCommitState state;
        if (commits_.empty()) {
            return state;
        }

        const CommitId head = head_commit();
        state.commits.push_back(CommitRecord{
            .id = head,
            .parent = no_commit,
            .first_diff = 0,
            .diff_count = 0,
        });
        state.first_commit_id = head;
        return state;
    }

    void apply_prune(PrunedCommitState&& state) noexcept {
        commits_.swap(state.commits);
        diffs_.swap(state.diffs);
        first_commit_id_ = state.first_commit_id;
    }

    void clear() noexcept {
        std::vector<CommitRecord>{}.swap(commits_);
        std::vector<Diff>{}.swap(diffs_);
        head_commit_.store(no_commit, std::memory_order_release);
        first_commit_id_ = no_commit;
    }

private:
    template<class T>
    static void reserve_amortized(
        std::vector<T>& values,
        std::size_t additional) {
        if (additional > values.max_size() - values.size()) {
            throw std::length_error("commit log capacity overflow");
        }

        const std::size_t needed = values.size() + additional;
        if (needed <= values.capacity()) {
            return;
        }

        const std::size_t maximum = values.max_size();
        std::size_t grown = values.capacity();
        if (grown == 0) {
            grown = needed;
        } else if (grown > maximum / 2) {
            grown = maximum;
        } else {
            grown *= 2;
        }
        values.reserve(std::max(needed, grown));
    }

    std::vector<CommitRecord> commits_;
    std::vector<Diff> diffs_;
    std::atomic<CommitId> head_commit_{no_commit};
    CommitId first_commit_id_ = no_commit;
};

}  // namespace mvcc_hive::detail
