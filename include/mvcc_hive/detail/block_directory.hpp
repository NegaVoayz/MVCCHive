#pragma once

#include <mvcc_hive/detail/version_block.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>

namespace mvcc_hive::detail {

template<class T>
class BlockDirectory {
public:
    static constexpr std::size_t page_capacity = 256;
    static_assert(std::has_single_bit(page_capacity));

    static constexpr unsigned page_shift =
        std::countr_zero(page_capacity);
    static constexpr std::size_t page_mask = page_capacity - 1;

    [[nodiscard]] std::optional<
        std::reference_wrapper<VersionBlock<T>>>
    find(std::uint64_t block_index) const noexcept {
        std::shared_lock lock{mutex_};
        const std::uint64_t page_index = block_index >> page_shift;
        if (page_index >= pages_.size()) {
            return std::nullopt;
        }

        const auto& page = pages_[static_cast<std::size_t>(page_index)];
        if (!page) {
            return std::nullopt;
        }

        return page->entries[block_index & page_mask];
    }

    void publish(
        std::uint64_t block_index, VersionBlock<T>& block) {
        std::unique_lock lock{mutex_};
        const std::uint64_t page_index = block_index >> page_shift;
        if (page_index > std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("block directory page index overflow");
        }

        const auto page_offset = static_cast<std::size_t>(page_index);
        if (page_offset >= pages_.size()) {
            pages_.resize(page_offset + 1);
        }
        if (!pages_[page_offset]) {
            pages_[page_offset] = std::make_unique<Page>();
        }

        auto& entry = pages_[page_offset]->entries[block_index & page_mask];
        if (entry) {
            throw std::logic_error("block directory entry already occupied");
        }
        entry = std::ref(block);
    }

    void clear(std::uint64_t block_index) noexcept {
        std::unique_lock lock{mutex_};
        const std::uint64_t page_index = block_index >> page_shift;
        if (page_index >= pages_.size()) {
            return;
        }

        auto& page = pages_[static_cast<std::size_t>(page_index)];
        if (page) {
            page->entries[block_index & page_mask].reset();
        }
        trim_trailing_pages();
    }

    [[nodiscard]] std::size_t page_count() const noexcept {
        std::shared_lock lock{mutex_};
        return pages_.size();
    }

private:
    struct Page {
        std::array<
            std::optional<std::reference_wrapper<VersionBlock<T>>>,
            page_capacity>
            entries{};
    };

    void trim_trailing_pages() noexcept {
        while (!pages_.empty()) {
            const auto& page = pages_.back();
            if (!page) {
                pages_.pop_back();
                continue;
            }

            bool empty = true;
            for (const auto& entry : page->entries) {
                if (entry) {
                    empty = false;
                    break;
                }
            }
            if (!empty) {
                break;
            }
            pages_.pop_back();
        }
        if (pages_.empty()) {
            std::deque<std::unique_ptr<Page>>{}.swap(pages_);
        }
    }

    mutable std::shared_mutex mutex_;
    std::deque<std::unique_ptr<Page>> pages_;
};

}  // namespace mvcc_hive::detail
