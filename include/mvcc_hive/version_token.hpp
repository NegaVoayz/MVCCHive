#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include <mvcc_hive/detail/layout.hpp>

namespace mvcc_hive {

namespace detail {
struct VersionTokenAccess;
}  // namespace detail

// Opaque equality-comparable identity for a target location in the retained
// trace. It is deliberately not a capability: it cannot resolve payloads or
// mutate the hive.
class VersionToken {
public:
    constexpr VersionToken() noexcept = default;

    static constexpr VersionToken none() noexcept {
        return VersionToken{};
    }

    constexpr bool has_value() const noexcept {
        return bits_ != none_bits;
    }

    friend constexpr bool operator==(
        VersionToken lhs, VersionToken rhs) noexcept = default;

private:
    friend struct detail::VersionTokenAccess;

    static constexpr std::uint64_t none_bits = detail::no_target;

    explicit constexpr VersionToken(std::uint64_t bits) noexcept
        : bits_(bits) {}

    std::uint64_t bits_ = none_bits;
};

static_assert(sizeof(VersionToken) == 8);

namespace detail {

struct VersionTokenAccess {
    static constexpr bool none_matches_target_sentinel() noexcept {
        return VersionToken::none_bits == no_target;
    }

    static constexpr VersionToken from_target(TargetId target) noexcept {
        return VersionToken{target};
    }

    static constexpr TargetId target(VersionToken token) noexcept {
        return token.bits_;
    }
};

static_assert(VersionTokenAccess::none_matches_target_sentinel());

}  // namespace detail

struct alignas(32) Diff {
    detail::CommitId commit_id;
    detail::VirtualId virtual_id;
    VersionToken previous_target;
    VersionToken current_target;
};

static_assert(sizeof(Diff) == 32);
static_assert(alignof(Diff) == 32);
static_assert(std::is_trivially_copyable_v<Diff>);

}  // namespace mvcc_hive
