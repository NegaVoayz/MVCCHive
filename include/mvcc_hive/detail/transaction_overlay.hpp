#pragma once

#include <mvcc_hive/detail/layout.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mvcc_hive::detail {

class TransactionOverlay {
public:
    TransactionOverlay() {
        rehash(16);
    }

    [[nodiscard]] bool find(
        VirtualId id, TargetId& target) const noexcept {
        std::size_t index = hash(id) & (slots_.size() - 1);
        while (slots_[index].occupied) {
            if (slots_[index].key == id) {
                target = slots_[index].target;
                return true;
            }
            index = (index + 1) & (slots_.size() - 1);
        }
        return false;
    }

    void prepare_insert(std::size_t additional) {
        const std::size_t required = size_ + additional;
        const std::size_t limit = (slots_.size() * 7) / 10;
        if (required > limit) {
            std::size_t capacity = slots_.size() * 2;
            while (required > (capacity * 7) / 10) {
                capacity *= 2;
            }
            rehash(capacity);
        }
        touched_.reserve(touched_.size() + additional);
    }

    void set(VirtualId id, TargetId target) noexcept {
        std::size_t index = hash(id) & (slots_.size() - 1);
        while (slots_[index].occupied) {
            if (slots_[index].key == id) {
                slots_[index].target = target;
                return;
            }
            index = (index + 1) & (slots_.size() - 1);
        }

        assert(touched_.capacity() > touched_.size());
        slots_[index] = Slot{
            .key = id,
            .target = target,
            .occupied = true,
        };
        touched_.push_back(id);
        ++size_;
    }

    [[nodiscard]] const std::vector<VirtualId>& touched()
        const noexcept {
        return touched_;
    }

private:
    struct Slot {
        VirtualId key = 0;
        TargetId target = no_target;
        bool occupied = false;
    };

    [[nodiscard]] static std::uint64_t hash(
        VirtualId value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    void rehash(std::size_t capacity) {
        std::vector<Slot> replacement(capacity);
        for (const Slot& slot : slots_) {
            if (!slot.occupied) {
                continue;
            }
            std::size_t index =
                hash(slot.key) & (replacement.size() - 1);
            while (replacement[index].occupied) {
                index = (index + 1) & (replacement.size() - 1);
            }
            replacement[index] = slot;
        }
        slots_.swap(replacement);
    }

    std::vector<Slot> slots_;
    std::vector<VirtualId> touched_;
    std::size_t size_ = 0;
};

// Multi-writer seam. The first implementation serializes writers with the
// hive's writer mutex and deliberately uses a no-op lock set.
class RowLockSet {};

}  // namespace mvcc_hive::detail
