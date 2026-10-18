#include <mvcc_hive/mvcc_hive.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using Hive = mvcc_hive::MVCCHive<int>;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "undo_redo_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

void check_undo_redo_round_trip() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(1);
    auto first = tx.commit();
    REQUIRE(first.commit_id() == 1);

    auto erase_tx = hive.begin_transaction();
    erase_tx.erase(id);
    auto removed = erase_tx.commit();
    REQUIRE(removed.commit_id() == 2);
    REQUIRE(!hive.head().contains(id));

    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 1);
    REQUIRE(hive.head().at(id) == 1);

    auto future = hive.snapshot(2);
    REQUIRE(future.has_value());
    REQUIRE(future->valid());
    REQUIRE(!future->contains(id));

    bool rejected = false;
    try {
        auto historical = hive.begin_transaction(2);
        static_cast<void>(historical);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    REQUIRE(rejected);

    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 2);
    REQUIRE(!hive.head().contains(id));
    REQUIRE(!hive.redo());
}

void check_new_commit_discards_future() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(10);
    auto first = tx.commit();
    REQUIRE(first.commit_id() == 1);

    auto erase_tx = hive.begin_transaction();
    erase_tx.erase(id);
    static_cast<void>(erase_tx.commit());

    REQUIRE(hive.undo());
    auto replace_tx = hive.begin_transaction();
    replace_tx.replace(id, 20);
    auto replaced = replace_tx.commit();

    REQUIRE(replaced.commit_id() == 2);
    REQUIRE(replaced.at(id) == 20);
    REQUIRE(hive.head().commit_id() == 2);
    REQUIRE(!hive.redo());

    const auto diffs = hive.diffs(2);
    REQUIRE(diffs.size() == 1);
    REQUIRE(diffs[0].kind == Hive::DiffKind::update);
    REQUIRE(!hive.diff(2).has_value());
    REQUIRE(!hive.snapshot(3).has_value());
}

void check_future_snapshot_blocks_discard() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(10);
    static_cast<void>(tx.commit());

    auto erase_tx = hive.begin_transaction();
    erase_tx.erase(id);
    static_cast<void>(erase_tx.commit());

    REQUIRE(hive.undo());
    auto future = hive.snapshot(2);
    REQUIRE(future.has_value());

    auto blocked_tx = hive.begin_transaction();
    blocked_tx.replace(id, 30);
    bool threw = false;
    try {
        static_cast<void>(blocked_tx.commit());
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(hive.head().commit_id() == 1);
    REQUIRE(hive.head().at(id) == 10);
    REQUIRE(hive.snapshot(2).has_value());

    blocked_tx.rollback();
    future.reset();

    auto retry_tx = hive.begin_transaction();
    retry_tx.replace(id, 30);
    auto replaced = retry_tx.commit();
    REQUIRE(replaced.commit_id() == 2);
    REQUIRE(replaced.at(id) == 30);
}

void check_undo_to_zero_and_redo() {
    Hive hive;
    REQUIRE(!hive.undo());
    REQUIRE(!hive.redo());

    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(7);
    static_cast<void>(tx.commit());

    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 0);
    REQUIRE(!hive.head().contains(id));

    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 1);
    REQUIRE(hive.head().at(id) == 7);
    REQUIRE(!hive.redo());
}

void check_empty_commit_preserves_redo() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(1);
    static_cast<void>(tx.commit());

    auto erase_tx = hive.begin_transaction();
    erase_tx.erase(id);
    static_cast<void>(erase_tx.commit());
    REQUIRE(hive.undo());

    auto empty_tx = hive.begin_transaction();
    auto unchanged = empty_tx.commit();
    REQUIRE(unchanged.commit_id() == 1);
    REQUIRE(hive.head().commit_id() == 1);
    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 2);
}

