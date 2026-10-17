#pragma once

#include <mvcc_hive/detail/block_directory.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace mvcc_hive::detail {

template<class T>
class VersionStore {
public:
    static_assert(std::is_nothrow_destructible_v<T>);

    using Layout = BlockLayout<T>;

    VersionStore() = default;
    VersionStore(const VersionStore&) = delete;
    VersionStore& operator=(const VersionStore&) = delete;
    VersionStore(VersionStore&&) = delete;
    VersionStore& operator=(VersionStore&&) = delete;

    ~VersionStore() {
        destroy_all_payloads();
    }

    template<class... Args>
    [[nodiscard]] TargetId emplace(
        std::uint32_t initial_views, Args&&... args) {
        assert(initial_views != 0);

        const std::uint64_t block_index = acquire_block_with_space();
        VersionBlock<T>& block = *owned_blocks_[block_index];
        const std::size_t slot = block.take_free_slot();
        assert(slot < Layout::capacity);

        try {
            std::construct_at(
                block.payload_at(slot), std::forward<Args>(args)...);
        } catch (...) {
            block.return_free_slot(slot);
            throw;
        }

        block.views[slot].store(
            initial_views, std::memory_order_relaxed);
        if (block.is_full()) {
            remove_available_block(block_index);
        }

        return Layout::encode(block_index, slot);
    }

    void acquire_view(TargetId target) noexcept {
        assert(Layout::is_live_target(target));
        VersionBlock<T>& block = require_block(target);
        const std::size_t slot = Layout::slot_index(target);
        block.views[slot].fetch_add(1, std::memory_order_relaxed);
    }

    void release_view(TargetId target) noexcept {
        if (!Layout::is_live_target(target)) {
            return;
        }

        auto block = directory_.find(Layout::block_index(target));
        if (!block) {
            return;
        }

        const std::size_t slot = Layout::slot_index(target);
        const std::uint32_t previous =
            block->get().views[slot].fetch_sub(
                1, std::memory_order_acq_rel);
        assert(previous != 0);
        if (previous == 1) {
            std::lock_guard lock{retirement_mutex_};
            retired_targets_.push_back(target);
        }
    }

    [[nodiscard]] std::optional<
        std::reference_wrapper<const T>>
    resolve(TargetId target) const noexcept {
        if (!Layout::is_live_target(target)) {
            return std::nullopt;
        }

        auto block = directory_.find(Layout::block_index(target));
        if (!block) {
            return std::nullopt;
        }

        const std::size_t slot = Layout::slot_index(target);
        if (block->get().views[slot].load(
                std::memory_order_acquire) == 0) {
            return std::nullopt;
        }
        return std::cref(*block->get().payload_at(slot));
    }

    // Writer-only. Retires targets whose final view was released and releases
    // every block that no longer contains a constructed payload.
    void drain_retirements() {
        std::vector<TargetId> retired;
        {
            std::lock_guard lock{retirement_mutex_};
            retired.swap(retired_targets_);
        }

        for (const TargetId target : retired) {
            retire_target(target);
        }
        release_empty_blocks();
    }

    // Writer-only structural shrink after a history drop.
    void release_empty_blocks() {
        for (std::uint64_t block_index = 0;
             block_index < owned_blocks_.size();
             ++block_index) {
            const auto& block = owned_blocks_[block_index];
            if (block && block->is_empty()) {
                release_block(block_index);
            }
        }
        trim_owned_tail();
        directory_.clear(
            static_cast<std::uint64_t>(owned_blocks_.size()));
    }

    [[nodiscard]] std::size_t allocated_block_count() const noexcept {
        std::size_t count = 0;
        for (const auto& block : owned_blocks_) {
            count += block ? 1 : 0;
        }
        return count;
    }

