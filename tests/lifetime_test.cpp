#include <mvcc_hive/mvcc_hive.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "lifetime_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

struct alignas(64) AlignedValue {
    static inline int alive = 0;

    explicit AlignedValue(int initial)
        : number(initial),
          address(this) {
        ++alive;
    }

    AlignedValue(const AlignedValue&) = delete;
    AlignedValue& operator=(const AlignedValue&) = delete;
    AlignedValue(AlignedValue&&) = delete;
    AlignedValue& operator=(AlignedValue&&) = delete;

    ~AlignedValue() {
        --alive;
    }

    int number;
    const void* address;
};

struct AddressTrap {
    explicit AddressTrap(int initial)
        : number(initial) {}

    AddressTrap(const AddressTrap&) = delete;
    AddressTrap& operator=(const AddressTrap&) = delete;
    AddressTrap(AddressTrap&&) = delete;
    AddressTrap& operator=(AddressTrap&&) = delete;

    AddressTrap* operator&() = delete;
    const AddressTrap* operator&() const = delete;

    int number;
};

void check_aligned_payload_is_stable() {
    AlignedValue::alive = 0;
    mvcc_hive::MVCCHive<AlignedValue> hive;

    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(1);
    auto first = tx.commit();
    const auto* first_address = std::addressof(first.at(id));
    REQUIRE(reinterpret_cast<std::uintptr_t>(first_address) % 64 == 0);
    REQUIRE(first_address->number == 1);
    REQUIRE(first_address->address == first_address);

    auto replacement = hive.begin_transaction();
    replacement.replace(id, 2);
    auto second = replacement.commit();
    const auto* second_address = std::addressof(second.at(id));
    REQUIRE(second_address != first_address);
    REQUIRE(reinterpret_cast<std::uintptr_t>(second_address) % 64 == 0);
    REQUIRE(second_address->number == 2);

    hive.prune_before(first.commit_id());
    REQUIRE(std::addressof(first.at(id)) == first_address);
    REQUIRE(first.at(id).number == 1);
    REQUIRE(AlignedValue::alive == 2);
}

void check_overloaded_address_operator() {
    mvcc_hive::MVCCHive<AddressTrap> hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(3);
    auto snapshot = tx.commit();

    const AddressTrap* value = std::addressof(snapshot.at(id));
    REQUIRE(value->number == 3);

    auto replacement = hive.begin_transaction();
    replacement.replace(id, 4);
    auto updated = replacement.commit();
    REQUIRE(std::addressof(updated.at(id))->number == 4);
    REQUIRE(std::addressof(snapshot.at(id)) == value);
    REQUIRE(std::addressof(snapshot.at(id))->number == 3);
}

}  // namespace

int main() {
    static_assert(alignof(AlignedValue) == 64);
    static_assert(std::atomic<mvcc_hive::detail::RowNodeId>::
                      is_always_lock_free);

    check_aligned_payload_is_stable();
    check_overloaded_address_operator();
}
