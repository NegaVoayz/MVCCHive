#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace mvcc_hive::detail {

template<class T, std::size_t BlockCapacity = 2048>
class BlockedVector {
public:
    static constexpr std::size_t block_capacity = BlockCapacity;
    static_assert(std::has_single_bit(block_capacity));
    static_assert(std::is_default_constructible_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>);
    static_assert(std::is_nothrow_copy_assignable_v<T>);

    BlockedVector() = default;
    BlockedVector(const BlockedVector&) = delete;
    BlockedVector& operator=(const BlockedVector&) = delete;

    BlockedVector(BlockedVector&& other) noexcept
        : blocks_(std::move(other.blocks_)),
          size_(std::exchange(other.size_, 0)) {}

    BlockedVector& operator=(BlockedVector&& other) noexcept {
        if (this != &other) {
            blocks_ = std::move(other.blocks_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    void swap(BlockedVector& other) noexcept {
        blocks_.swap(other.blocks_);
        std::swap(size_, other.size_);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    // Block-aligned capacity. Allocation happens here, never in
    // append_prepared().
    [[nodiscard]] std::size_t capacity() const noexcept {
        return blocks_.size() * block_capacity;
    }

    void reserve(std::size_t additional) {
        if (additional > maximum_size() - size_) {
            throw std::length_error("blocked vector capacity overflow");
        }

        const std::size_t needed = size_ + additional;
        if (needed == 0) {
            return;
        }

        const std::size_t blocks_needed =
            (needed >> block_shift) +
            ((needed & block_mask) != 0 ? 1 : 0);
        if (blocks_needed <= blocks_.size()) {
            return;
        }

        if (blocks_.capacity() < blocks_needed) {
            const std::size_t grown =
                blocks_.capacity() == 0
                    ? blocks_needed
                    : std::max(
                          blocks_needed,
                          blocks_.capacity() * 2);
            blocks_.reserve(grown);
        }
        while (blocks_.size() < blocks_needed) {
            blocks_.push_back(std::make_unique<Block>());
        }
    }

    void push_back(const T& value) {
        reserve(1);
        append_prepared(value);
    }

    void append_prepared(const T& value) noexcept {
        assert(size_ < capacity());
        value_at(size_) = value;
        ++size_;
    }

    [[nodiscard]] T& operator[](std::size_t index) noexcept {
        assert(index < size_);
        return value_at(index);
    }

    [[nodiscard]] const T& operator[](
        std::size_t index) const noexcept {
        assert(index < size_);
        return value_at(index);
    }

    void clear() noexcept {
        BlockedVector{}.swap(*this);
    }

private:
    static constexpr unsigned block_shift =
        std::countr_zero(block_capacity);
    static constexpr std::size_t block_mask = block_capacity - 1;

    struct Block {
        Block() noexcept {}
        std::array<T, block_capacity> values;
    };

    [[nodiscard]] static constexpr std::size_t maximum_size() noexcept {
        return std::numeric_limits<std::size_t>::max();
    }

    [[nodiscard]] T& value_at(std::size_t index) noexcept {
        const std::size_t block = index >> block_shift;
        return blocks_[block]->values[index & block_mask];
    }

    [[nodiscard]] const T& value_at(std::size_t index) const noexcept {
        const std::size_t block = index >> block_shift;
        return blocks_[block]->values[index & block_mask];
    }

    std::vector<std::unique_ptr<Block>> blocks_;
    std::size_t size_ = 0;
};

}  // namespace mvcc_hive::detail
