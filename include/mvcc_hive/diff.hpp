#pragma once

#include <cstdint>
#include <type_traits>

namespace mvcc_hive {

using CommitId = std::uint64_t;
using VirtualId = std::uint64_t;

enum class DiffKind : std::uint8_t {
    insert,
    update,
    remove,
};

// Public history record. Target locations remain private to the hive; callers
// inspect values through snapshots instead of decoding storage identities.
struct Diff {
    CommitId commit_id;
    VirtualId virtual_id;
    DiffKind kind;

    [[nodiscard]] constexpr bool had_previous() const noexcept {
        return kind != DiffKind::insert;
    }

    [[nodiscard]] constexpr bool has_current() const noexcept {
        return kind != DiffKind::remove;
    }
};

static_assert(sizeof(Diff) == 24);
static_assert(alignof(Diff) == 8);
static_assert(std::is_trivially_copyable_v<Diff>);
static_assert(std::is_standard_layout_v<Diff>);

}  // namespace mvcc_hive