    [[nodiscard]] std::size_t live_slot_count() const noexcept {
        std::size_t count = 0;
        for (const auto& block : owned_blocks_) {
            if (block) {
                count += block->header.live_versions;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t directory_page_count() const noexcept {
        return directory_.page_count();
    }

private:
    [[nodiscard]] VersionBlock<T>& require_block(
        TargetId target) const noexcept {
        auto block = directory_.find(Layout::block_index(target));
        assert(block);
        return block->get();
    }

    [[nodiscard]] std::uint64_t acquire_block_with_space() {
        while (!available_blocks_.empty()) {
            const std::uint64_t block_index = available_blocks_.back();
            if (block_index < owned_blocks_.size() &&
                owned_blocks_[block_index] &&
                !owned_blocks_[block_index]->is_full()) {
                return block_index;
            }
            available_blocks_.pop_back();
        }

        std::uint64_t block_index = 0;
        if (!free_blocks_.empty()) {
            block_index = free_blocks_.back();
            free_blocks_.pop_back();
        } else {
            block_index = static_cast<std::uint64_t>(owned_blocks_.size());
        }

        if (block_index > Layout::max_block_index) {
            throw std::length_error(
                "version store exhausted the target address space");
        }

        auto block = std::make_unique<VersionBlock<T>>();
        VersionBlock<T>& block_ref = *block;
        directory_.publish(block_index, block_ref);

        if (block_index == owned_blocks_.size()) {
            owned_blocks_.push_back(std::move(block));
        } else {
            owned_blocks_[block_index] = std::move(block);
        }
        available_blocks_.push_back(block_index);
        return block_index;
    }

    void remove_available_block(std::uint64_t block_index) noexcept {
        const auto it =
            std::find(available_blocks_.begin(),
                      available_blocks_.end(),
                      block_index);
        if (it != available_blocks_.end()) {
            *it = available_blocks_.back();
            available_blocks_.pop_back();
        }
    }

    void add_available_block(std::uint64_t block_index) {
        if (std::find(available_blocks_.begin(),
                      available_blocks_.end(),
                      block_index) == available_blocks_.end()) {
            available_blocks_.push_back(block_index);
        }
    }

    void retire_target(TargetId target) {
        if (!Layout::is_live_target(target)) {
            return;
        }

        const std::uint64_t block_index = Layout::block_index(target);
        if (block_index >= owned_blocks_.size() ||
            !owned_blocks_[block_index]) {
            return;
        }

        VersionBlock<T>& block = *owned_blocks_[block_index];
        const std::size_t slot = Layout::slot_index(target);
        if (block.views[slot].load(std::memory_order_acquire) != 0) {
            return;
        }
        if (block.slot_is_free(slot)) {
            return;
        }

        const bool was_full = block.is_full();
        std::destroy_at(block.payload_at(slot));
        block.views[slot].store(0, std::memory_order_relaxed);
        block.return_free_slot(slot);
        if (was_full) {
            add_available_block(block_index);
        }
    }

    void release_block(std::uint64_t block_index) {
        assert(block_index < owned_blocks_.size());
        assert(owned_blocks_[block_index]);
        assert(owned_blocks_[block_index]->is_empty());

        remove_available_block(block_index);
        directory_.clear(block_index);
        owned_blocks_[block_index].reset();
        free_blocks_.push_back(block_index);
    }

    void trim_owned_tail() {
        while (!owned_blocks_.empty() && !owned_blocks_.back()) {
            const std::uint64_t block_index =
                static_cast<std::uint64_t>(owned_blocks_.size() - 1);
            free_blocks_.erase(
                std::remove(
                    free_blocks_.begin(), free_blocks_.end(), block_index),
                free_blocks_.end());
            owned_blocks_.pop_back();
        }
    }

    void destroy_all_payloads() noexcept {
        for (auto& block : owned_blocks_) {
            if (!block) {
                continue;
            }

            for (std::size_t slot = 0;
                 slot < Layout::capacity;
                 ++slot) {
                if (!block->slot_is_free(slot)) {
                    std::destroy_at(block->payload_at(slot));
                }
            }
            block.reset();
        }
        owned_blocks_.clear();
        available_blocks_.clear();
        free_blocks_.clear();

        std::lock_guard lock{retirement_mutex_};
        retired_targets_.clear();
    }

    BlockDirectory<T> directory_;
    std::deque<std::unique_ptr<VersionBlock<T>>> owned_blocks_;

    // Writer-only allocation state.
    std::vector<std::uint64_t> available_blocks_;
    std::vector<std::uint64_t> free_blocks_;

    // The only reader-to-writer handoff.
    mutable std::mutex retirement_mutex_;
    std::vector<TargetId> retired_targets_;
};

}  // namespace mvcc_hive::detail
