#pragma once

#include <mvcc_hive/detail/layout.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mvcc_hive::detail {

class RowIndex {
public:
    class PrunePlan {
    public:
        [[nodiscard]] const std::vector<TargetId>& released_targets()
            const noexcept {
            return released_targets_;
        }

    private:
        friend class RowIndex;

        struct TailLink {
            RowNodeId tail;
            RowNodeId first_released;
        };

        std::vector<TailLink> links_;
        std::vector<RowNodeId> release_nodes_;
        std::vector<TargetId> released_targets_;
    };

    class TruncatePlan {
    public:
        TruncatePlan() = default;
        TruncatePlan(const TruncatePlan&) = delete;
        TruncatePlan& operator=(const TruncatePlan&) = delete;
        TruncatePlan(TruncatePlan&&) noexcept = default;
        TruncatePlan& operator=(TruncatePlan&&) noexcept = default;

        [[nodiscard]] std::vector<RowNodeId>& released_nodes()
            noexcept {
            return released_nodes_;
        }

        [[nodiscard]] std::vector<TargetId>& released_targets()
            noexcept {
            return released_targets_;
        }

    private:
        friend class RowIndex;

        struct HeadLink {
            VirtualId id;
            RowNodeId new_head;
        };

        std::vector<HeadLink> heads_;
        std::vector<RowNodeId> released_nodes_;
        std::vector<TargetId> released_targets_;
    };

    static constexpr std::size_t row_page_capacity = 256;
    static_assert(std::has_single_bit(row_page_capacity));

    static constexpr unsigned row_page_shift =
        std::countr_zero(row_page_capacity);
    static constexpr std::size_t row_page_mask =
        row_page_capacity - 1;

    RowIndex() = default;
    RowIndex(const RowIndex&) = delete;
    RowIndex& operator=(const RowIndex&) = delete;
    RowIndex(RowIndex&&) = delete;
    RowIndex& operator=(RowIndex&&) = delete;

    void ensure_rows(VirtualId count) {
        if (count <= row_count_) {
            return;
        }

        const std::uint64_t page_count =
            (count + row_page_capacity - 1) >> row_page_shift;
        std::unique_lock lock{table_mutex_};
        if (page_count > pages_.size()) {
            pages_.resize(static_cast<std::size_t>(page_count));
        }
        for (auto& page : pages_) {
            if (!page) {
                page = std::make_unique<RowPage>();
            }
        }
        row_count_ = count;
    }

    [[nodiscard]] RowNodeId newest(VirtualId id) const noexcept {
        auto head = find_head(id);
        if (!head) {
            return no_row_node;
        }
        return head->get().newest.load(std::memory_order_acquire);
    }

    void set_newest(VirtualId id, RowNodeId node) noexcept {
        auto head = find_head(id);
        assert(head);
        head->get().newest.store(node, std::memory_order_release);
    }

    [[nodiscard]] TargetId visible_target(
        VirtualId id, CommitId snapshot_commit) const noexcept {
        const RowNodeId node_id =
            visible_node(id, snapshot_commit);
        if (node_id == no_row_node) {
            return no_target;
        }
        auto node = node_at(node_id);
        if (!node) {
            return no_target;
        }
        return node->get().target;
    }

    [[nodiscard]] RowNodeId visible_node(
        VirtualId id, CommitId snapshot_commit) const noexcept {
        RowNodeId node_id = newest(id);
        while (node_id != no_row_node) {
            auto current = node_at(node_id);
            if (!current) {
                return no_row_node;
            }
            if (current->get().commit_id <= snapshot_commit) {
                return node_id;
            }
            node_id = current->get().previous;
        }
        return no_row_node;
    }

    [[nodiscard]] RowNodeId allocate_node(
        const RowVersionNode& node) {
        std::unique_lock lock{node_mutex_};
        const std::uint64_t chunk_index = acquire_chunk_with_space();
        RowNodeChunk& chunk = *chunks_[chunk_index];
        const std::size_t slot = chunk.take_free_slot();
        assert(slot < RowNodeLayout::capacity);
        chunk.nodes[slot] = node;
        if (chunk.is_full()) {
            remove_available_chunk(chunk_index);
        }
        ++live_nodes_;
        return RowNodeLayout::encode(chunk_index, slot);
    }

