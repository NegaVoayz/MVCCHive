#include <mvcc_hive/mvcc_hive.hpp>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

using mvcc_hive::MVCCHive;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "transaction_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

struct Value {
    static inline int alive = 0;

    explicit Value(int initial)
        : number(initial), address(this) {
        ++alive;
    }

    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    Value(Value&&) = delete;
    Value& operator=(Value&&) = delete;

    ~Value() {
        --alive;
    }

    int number;
    const void* address;
};

void check_insert_read_and_history() {
    MVCCHive<Value> hive;
    auto empty = hive.head();
    REQUIRE(empty.valid());
    REQUIRE(empty.commit_id() == 0);
    REQUIRE(!empty.contains(0));

    MVCCHive<Value>::VirtualId id = 0;
    Value::alive = 0;
    {
        auto tx = hive.begin_transaction();
        id = tx.emplace(42);
        REQUIRE(id == 0);
        REQUIRE(tx.at(id).number == 42);
        REQUIRE(tx.find(id).has_value());
        REQUIRE(!hive.head().contains(id));

        auto committed = tx.commit();
        REQUIRE(committed.commit_id() == 1);
        REQUIRE(committed.contains(id));
        REQUIRE(committed.at(id).number == 42);
    }

    REQUIRE(Value::alive == 1);
    auto first = hive.snapshot(1);
    REQUIRE(first.has_value());
    REQUIRE(first->valid());

    const auto diffs = hive.diffs(1);
    REQUIRE(diffs.size() == 1);
    REQUIRE(diffs[0].commit_id == 1);
    REQUIRE(diffs[0].virtual_id == id);
    REQUIRE(!diffs[0].previous_target.has_value());
    REQUIRE(diffs[0].current_target.has_value());

    const auto first_diff = hive.diff(0);
    REQUIRE(first_diff.has_value());
    REQUIRE(first_diff->current_target ==
            diffs[0].current_target);
    REQUIRE(!hive.diff(1).has_value());
}

void check_replace_preserves_old_payload() {
    MVCCHive<Value> hive;
    Value::alive = 0;

    auto tx1 = hive.begin_transaction();
    const auto id = tx1.emplace(1);
    auto first = tx1.commit();
    const Value& old_value = first.at(id);
    const void* old_address = &old_value;
    REQUIRE(old_value.number == 1);

    auto tx2 = hive.begin_transaction();
    tx2.replace(id, 2);
    REQUIRE(tx2.at(id).number == 2);
    REQUIRE(&first.at(id) == old_address);
    REQUIRE(first.at(id).number == 1);

    const Value& staged_before_second_write = tx2.at(id);
    tx2.replace(id, 3);
    REQUIRE(staged_before_second_write.number == 2);
    REQUIRE(tx2.at(id).number == 3);

    auto second = tx2.commit();
    REQUIRE(second.commit_id() == 2);
    REQUIRE(second.at(id).number == 3);
    REQUIRE(first.at(id).number == 1);
    REQUIRE(&first.at(id) == old_address);
    REQUIRE(&second.at(id) != old_address);
}

void check_erase_and_swap() {
    MVCCHive<Value> hive;
    Value::alive = 0;

    auto tx1 = hive.begin_transaction();
    const auto first = tx1.emplace(10);
    const auto second = tx1.emplace(20);
    auto initial = tx1.commit();
    const void* first_address = &initial.at(first);
    const void* second_address = &initial.at(second);
    REQUIRE(Value::alive == 2);

    auto tx2 = hive.begin_transaction();
    tx2.swap(first, second);
    REQUIRE(Value::alive == 2);
    REQUIRE(tx2.at(first).number == 20);
    REQUIRE(tx2.at(second).number == 10);
    auto swapped = tx2.commit();
    REQUIRE(swapped.at(first).number == 20);
    REQUIRE(swapped.at(second).number == 10);
    REQUIRE(&swapped.at(first) == second_address);
    REQUIRE(&swapped.at(second) == first_address);
    REQUIRE(initial.at(first).number == 10);
    REQUIRE(initial.at(second).number == 20);

    auto tx3 = hive.begin_transaction();
    tx3.erase(first);
    REQUIRE(!tx3.find(first).has_value());
    bool threw = false;
    try {
        static_cast<void>(tx3.at(first));
    } catch (const std::out_of_range&) {
        threw = true;
    }
    REQUIRE(threw);

    auto erased = tx3.commit();
    REQUIRE(!erased.contains(first));
    REQUIRE(swapped.at(first).number == 20);
    REQUIRE(erased.at(second).number == 10);
}