void check_old_snapshot_survives_discard() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(10);
    auto first = tx.commit();
    REQUIRE(first.commit_id() == 1);

    auto erase_tx = hive.begin_transaction();
    erase_tx.erase(id);
    static_cast<void>(erase_tx.commit());
    REQUIRE(hive.undo());

    auto old = hive.snapshot(1);
    REQUIRE(old.has_value());

    auto replace_tx = hive.begin_transaction();
    replace_tx.replace(id, 50);
    auto replaced = replace_tx.commit();
    REQUIRE(replaced.commit_id() == 2);
    REQUIRE(old->valid());
    REQUIRE(old->at(id) == 10);
    REQUIRE(hive.head().at(id) == 50);
}

void check_clear_history_uses_checkout() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(1);
    static_cast<void>(tx.commit());

    auto replace_tx = hive.begin_transaction();
    replace_tx.replace(id, 2);
    static_cast<void>(replace_tx.commit());

    auto second_replace = hive.begin_transaction();
    second_replace.replace(id, 3);
    static_cast<void>(second_replace.commit());

    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 2);

    bool threw = false;
    try {
        hive.prune_before(3);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    REQUIRE(threw);

    hive.clear_history();
    REQUIRE(hive.head().commit_id() == 2);
    REQUIRE(hive.head().at(id) == 2);
    REQUIRE(hive.snapshot(2).has_value());
    REQUIRE(!hive.snapshot(3).has_value());
    REQUIRE(!hive.redo());

    auto continuation = hive.begin_transaction();
    continuation.replace(id, 4);
    auto committed = continuation.commit();
    REQUIRE(committed.commit_id() == 3);
    REQUIRE(committed.at(id) == 4);
}

void check_multi_level_undo_redo() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(1);
    static_cast<void>(tx.commit());

    auto second = hive.begin_transaction();
    second.replace(id, 2);
    static_cast<void>(second.commit());

    auto third = hive.begin_transaction();
    third.replace(id, 3);
    static_cast<void>(third.commit());

    REQUIRE(hive.undo());
    REQUIRE(hive.head().at(id) == 2);
    REQUIRE(hive.undo());
    REQUIRE(hive.head().at(id) == 1);
    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 0);
    REQUIRE(!hive.head().contains(id));

    REQUIRE(hive.redo());
    REQUIRE(hive.head().at(id) == 1);
    REQUIRE(hive.redo());
    REQUIRE(hive.head().at(id) == 2);
    REQUIRE(hive.redo());
    REQUIRE(hive.head().at(id) == 3);
    REQUIRE(!hive.redo());
}

void check_undo_floor_errors() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(1);
    static_cast<void>(tx.commit());

    auto second = hive.begin_transaction();
    second.replace(id, 2);
    static_cast<void>(second.commit());

    auto third = hive.begin_transaction();
    third.replace(id, 3);
    static_cast<void>(third.commit());

    hive.prune_before(2);
    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 2);

    bool threw = false;
    try {
        static_cast<void>(hive.undo());
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(hive.head().commit_id() == 2);
    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 3);

    hive.clear_history();
    threw = false;
    try {
        static_cast<void>(hive.undo());
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(hive.head().commit_id() == 3);
    REQUIRE(!hive.redo());
}

void check_first_commit_is_not_an_undo_floor() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto id = tx.emplace(1);
    static_cast<void>(tx.commit());

    hive.clear_history();
    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 0);
    REQUIRE(!hive.head().contains(id));
    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 1);
    REQUIRE(hive.head().at(id) == 1);
}

}  // namespace

int main() {
    check_undo_redo_round_trip();
    check_new_commit_discards_future();
    check_future_snapshot_blocks_discard();
    check_undo_to_zero_and_redo();
    check_empty_commit_preserves_redo();
    check_old_snapshot_survives_discard();
    check_clear_history_uses_checkout();
    check_multi_level_undo_redo();
    check_undo_floor_errors();
    check_first_commit_is_not_an_undo_floor();
}
