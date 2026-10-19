#include <mvcc_hive/mvcc_hive.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using Hive = mvcc_hive::MVCCHive<int>;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "commit_id_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

void check_explicit_ids_align_hives() {
    Hive alpha;
    Hive beta;

    auto alpha_tx = alpha.begin_transaction();
    const auto alpha_id = alpha_tx.emplace(42);
    const auto alpha_first = alpha_tx.commit(100);
    REQUIRE(alpha_first.commit_id() == 100);

    auto beta_tx = beta.begin_transaction();
    const auto beta_first = beta_tx.commit(100);
    REQUIRE(beta_first.commit_id() == 100);

    REQUIRE(alpha.head().commit_id() == 100);
    REQUIRE(beta.head().commit_id() == 100);
    REQUIRE(alpha.max_commit_id() == 100);
    REQUIRE(beta.max_commit_id() == 100);
    REQUIRE(alpha.head().contains(alpha_id));
    REQUIRE(alpha.diffs(100).size() == 1);
    REQUIRE(beta.diffs(100).empty());

    auto alpha_next = alpha.begin_transaction();
    alpha_next.replace(alpha_id, 43);
    REQUIRE(alpha_next.commit(250).commit_id() == 250);

    auto beta_next = beta.begin_transaction();
    const auto beta_id = beta_next.emplace(7);
    REQUIRE(beta_next.commit(250).commit_id() == 250);

    REQUIRE(alpha.head().commit_id() == 250);
    REQUIRE(beta.head().commit_id() == 250);

    REQUIRE(alpha.undo());
    REQUIRE(beta.undo());
    REQUIRE(alpha.head().commit_id() == 100);
    REQUIRE(beta.head().commit_id() == 100);
    REQUIRE(alpha.head().at(alpha_id) == 42);
    REQUIRE(!beta.head().contains(beta_id));

    REQUIRE(alpha.redo());
    REQUIRE(beta.redo());
    REQUIRE(alpha.head().commit_id() == 250);
    REQUIRE(beta.head().commit_id() == 250);
    REQUIRE(alpha.head().at(alpha_id) == 43);
    REQUIRE(beta.head().at(beta_id) == 7);
}

void check_explicit_empty_commit_advances_checkout() {
    Hive hive;
    auto tx = hive.begin_transaction();
    const auto committed = tx.commit(7);

    REQUIRE(committed.commit_id() == 7);
    REQUIRE(hive.head().commit_id() == 7);
    REQUIRE(hive.diffs(7).empty());

    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 0);
    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 7);
}

void check_invalid_explicit_id_preserves_transaction() {
    Hive hive;
    auto first = hive.begin_transaction();
    const auto id = first.emplace(1);
    REQUIRE(first.commit(50).commit_id() == 50);

    auto waiting = hive.begin_transaction();
    waiting.replace(id, 2);

    for (const Hive::CommitId invalid : {Hive::CommitId{0},
                                         Hive::CommitId{49},
                                         Hive::CommitId{50}}) {
        bool threw = false;
        try {
            static_cast<void>(waiting.commit(invalid));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        REQUIRE(threw);
        REQUIRE(hive.head().commit_id() == 50);
        REQUIRE(waiting.at(id) == 2);
    }

    REQUIRE(waiting.commit(51).commit_id() == 51);
    REQUIRE(hive.head().at(id) == 2);
}

void check_sparse_history_undo_redo_and_pruning() {
    Hive hive;

    auto first = hive.begin_transaction();
    const auto id = first.emplace(1);
    REQUIRE(first.commit(10).commit_id() == 10);

    auto second = hive.begin_transaction();
    second.replace(id, 2);
    REQUIRE(second.commit(30).commit_id() == 30);

    auto third = hive.begin_transaction();
    third.replace(id, 3);
    REQUIRE(third.commit(40).commit_id() == 40);

    REQUIRE(!hive.snapshot(20).has_value());
    REQUIRE(hive.snapshot(10).has_value());
    REQUIRE(hive.snapshot(30).has_value());
    REQUIRE(hive.snapshot(40).has_value());

    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 30);
    REQUIRE(hive.head().at(id) == 2);
    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 10);
    REQUIRE(hive.head().at(id) == 1);
    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 0);

    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 10);
    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 30);
    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 40);

    hive.prune_before(30);
    REQUIRE(!hive.snapshot(10).has_value());
    REQUIRE(hive.undo());
    REQUIRE(hive.head().commit_id() == 30);

    bool threw = false;
    try {
        static_cast<void>(hive.undo());
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(hive.head().commit_id() == 30);
    REQUIRE(hive.redo());
    REQUIRE(hive.head().commit_id() == 40);
}

void check_clear_history_keeps_id_high_water() {
    Hive hive;
    auto first = hive.begin_transaction();
    const auto id = first.emplace(1);
    REQUIRE(first.commit(100).commit_id() == 100);

    hive.clear_history();
    REQUIRE(hive.max_commit_id() == 100);

    auto next = hive.begin_transaction();
    next.replace(id, 2);

    bool threw = false;
    try {
        static_cast<void>(next.commit(100));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(next.at(id) == 2);

    REQUIRE(next.commit(101).commit_id() == 101);
    REQUIRE(hive.head().commit_id() == 101);
    REQUIRE(hive.head().at(id) == 2);
}

}  // namespace

int main() {
    check_explicit_ids_align_hives();
    check_explicit_empty_commit_advances_checkout();
    check_invalid_explicit_id_preserves_transaction();
    check_sparse_history_undo_redo_and_pruning();
    check_clear_history_keeps_id_high_water();
}