void check_rollback_destroys_only_staged_versions() {
    MVCCHive<Value> hive;
    Value::alive = 0;

    {
        auto tx = hive.begin_transaction();
        const auto id = tx.emplace(1);
        tx.replace(id, 2);
        REQUIRE(Value::alive == 2);
        tx.rollback();
        REQUIRE(Value::alive == 0);
    }
    REQUIRE(hive.head().commit_id() == 0);

    {
        auto tx = hive.begin_transaction();
        const auto id = tx.emplace(7);
        REQUIRE(Value::alive == 1);
        tx.erase(id);
        auto empty_commit = tx.commit();
        REQUIRE(empty_commit.commit_id() == 0);
        REQUIRE(Value::alive == 0);
    }
    REQUIRE(hive.head().commit_id() == 0);
}

void check_overlay_growth_and_enumeration() {
    MVCCHive<Value> hive;
    Value::alive = 0;

    constexpr std::size_t count = 1000;
    std::vector<MVCCHive<Value>::VirtualId> ids;
    ids.reserve(count);

    auto tx = hive.begin_transaction();
    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(
            tx.emplace(static_cast<int>(i)));
    }
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(tx.at(ids[i]).number == static_cast<int>(i));
    }
    REQUIRE(!tx.find(count).has_value());

    auto snapshot = tx.commit();
    REQUIRE(snapshot.commit_id() == 1);
    REQUIRE(Value::alive == static_cast<int>(count));

    std::size_t visited = 0;
    long long sum = 0;
    snapshot.for_each([&](auto, const Value& value) {
        ++visited;
        sum += value.number;
    });
    REQUIRE(visited == count);
    REQUIRE(sum == (count * (count - 1)) / 2);
}

void check_snapshot_lifetime_and_copy() {
    std::optional<MVCCHive<Value>::Snapshot> snapshot;
    Value::alive = 0;

    {
        MVCCHive<Value> hive;
        auto tx = hive.begin_transaction();
        const auto id = tx.emplace(9);
        auto committed = tx.commit();
        snapshot = committed;
        REQUIRE(snapshot->valid());

        auto copy = *snapshot;
        REQUIRE(copy.valid());
        REQUIRE(copy.at(id).number == 9);
    }

    REQUIRE(Value::alive == 0);
    REQUIRE(!snapshot->valid());
    REQUIRE(!snapshot->find(0).has_value());
}

void check_writable_transaction_requires_head() {
    MVCCHive<Value> hive;
    Value::alive = 0;
    auto tx = hive.begin_transaction();
    static_cast<void>(tx.emplace(1));
    static_cast<void>(tx.commit());

    bool threw = false;
    try {
        auto old = hive.begin_transaction(0);
        static_cast<void>(old);
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE(threw);
}

}  // namespace

int main() {
    static_assert(noexcept(
        std::declval<const MVCCHive<Value>::Transaction&>().find(0)));
    static_assert(std::is_same_v<
                  decltype(std::declval<
                               const MVCCHive<Value>::Transaction&>()
                               .at(0)),
                  const Value&>);

    check_insert_read_and_history();
    check_replace_preserves_old_payload();
    check_erase_and_swap();
    check_rollback_destroys_only_staged_versions();
    check_overlay_growth_and_enumeration();
    check_snapshot_lifetime_and_copy();
    check_writable_transaction_requires_head();
}
