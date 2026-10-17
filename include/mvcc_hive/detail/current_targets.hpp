#pragma once

#include <mvcc_hive/detail/row_index.hpp>

#include <cstddef>
#include <vector>

namespace mvcc_hive::detail {

class CurrentTargetCache {
public:
    CurrentTargetCache() = default;
    CurrentTargetCache(const CurrentTargetCache&) = delete;
    CurrentTargetCache& operator=(const CurrentTargetCache&) = delete;

    void ensure_size(std::size_t size) {
        if (values_.size() < size) {
            values_.resize(size, no_target);
        }
    }

    void set(VirtualId id, TargetId target) noexcept {
        if (id < values_.size()) {
            values_[id] = target;
        }
    }

    [[nodiscard]] TargetId get(VirtualId id) const noexcept {
        return id < values_.size() ? values_[id] : no_target;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return values_.size();
    }

    // Writer-only repair path. The authoritative row heads supply every
    // entry, including rows whose cache entry was damaged.
    void rebuild(
        const RowIndex& rows, CommitId head_commit) {
        values_.assign(rows.row_count(), no_target);
        for (VirtualId id = 0; id < values_.size(); ++id) {
            RowNodeId node_id = rows.newest(id);
            while (node_id != no_row_node) {
                auto node = rows.node_at(node_id);
                if (!node) {
                    break;
                }
                if (node->get().commit_id <= head_commit) {
                    values_[id] = node->get().target;
                    break;
                }
                node_id = node->get().previous;
            }
        }
    }

private:
    std::vector<TargetId> values_;
};

}  // namespace mvcc_hive::detail