    void release_node(RowNodeId node_id) noexcept {
        if (node_id == no_row_node) {
            return;
        }

        std::unique_lock lock{node_mutex_};
        release_node_locked(node_id);
    }

    // Writer-only planning pass. It allocates before any mutation, so the
    // caller can reserve exactly the reclamation it needs and then apply.
    [[nodiscard]] PrunePlan prepare_prune(
        CommitId retained_from,
        std::optional<CommitId> oldest_active = std::nullopt) {
        PrunePlan plan;
        const std::size_t rows = row_count();

        std::shared_lock lock{node_mutex_};
        for (VirtualId id = 0; id < rows; ++id) {
            RowNodeId head = newest(id);
            const auto first_at_or_before =
                [this](RowNodeId start, CommitId cutoff) {
                    RowNodeId cursor = start;
                    while (cursor != no_row_node) {
                        if (node_locked(cursor).commit_id <= cutoff) {
                            return cursor;
                        }
                        cursor = node_locked(cursor).previous;
                    }
                    return no_row_node;
                };

            RowNodeId tail =
                first_at_or_before(head, retained_from);
            if (oldest_active &&
                *oldest_active < retained_from) {
                const RowNodeId older =
                    first_at_or_before(head, *oldest_active);
                tail = older;
            }
            if (tail == no_row_node) {
                continue;
            }
            const RowNodeId first_released =
                node_locked(tail).previous;
            if (first_released == no_row_node) {
                continue;
            }

            plan.links_.push_back(PrunePlan::TailLink{
                .tail = tail,
                .first_released = first_released,
            });
            for (RowNodeId cursor = first_released;
                 cursor != no_row_node;) {
                const RowVersionNode& node = node_locked(cursor);
                if (node.target != no_target) {
                    plan.released_targets_.push_back(node.target);
                }
                plan.release_nodes_.push_back(cursor);
                cursor = node.previous;
            }
        }
        return plan;
    }

    void apply_prune(const PrunePlan& plan) noexcept {
        std::unique_lock lock{node_mutex_};
        for (const PrunePlan::TailLink& link : plan.links_) {
            node_locked(link.tail).previous = no_row_node;
        }
        for (const RowNodeId node_id : plan.release_nodes_) {
            release_node_locked(node_id);
        }
    }

    // Writer-only planning pass. Future nodes are detached from their row
    // heads but are not released here; the caller decides when the retired
    // chain is safe to reclaim.
    [[nodiscard]] TruncatePlan prepare_truncate(
        CommitId retain_through) {
        TruncatePlan plan;
        const std::size_t rows = row_count();

        for (VirtualId id = 0; id < rows; ++id) {
            const RowNodeId head = newest(id);
            if (head == no_row_node) {
                continue;
            }

            RowNodeId tail = no_row_node;
            for (RowNodeId cursor = head;
                 cursor != no_row_node;) {
                auto node = node_at(cursor);
                if (!node) {
                    break;
                }
                if (node->get().commit_id <= retain_through) {
                    tail = cursor;
                    break;
                }
                cursor = node->get().previous;
            }

            if (tail == head) {
                continue;
            }

            plan.heads_.push_back(
                TruncatePlan::HeadLink{
                    .id = id,
                    .new_head = tail,
                });
            for (RowNodeId cursor = head;
                 cursor != tail;) {
                auto node = node_at(cursor);
                if (!node) {
                    break;
                }
                const RowVersionNode& value = node->get();
                if (value.target != no_target) {
                    plan.released_targets_.push_back(value.target);
                }
                plan.released_nodes_.push_back(cursor);
                cursor = value.previous;
            }
        }
        return plan;
    }

    void apply_truncate(const TruncatePlan& plan) noexcept {
        for (const TruncatePlan::HeadLink& link : plan.heads_) {
            set_newest(link.id, link.new_head);
        }
    }

