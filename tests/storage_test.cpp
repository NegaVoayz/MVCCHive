#include <mvcc_hive/detail/version_block.hpp>
#include <mvcc_hive/detail/version_store.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

using mvcc_hive::detail::BlockLayout;
using mvcc_hive::detail::TargetId;
using mvcc_hive::detail::VersionBlock;
using mvcc_hive::detail::VersionStore;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "storage_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

struct CountingValue {
    static inline int alive = 0;

    explicit CountingValue(int initial)
        : value(initial), address(this) {
        ++alive;
    }

    CountingValue(const CountingValue&) = delete;
    CountingValue& operator=(const CountingValue&) = delete;
    CountingValue(CountingValue&&) = delete;
    CountingValue& operator=(CountingValue&&) = delete;

    ~CountingValue() {
        --alive;
    }

    int value;
    const void* address;
};

struct FourSlotValue {
    std::byte bytes[4096]{};
};

void check_block_allocation_is_payload_free() {
    REQUIRE(CountingValue::alive == 0);
    {
        auto block = std::make_unique<VersionBlock<CountingValue>>();
        REQUIRE(CountingValue::alive == 0);
        REQUIRE(block->header.live_versions == 0);
        REQUIRE(block->is_empty());
        REQUIRE(block->slot_is_free(0));
        REQUIRE(block->views[0].load() == 0);
    }
    REQUIRE(CountingValue::alive == 0);
}

void check_construct_resolve_and_release() {
    static_assert(
        BlockLayout<CountingValue>::capacity
        == 1024);

    {
        VersionStore<CountingValue> store;
        REQUIRE(store.allocated_block_count() == 0);

        const TargetId first = store.emplace(1, 41);
        const TargetId second = store.emplace(1, 42);
        REQUIRE(CountingValue::alive == 2);
        REQUIRE(store.live_slot_count() == 2);
        REQUIRE(store.allocated_block_count() == 1);

        auto first_value = store.resolve(first);
        auto second_value = store.resolve(second);
        REQUIRE(first_value.has_value());
        REQUIRE(second_value.has_value());
        REQUIRE(first_value->get().value == 41);
        REQUIRE(second_value->get().value == 42);

        const void* first_address = &first_value->get();
        const TargetId third = store.emplace(1, 43);
        REQUIRE(third != first);
        REQUIRE(&store.resolve(first)->get() == first_address);
        REQUIRE(store.resolve(first)->get().address ==
                first_address);

        store.release_view(first);
        store.release_view(second);
        store.drain_retirements();
        REQUIRE(CountingValue::alive == 1);
        REQUIRE(store.live_slot_count() == 1);
        REQUIRE(!store.resolve(first).has_value());
        REQUIRE(!store.resolve(second).has_value());
    }
    REQUIRE(CountingValue::alive == 0);
}

void check_block_release_and_reuse() {
    static_assert(
        BlockLayout<FourSlotValue>::capacity == 4);

    VersionStore<FourSlotValue> store;
    TargetId targets[4]{
        store.emplace(1, FourSlotValue{}),
        store.emplace(1, FourSlotValue{}),
        store.emplace(1, FourSlotValue{}),
        store.emplace(1, FourSlotValue{}),
    };

    REQUIRE(store.allocated_block_count() == 1);
    REQUIRE(store.live_slot_count() == 4);

    for (const TargetId target : targets) {
        store.release_view(target);
    }
    store.drain_retirements();
    REQUIRE(store.live_slot_count() == 0);
    REQUIRE(store.allocated_block_count() == 0);
    REQUIRE(store.directory_page_count() == 0);

    const TargetId reused = store.emplace(1, FourSlotValue{});
    REQUIRE(reused == 0);
    REQUIRE(store.allocated_block_count() == 1);
    store.release_view(reused);
    store.drain_retirements();
    REQUIRE(store.allocated_block_count() == 0);
}

void check_teardown_destroys_live_payloads() {
    REQUIRE(CountingValue::alive == 0);
    {
        VersionStore<CountingValue> store;
        static_cast<void>(store.emplace(1, 1));
        static_cast<void>(store.emplace(1, 2));
        static_cast<void>(store.emplace(1, 3));
        REQUIRE(CountingValue::alive == 3);
    }
    REQUIRE(CountingValue::alive == 0);
}

void check_constructor_failure_returns_slot() {
    struct Throws {
        Throws() {
            throw 7;
        }
    };

    VersionStore<Throws> store;
    bool threw = false;
    try {
        static_cast<void>(store.emplace(1));
    } catch (int value) {
        threw = value == 7;
    }

    REQUIRE(threw);
    REQUIRE(store.live_slot_count() == 0);
    REQUIRE(store.allocated_block_count() == 1);
    store.release_empty_blocks();
    REQUIRE(store.allocated_block_count() == 0);
}

}  // namespace

int main() {
    check_block_allocation_is_payload_free();
    check_construct_resolve_and_release();
    check_block_release_and_reuse();
    check_teardown_destroys_live_payloads();
    check_constructor_failure_returns_slot();
}
