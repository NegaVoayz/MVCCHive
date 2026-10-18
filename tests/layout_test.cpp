#include <mvcc_hive/mvcc_hive.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace {

using mvcc_hive::detail::BlockLayout;
using mvcc_hive::detail::RowNodeId;
using mvcc_hive::detail::RowNodeLayout;
using mvcc_hive::detail::TargetId;
using mvcc_hive::detail::no_row_node;
using mvcc_hive::detail::no_target;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "layout_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

struct Payload8 {
    std::array<std::byte, 8> bytes;
};

struct Payload64 {
    std::array<std::byte, 64> bytes;
};

struct Payload128 {
    std::array<std::byte, 128> bytes;
};

struct Payload392 {
    std::array<std::byte, 392> bytes;
};

struct alignas(64) OversizedPayload {
    std::array<std::byte, 16 * 1024 + 1> bytes;
};

template<class T>
void check_adaptive_layout(
    std::size_t expected_capacity, unsigned expected_shift) {
    using Layout = BlockLayout<T>;

    REQUIRE(Layout::capacity == expected_capacity);
    REQUIRE(Layout::shift == expected_shift);
    REQUIRE(Layout::slot_mask == expected_capacity - 1);
    REQUIRE(std::has_single_bit(Layout::capacity));
    REQUIRE(Layout::capacity * sizeof(T) <= Layout::payload_budget);
    REQUIRE(std::has_single_bit(Layout::capacity));
}

void check_target_reservation() {
    using Layout = BlockLayout<Payload64>;

    const TargetId zero = Layout::encode(0, 0);
    REQUIRE(zero == 0);
    REQUIRE(Layout::is_live_target(zero));

    const TargetId last =
        Layout::encode(Layout::max_block_index, Layout::slot_mask);
    REQUIRE(last == Layout::highest_live_target);
    REQUIRE(last < no_target);
    REQUIRE(Layout::is_live_target(last));

    REQUIRE(!Layout::is_live_target(no_target));
    REQUIRE(!Layout::can_encode(Layout::max_block_index + 1, 0));
}

void check_public_diff_layout() {
    using mvcc_hive::Diff;
    using mvcc_hive::DiffKind;

    static_assert(sizeof(Diff) == 24);
    static_assert(alignof(Diff) == 8);
    static_assert(std::is_trivially_copyable_v<Diff>);
    static_assert(std::is_standard_layout_v<Diff>);
    static_assert(offsetof(Diff, commit_id) == 0);
    static_assert(offsetof(Diff, virtual_id) == 8);
    static_assert(offsetof(Diff, kind) == 16);

    const Diff record{
        .commit_id = 3,
        .virtual_id = 7,
        .kind = DiffKind::update,
    };
    REQUIRE(record.had_previous());
    REQUIRE(record.has_current());
}

void check_single_slot_layout() {
    using Layout = BlockLayout<OversizedPayload>;

    REQUIRE(Layout::capacity == 1);
    REQUIRE(Layout::shift == 0);
    REQUIRE(Layout::slot_mask == 0);
    REQUIRE(Layout::max_block_index == no_target - 1);
    REQUIRE(Layout::encode(no_target - 1, 0) == no_target - 1);
    REQUIRE(Layout::is_live_target(no_target - 1));
    REQUIRE(!Layout::can_encode(no_target, 0));
}

void check_target_projection() {
    using Layout = BlockLayout<Payload128>;

    for (std::uint64_t block = 0; block < 4096; ++block) {
        for (std::size_t slot = 0; slot < Layout::capacity; ++slot) {
            const TargetId target = Layout::encode(block, slot);
            REQUIRE(Layout::block_index(target) ==
                    target / Layout::capacity);
            REQUIRE(Layout::slot_index(target) ==
                    target % Layout::capacity);
        }
    }
}

void check_row_node_projection() {
    REQUIRE(RowNodeLayout::capacity == 256);
    REQUIRE(RowNodeLayout::shift == 8);
    REQUIRE(RowNodeLayout::slot_mask == 255);
    REQUIRE(no_row_node == 0);

    const RowNodeId first = RowNodeLayout::encode(0, 1);
    const RowNodeId last_in_chunk =
        RowNodeLayout::encode(0, RowNodeLayout::slot_mask);
    const RowNodeId next_chunk = RowNodeLayout::encode(1, 0);

    REQUIRE(first == 1);
    REQUIRE(last_in_chunk == 255);
    REQUIRE(next_chunk == 256);
    REQUIRE(RowNodeLayout::chunk_index(first) == 0);
    REQUIRE(RowNodeLayout::slot_index(first) == 1);
    REQUIRE(RowNodeLayout::chunk_index(last_in_chunk) == 0);
    REQUIRE(RowNodeLayout::slot_index(last_in_chunk) == 255);
    REQUIRE(RowNodeLayout::chunk_index(next_chunk) == 1);
    REQUIRE(RowNodeLayout::slot_index(next_chunk) == 0);

    for (RowNodeId node = 1; node < 4096; ++node) {
        REQUIRE(RowNodeLayout::chunk_index(node) ==
                node / RowNodeLayout::capacity);
        REQUIRE(RowNodeLayout::slot_index(node) ==
                node % RowNodeLayout::capacity);
        REQUIRE(RowNodeLayout::is_live_node(node));
    }

    REQUIRE(!RowNodeLayout::is_live_node(no_row_node));
    REQUIRE(RowNodeLayout::can_encode(
        std::numeric_limits<RowNodeId>::max() >> RowNodeLayout::shift,
        RowNodeLayout::slot_mask));
    REQUIRE(!RowNodeLayout::can_encode(
        (std::numeric_limits<RowNodeId>::max() >>
         RowNodeLayout::shift) + 1,
        0));
}

}  // namespace

int main() {
    check_adaptive_layout<Payload8>(2048, 11);
    check_adaptive_layout<Payload64>(256, 8);
    check_adaptive_layout<Payload128>(128, 7);
    check_adaptive_layout<Payload392>(32, 5);
    check_target_reservation();
    check_public_diff_layout();
    check_single_slot_layout();
    check_target_projection();
    check_row_node_projection();
}