    // Convenience for tests and callers with no target anchors to release.
    [[nodiscard]] std::vector<TargetId> prune_rows(
        CommitId retained_from,
        std::optional<CommitId> oldest_active = std::nullopt) {
        auto plan = prepare_prune(retained_from, oldest_active);
        auto released_targets =
            std::move(plan.released_targets_);
        apply_prune(plan);
        return released_targets;
    }

    [[nodiscard]] std::optional<
        std::reference_wrapper<const RowVersionNode>>
    node_at(RowNodeId node_id) const noexcept {
        if (node_id == no_row_node) {
            return std::nullopt;
        }

        std::shared_lock lock{node_mutex_};
        const std::uint64_t chunk_index =
            RowNodeLayout::chunk_index(node_id);
        if (chunk_index >= chunks_.size() || !chunks_[chunk_index]) {
            return std::nullopt;
        }

        const auto& chunk = chunks_[chunk_index];
        return std::cref(
            chunk->nodes[RowNodeLayout::slot_index(node_id)]);
    }

    [[nodiscard]] std::size_t row_count() const noexcept {
        std::shared_lock lock{table_mutex_};
        return row_count_;
    }

    [[nodiscard]] std::size_t live_node_count() const noexcept {
        std::shared_lock lock{node_mutex_};
        return live_nodes_;
    }

    [[nodiscard]] std::size_t chunk_count() const noexcept {
        std::shared_lock lock{node_mutex_};
        std::size_t count = 0;
        for (const auto& chunk : chunks_) {
            count += chunk ? 1 : 0;
        }
        return count;
    }

    [[nodiscard]] std::size_t chunk_directory_slots() const noexcept {
        std::shared_lock lock{node_mutex_};
        return chunks_.size();
    }

private:
    struct RowPage {
        std::array<RowHead, row_page_capacity> heads{};
    };

    struct RowNodeChunk {
        static constexpr std::size_t free_words =
            RowNodeLayout::capacity / 64;

        RowNodeChunk(std::size_t chunk_index) noexcept {
            free_bits.fill(~std::uint64_t{0});
            if (chunk_index == 0) {
                free_bits[0] &= ~std::uint64_t{1};
            }
            usable_slots_ =
                chunk_index == 0
                    ? RowNodeLayout::capacity - 1
                    : RowNodeLayout::capacity;
        }

        [[nodiscard]] std::size_t take_free_slot() noexcept {
            for (std::size_t word = 0; word < free_words; ++word) {
                const std::uint64_t available = free_bits[word];
                if (available == 0) {
                    continue;
                }
                const unsigned bit = std::countr_zero(available);
                free_bits[word] =
                    available & (available - std::uint64_t{1});
                ++live_nodes;
                return word * 64 + bit;
            }
            return RowNodeLayout::capacity;
        }

        void return_free_slot(std::size_t slot) noexcept {
            free_bits[slot / 64] |=
                std::uint64_t{1} << (slot % 64);
            --live_nodes;
        }

        [[nodiscard]] bool slot_is_free(std::size_t slot) const noexcept {
            return (free_bits[slot / 64] &
                    (std::uint64_t{1} << (slot % 64))) != 0;
        }

        [[nodiscard]] bool is_full() const noexcept {
            return live_nodes == usable_slots_;
        }

        std::array<std::uint64_t, free_words> free_bits{};
        std::size_t live_nodes = 0;
        std::size_t usable_slots_ = 0;
        std::array<RowVersionNode, RowNodeLayout::capacity> nodes{};
    };

    [[nodiscard]] std::optional<std::reference_wrapper<RowHead>>
    find_head(VirtualId id) const noexcept {
        std::shared_lock lock{table_mutex_};
        const std::uint64_t page_index = id >> row_page_shift;
        if (page_index >= pages_.size()) {
            return std::nullopt;
        }
        const auto& page = pages_[static_cast<std::size_t>(page_index)];
        if (!page) {
            return std::nullopt;
        }
        return std::ref(page->heads[id & row_page_mask]);
    }

