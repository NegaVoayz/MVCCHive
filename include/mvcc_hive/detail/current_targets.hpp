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

    void clear() noexcept {
        std::vector<TargetId>{}.swap(values_);
    }

    // Writer-only repair path. The authoritative row heads supply every
    // entry, including rows whose cache entry was damaged.
    [[nodiscard]] std::vector<TargetId> prepare_rebuild(
        const RowIndex& rows, CommitId checkout) const {
        std::vector<TargetId> values(rows.row_count(), no_target);
        for (VirtualId id = 0; id < values.size(); ++id) {
            const RowNodeId node_id =
                rows.visible_node(id, checkout);
            if (node_id == no_row_node) {
                continue;
            }
            auto node = rows.node_at(node_id);
            if (node) {
                values[id] = node->get().target;
            }
        }
        return values;
    }

    void apply_rebuild(std::vector<TargetId>&& values) noexcept {
        values_.swap(values);
    }

    void rebuild(const RowIndex& rows, CommitId checkout) {
        apply_rebuild(prepare_rebuild(rows, checkout));
    }

private:
    std::vector<TargetId> values_;
};

}  // namespace mvcc_hive::detail
