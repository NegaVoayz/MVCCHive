#pragma once

#include <mvcc_hive/detail/layout.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace mvcc_hive::detail {

struct BlockHeader {
    std::size_t live_versions = 0;
    std::size_t free_word_hint = 0;
};

template<class T>
struct VersionBlock {
    static_assert(std::is_nothrow_destructible_v<T>);

    using Layout = BlockLayout<T>;

    static constexpr std::size_t free_words =
        (Layout::capacity + 63) / 64;

    VersionBlock() noexcept {
        free_bits.fill(~std::uint64_t{0});
        if constexpr (Layout::capacity % 64 != 0) {
            constexpr unsigned tail_bits = Layout::capacity % 64;
            free_bits.back() =
                (std::uint64_t{1} << tail_bits) - std::uint64_t{1};
        }
        for (auto& view : views) {
            view.store(0, std::memory_order_relaxed);
        }
    }

    VersionBlock(const VersionBlock&) = delete;
    VersionBlock& operator=(const VersionBlock&) = delete;
    VersionBlock(VersionBlock&&) = delete;
    VersionBlock& operator=(VersionBlock&&) = delete;

    [[nodiscard]] T* payload_at(std::size_t slot) noexcept {
        return std::launder(
            reinterpret_cast<T*>(payload + slot * sizeof(T)));
    }

    [[nodiscard]] const T* payload_at(std::size_t slot) const noexcept {
        return std::launder(
            reinterpret_cast<const T*>(payload + slot * sizeof(T)));
    }

    [[nodiscard]] bool is_full() const noexcept {
        return header.live_versions == Layout::capacity;
    }

    [[nodiscard]] bool is_empty() const noexcept {
        return header.live_versions == 0;
    }

    [[nodiscard]] std::size_t take_free_slot() noexcept {
        for (std::size_t word_offset = 0;
             word_offset < free_words;
             ++word_offset) {
            const std::size_t word_index =
                (header.free_word_hint + word_offset) % free_words;
            const std::uint64_t available = free_bits[word_index];
            if (available == 0) {
                continue;
            }

            const unsigned bit = std::countr_zero(available);
            free_bits[word_index] =
                available & (available - std::uint64_t{1});
            header.free_word_hint = word_index;
            ++header.live_versions;

            return word_index * 64 + bit;
        }

        return Layout::capacity;
    }

    void return_free_slot(std::size_t slot) noexcept {
        const std::size_t word_index = slot / 64;
        const unsigned bit = static_cast<unsigned>(slot % 64);
        free_bits[word_index] |= std::uint64_t{1} << bit;
        header.free_word_hint =
            word_index < header.free_word_hint
                ? word_index
                : header.free_word_hint;
        --header.live_versions;
    }

    [[nodiscard]] bool slot_is_free(std::size_t slot) const noexcept {
        const std::size_t word_index = slot / 64;
        const unsigned bit = static_cast<unsigned>(slot % 64);
        return (free_bits[word_index] & (std::uint64_t{1} << bit)) != 0;
    }

    BlockHeader header{};

    // Writer-only slot state.
    std::array<std::uint64_t, free_words> free_bits{};

    // Reader-visible anchor counts, one per payload slot.
    std::array<std::atomic<std::uint32_t>, Layout::capacity> views{};

    // Raw payload storage. No T object is constructed by block allocation.
    alignas(T) std::byte payload[Layout::capacity * sizeof(T)];
};

template<class T>
inline constexpr std::size_t VersionBlock<T>::free_words;

}  // namespace mvcc_hive::detail
