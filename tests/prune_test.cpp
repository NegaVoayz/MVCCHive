#include <mvcc_hive/mvcc_hive.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {

using mvcc_hive::MVCCHive;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "prune_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

struct Value {
    static inline int alive = 0;

    explicit Value(int initial)
        : number(initial) {
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
};

MVCCHive<Value>::VirtualId insert(
    MVCCHive<Value>& hive, int number) {
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(number);
    static_cast<void>(tx.commit());
    return id;
}

void replace(
    MVCCHive<Value>& hive,
    MVCCHive<Value>::VirtualId id,
    int number) {
    auto tx = hive.begin_transaction();
    tx.replace(id, number);
    static_cast<void>(tx.commit());
}

void check_prune_and_history_clear() {
    Value::alive = 0;
    MVCCHive<Value> hive;

    const auto id = insert(hive, 1);
    replace(hive, id, 2);
    replace(hive, id, 3);
    replace(hive, id, 4);
    REQUIRE(Value::alive == 4);

    auto snapshot_two = hive.snapshot(2);
    REQUIRE(snapshot_two.has_value());
    REQUIRE(snapshot_two->at(id).number == 2);

    hive.prune_before(3);
    REQUIRE(Value::alive == 3);
    REQUIRE(snapshot_two->valid());
    REQUIRE(snapshot_two->at(id).number == 2);
    REQUIRE(!hive.snapshot(1).has_value());
    REQUIRE(!hive.snapshot(2).has_value());

    {
        auto retained = hive.snapshot(3);
        REQUIRE(retained.has_value());
        REQUIRE(retained->at(id).number == 3);
    }
    REQUIRE(hive.head().at(id).number == 4);
    REQUIRE(hive.diffs(3).size() == 1);
    REQUIRE(hive.diffs(1).empty());

    snapshot_two.reset();
    hive.clear_history();
    REQUIRE(Value::alive == 1);
    REQUIRE(hive.head().at(id).number == 4);
    REQUIRE(!hive.snapshot(3).has_value());

    replace(hive, id, 5);
    REQUIRE(Value::alive == 2);
    REQUIRE(hive.snapshot(4)->at(id).number == 4);
    REQUIRE(hive.head().at(id).number == 5);
    REQUIRE(hive.diffs(5).size() == 1);

    auto before_clear = hive.head();
    hive.clear();
    REQUIRE(!before_clear.valid());
    REQUIRE(Value::alive == 0);
    REQUIRE(hive.head().commit_id() == 0);
    REQUIRE(!hive.snapshot(4).has_value());
    REQUIRE(hive.diffs(4).empty());
}

void check_clear_history_keeps_active_snapshot() {
    Value::alive = 0;
    MVCCHive<Value> hive;

    const auto id = insert(hive, 10);
    replace(hive, id, 20);
    replace(hive, id, 30);
    REQUIRE(Value::alive == 3);

    auto old = hive.snapshot(1);
    REQUIRE(old.has_value());
    REQUIRE(old->at(id).number == 10);

    hive.clear_history();
    REQUIRE(old->valid());
    REQUIRE(old->at(id).number == 10);
    REQUIRE(Value::alive == 3);
    REQUIRE(!hive.snapshot(1).has_value());
    REQUIRE(hive.head().at(id).number == 30);

    old.reset();
    hive.clear_history();
    REQUIRE(Value::alive == 1);
    REQUIRE(hive.head().at(id).number == 30);
}

void check_zero_snapshot_does_not_drop_retained_history() {
    Value::alive = 0;
    MVCCHive<Value> hive;
    auto zero = hive.head();
    REQUIRE(zero.commit_id() == 0);

    const auto id = insert(hive, 10);
    replace(hive, id, 20);
    replace(hive, id, 30);
    REQUIRE(Value::alive == 3);

    hive.prune_before(2);
    REQUIRE(zero.valid());
    REQUIRE(!zero.contains(id));
    // Snapshot zero can still traverse every newer node's previous link.
    // The chain remains pinned until that snapshot is released.
    REQUIRE(Value::alive == 3);

    auto retained = hive.snapshot(2);
    REQUIRE(retained.has_value());
    REQUIRE(retained->at(id).number == 20);
    REQUIRE(hive.head().at(id).number == 30);
}

void check_prune_range_validation() {
    Value::alive = 0;
    MVCCHive<Value> hive;
    static_cast<void>(insert(hive, 1));

    bool threw = false;
    try {
        hive.prune_before(2);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(Value::alive == 1);
}

}  // namespace

int main() {
    check_prune_and_history_clear();
    check_clear_history_keeps_active_snapshot();
    check_zero_snapshot_does_not_drop_retained_history();
    check_prune_range_validation();
}
