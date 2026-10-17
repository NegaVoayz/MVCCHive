#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace mvcc_hive::detail {

using CommitId = std::uint64_t;
using VirtualId = std::uint64_t;
using TargetId = std::uint64_t;
using RowNodeId = std::uint64_t;

// Zero is useful as an "unset" commit only for bookkeeping. A real commit and
// every live commit ID start at one.
inline constexpr CommitId no_commit = 0;
inline constexpr TargetId no_target = std::numeric_limits<TargetId>::max();
inline constexpr RowNodeId no_row_node = 0;

template<class T>
struct BlockLayout {
    static_assert(sizeof(T) != 0);

    static constexpr std::size_t payload_budget = 16 * 1024;
    static constexpr std::size_t requested_slots =
        payload_budget / sizeof(T);
    static constexpr std::size_t capacity =
        requested_slots == 0 ? 1 : std::bit_floor(requested_slots);
    static_assert(std::has_single_bit(capacity));

    static constexpr unsigned shift = std::countr_zero(capacity);
    static constexpr std::size_t slot_mask = capacity - 1;

    // Reserve the block index containing the all-ones target. This makes the
    // sentinel structurally unreachable even with a shift of zero.
    static constexpr std::uint64_t max_block_index =
        (no_target >> shift) - 1;
    static constexpr TargetId highest_live_target =
        (max_block_index << shift) |
        static_cast<TargetId>(slot_mask);

    static_assert(max_block_index < no_target);
    static_assert(highest_live_target < no_target);

    static constexpr bool can_encode(
        std::uint64_t block_index, std::size_t slot_index) noexcept {
        return block_index <= max_block_index &&
               slot_index <= slot_mask;
    }

    static constexpr TargetId encode(
        std::uint64_t block_index, std::size_t slot_index) noexcept {
        return (block_index << shift) |
               static_cast<TargetId>(slot_index);
    }

    static constexpr std::uint64_t block_index(
        TargetId target) noexcept {
        return target >> shift;
    }

    static constexpr std::size_t slot_index(TargetId target) noexcept {
        return static_cast<std::size_t>(target & slot_mask);
    }

    static constexpr bool is_live_target(TargetId target) noexcept {
        return target != no_target &&
               block_index(target) <= max_block_index;
    }
};

struct RowNodeLayout {
    static constexpr std::size_t capacity = 256;
    static_assert(std::has_single_bit(capacity));

    static constexpr unsigned shift = std::countr_zero(capacity);
    static constexpr std::size_t slot_mask = capacity - 1;

    static constexpr bool can_encode(
        std::uint64_t chunk_index, std::size_t slot_index) noexcept {
        return chunk_index <=
                   (std::numeric_limits<RowNodeId>::max() >> shift) &&
               slot_index <= slot_mask;
    }

    static constexpr RowNodeId encode(
        std::uint64_t chunk_index, std::size_t slot_index) noexcept {
        return (chunk_index << shift) |
               static_cast<RowNodeId>(slot_index);
    }

    static constexpr std::uint64_t chunk_index(RowNodeId node) noexcept {
        return node >> shift;
    }

    static constexpr std::size_t slot_index(RowNodeId node) noexcept {
        return static_cast<std::size_t>(node & slot_mask);
    }

    static constexpr bool is_live_node(RowNodeId node) noexcept {
        return node != no_row_node;
    }
};

struct RowVersionNode {
    CommitId commit_id;
    TargetId target;
    RowNodeId previous;
};

struct RowHead {
    std::atomic<RowNodeId> newest{no_row_node};
};

static_assert(sizeof(RowVersionNode) == 24);
static_assert(std::is_trivially_copyable_v<RowVersionNode>);
static_assert(sizeof(RowHead) == 8);
static_assert(std::atomic<CommitId>::is_always_lock_free);
static_assert(std::atomic<RowNodeId>::is_always_lock_free);

}  // namespace mvcc_hive::detail
