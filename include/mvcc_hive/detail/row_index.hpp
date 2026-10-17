#pragma once

#include <mvcc_hive/detail/layout.hpp>

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
#include <vector>

namespace mvcc_hive::detail {

class RowIndex {
public:
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
        RowNodeId node_id = newest(id);
        const RowVersionNode* node = nullptr;

        while (node_id != no_row_node) {
            auto current = node_at(node_id);
            if (!current) {
                return no_target;
            }
            node = &current->get();
            if (node->commit_id <= snapshot_commit) {
                break;
            }
            node_id = node->previous;
        }

        if (node_id == no_row_node || node == nullptr) {
            return no_target;
        }
        return node->target;
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
        add_available_chunk(chunk_index);
        --live_nodes_;
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
        return row_count_;
    }

    [[nodiscard]] std::size_t live_node_count() const noexcept {
        std::shared_lock lock{node_mutex_};
        return live_nodes_;
    }

    [[nodiscard]] std::size_t chunk_count() const noexcept {
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
            return live_nodes == RowNodeLayout::capacity;
        }

        std::array<std::uint64_t, free_words> free_bits{};
        std::size_t live_nodes = 0;
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

        const std::uint64_t chunk_index =
            static_cast<std::uint64_t>(chunks_.size());
        if (!RowNodeLayout::can_encode(
                chunk_index, RowNodeLayout::slot_mask)) {
            throw std::length_error(
                "row-node arena exhausted its index space");
        }

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

    mutable std::shared_mutex table_mutex_;
    std::deque<std::unique_ptr<RowPage>> pages_;
    std::size_t row_count_ = 0;

    mutable std::shared_mutex node_mutex_;
    std::deque<std::unique_ptr<RowNodeChunk>> chunks_;
    std::vector<std::uint64_t> available_chunks_;
    std::size_t live_nodes_ = 0;
};

}  // namespace mvcc_hive::detail
