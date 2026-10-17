#pragma once

#include <mvcc_hive/version_token.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mvcc_hive::detail {

struct CommitRecord {
    CommitId id;
    CommitId parent;
    std::size_t first_diff;
    std::size_t diff_count;
};

class CommitStore {
public:
    CommitStore() = default;
    CommitStore(const CommitStore&) = delete;
    CommitStore& operator=(const CommitStore&) = delete;

    void reserve(
        std::size_t additional_commits,
        std::size_t additional_diffs) {
        commits_.reserve(commits_.size() + additional_commits);
        diffs_.reserve(diffs_.size() + additional_diffs);
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

private:
    std::vector<CommitRecord> commits_;
    std::vector<Diff> diffs_;
    std::atomic<CommitId> head_commit_{no_commit};
};

}  // namespace mvcc_hive::detail