    [[nodiscard]] std::uint64_t acquire_chunk_with_space() {
        while (!available_chunks_.empty()) {
            const std::uint64_t chunk_index = available_chunks_.back();
            if (chunk_index < chunks_.size() &&
                chunks_[chunk_index] &&
                !chunks_[chunk_index]->is_full()) {
                return chunk_index;
            }
            available_chunks_.pop_back();
        }

        if (!free_chunks_.empty()) {
            const std::uint64_t chunk_index = free_chunks_.back();
            auto chunk =
                std::make_unique<RowNodeChunk>(chunk_index);
            free_chunks_.pop_back();
            chunks_[chunk_index] = std::move(chunk);
            available_chunks_.push_back(chunk_index);
            return chunk_index;
        }

        const std::uint64_t chunk_index =
            static_cast<std::uint64_t>(chunks_.size());
        if (!RowNodeLayout::can_encode(
                chunk_index, RowNodeLayout::slot_mask)) {
            throw std::length_error(
                "row-node arena exhausted its index space");
        }

        free_chunks_.reserve(chunks_.size() + 1);
        chunks_.push_back(std::make_unique<RowNodeChunk>(chunk_index));
        available_chunks_.push_back(chunk_index);
        return chunk_index;
    }

    void remove_available_chunk(
        std::uint64_t chunk_index) noexcept {
        for (auto it = available_chunks_.begin();
             it != available_chunks_.end();
             ++it) {
            if (*it == chunk_index) {
                *it = available_chunks_.back();
                available_chunks_.pop_back();
                return;
            }
        }
    }

    void add_available_chunk(std::uint64_t chunk_index) {
        for (const std::uint64_t current : available_chunks_) {
            if (current == chunk_index) {
                return;
            }
        }
        available_chunks_.push_back(chunk_index);
    }

    [[nodiscard]] RowVersionNode& node_locked(
        RowNodeId node_id) noexcept {
        const std::uint64_t chunk_index =
            RowNodeLayout::chunk_index(node_id);
        assert(chunk_index < chunks_.size());
        assert(chunks_[chunk_index]);
        return chunks_[chunk_index]
            ->nodes[RowNodeLayout::slot_index(node_id)];
    }

    void release_node_locked(RowNodeId node_id) noexcept {
        const std::uint64_t chunk_index =
            RowNodeLayout::chunk_index(node_id);
        if (chunk_index >= chunks_.size() || !chunks_[chunk_index]) {
            return;
        }

        RowNodeChunk& chunk = *chunks_[chunk_index];
        const std::size_t slot = RowNodeLayout::slot_index(node_id);
        if (chunk.slot_is_free(slot)) {
            return;
        }

        chunk.return_free_slot(slot);
        --live_nodes_;
        if (chunk.live_nodes == 0) {
            release_chunk_locked(chunk_index);
        } else {
            add_available_chunk(chunk_index);
        }
    }

    void release_chunk_locked(std::uint64_t chunk_index) noexcept {
        remove_available_chunk(chunk_index);
        chunks_[chunk_index].reset();
        assert(free_chunks_.capacity() > free_chunks_.size());
        free_chunks_.push_back(chunk_index);
        trim_trailing_chunks_locked();
    }

    void trim_trailing_chunks_locked() noexcept {
        while (!chunks_.empty() && !chunks_.back()) {
            const std::uint64_t chunk_index =
                static_cast<std::uint64_t>(chunks_.size() - 1);
            free_chunks_.erase(
                std::remove(
                    free_chunks_.begin(),
                    free_chunks_.end(),
                    chunk_index),
                free_chunks_.end());
            chunks_.pop_back();
        }
        if (chunks_.empty()) {
            std::deque<std::unique_ptr<RowNodeChunk>>{}.swap(chunks_);
            std::vector<std::uint64_t>{}.swap(available_chunks_);
            std::vector<std::uint64_t>{}.swap(free_chunks_);
        }
    }

    mutable std::shared_mutex table_mutex_;
    std::deque<std::unique_ptr<RowPage>> pages_;
    std::size_t row_count_ = 0;

    mutable std::shared_mutex node_mutex_;
    std::deque<std::unique_ptr<RowNodeChunk>> chunks_;
    std::vector<std::uint64_t> available_chunks_;
    std::vector<std::uint64_t> free_chunks_;
    std::size_t live_nodes_ = 0;
};

}  // namespace mvcc_hive::detail
