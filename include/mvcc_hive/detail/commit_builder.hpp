#pragma once

#include <mvcc_hive/detail/commit_store.hpp>
#include <mvcc_hive/detail/current_targets.hpp>
#include <mvcc_hive/detail/row_index.hpp>

#include <algorithm>
#include <cassert>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mvcc_hive::detail {

class CommitBuilder {
public:
    CommitBuilder(
        CommitId commit_id,
        CommitId parent,
        RowIndex& rows,
        CommitStore& commits,
        CurrentTargetCache& current_targets,
        std::vector<DiffRecord> diffs)
        : commit_id_(commit_id),
          parent_(parent),
          rows_(rows),
          commits_(commits),
          current_targets_(current_targets),
          diffs_(std::move(diffs)) {}

    CommitBuilder(const CommitBuilder&) = delete;
    CommitBuilder& operator=(const CommitBuilder&) = delete;
    CommitBuilder(CommitBuilder&&) = delete;
    CommitBuilder& operator=(CommitBuilder&&) = delete;

    ~CommitBuilder() noexcept {
        if (!publish_started_) {
            release_prepared_nodes();
        }
    }

    void prepare() {
        if (prepared_ || publish_started_) {
            throw std::logic_error("commit builder prepared twice");
        }

        std::sort(
            diffs_.begin(), diffs_.end(),
            [](const DiffRecord& lhs, const DiffRecord& rhs) {
                return lhs.virtual_id < rhs.virtual_id;
            });
        for (std::size_t i = 1; i < diffs_.size(); ++i) {
            if (diffs_[i - 1].virtual_id == diffs_[i].virtual_id) {
                throw std::logic_error(
                    "commit contains duplicate virtual IDs");
            }
        }

        if (!diffs_.empty()) {
            rows_.get().ensure_rows(diffs_.back().virtual_id + 1);
            current_targets_.get().ensure_size(
                diffs_.back().virtual_id + 1);
        }
        commits_.get().reserve(1, diffs_.size());
        prepared_rows_.reserve(diffs_.size());

        for (const DiffRecord& diff : diffs_) {
            const RowNodeId old_head =
                rows_.get().newest(diff.virtual_id);
            const RowNodeId new_node = rows_.get().allocate_node(
                RowVersionNode{
                    .commit_id = commit_id_,
                    .target = diff.current_target,
                    .previous = old_head,
                });
            prepared_rows_.push_back(
                PreparedRow{.diff = diff, .new_node = new_node});

            const TargetId previous = rows_.get().visible_target(
                diff.virtual_id, parent_);
            if (previous != diff.previous_target) {
                throw std::logic_error(
                    "commit diff does not match its parent view");
            }
        }

        prepared_ = true;
    }

    // Publication performs no allocation and no user code.
    void publish() noexcept {
        assert(prepared_);
        assert(!publish_started_);
        publish_started_ = true;

        commits_.get().append_prepared(commit_id_, parent_, diffs_);
        for (const PreparedRow& row : prepared_rows_) {
            rows_.get().set_newest(row.diff.virtual_id, row.new_node);
        }
        for (const PreparedRow& row : prepared_rows_) {
            current_targets_.get().set(
                row.diff.virtual_id,
                row.diff.current_target);
        }
        commits_.get().publish_head(commit_id_);
        published_ = true;
    }

    [[nodiscard]] bool published() const noexcept {
        return published_;
    }

private:
    struct PreparedRow {
        DiffRecord diff;
        RowNodeId new_node;
    };

    void release_prepared_nodes() noexcept {
        for (const PreparedRow& row : prepared_rows_) {
            rows_.get().release_node(row.new_node);
        }
        prepared_rows_.clear();
    }

    CommitId commit_id_;
    CommitId parent_;
    std::reference_wrapper<RowIndex> rows_;
    std::reference_wrapper<CommitStore> commits_;
    std::reference_wrapper<CurrentTargetCache> current_targets_;
    std::vector<DiffRecord> diffs_;
    std::vector<PreparedRow> prepared_rows_;
    bool prepared_ = false;
    bool publish_started_ = false;
    bool published_ = false;
};

}  // namespace mvcc_hive::detail
