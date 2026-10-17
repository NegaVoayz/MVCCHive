#include <mvcc_hive/detail/commit_builder.hpp>
#include <mvcc_hive/detail/version_store.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using mvcc_hive::Diff;
using mvcc_hive::VersionToken;
using mvcc_hive::detail::CommitBuilder;
using mvcc_hive::detail::CommitId;
using mvcc_hive::detail::CommitStore;
using mvcc_hive::detail::CurrentTargetCache;
using mvcc_hive::detail::RowIndex;
using mvcc_hive::detail::RowNodeId;
using mvcc_hive::detail::TargetId;
using mvcc_hive::detail::VersionStore;
using mvcc_hive::detail::VersionTokenAccess;
using mvcc_hive::detail::VirtualId;
using mvcc_hive::detail::no_row_node;
using mvcc_hive::detail::no_target;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "history_test.cpp:" << line
                  << ": requirement failed: " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

VersionToken token(TargetId target) {
    return VersionTokenAccess::from_target(target);
}

Diff diff(
    CommitId commit,
    VirtualId id,
    TargetId previous,
    TargetId current) {
    return Diff{
        .commit_id = commit,
        .virtual_id = id,
        .previous_target = token(previous),
        .current_target = token(current),
    };
}

struct HistoryFixture {
    VersionStore<int> store;
    RowIndex rows;
    CommitStore commits;
    CurrentTargetCache current;

    TargetId value(int initial) {
        return store.emplace(1, initial);
    }

    void commit(
        CommitId id,
        CommitId parent,
        std::vector<Diff> diffs) {
        CommitBuilder builder{
            id, parent, rows, commits, current, std::move(diffs)};
        builder.prepare();
        builder.publish();
        REQUIRE(builder.published());
    }
};

void check_row_index_chain() {
    RowIndex rows;
    rows.ensure_rows(2);
    REQUIRE(rows.row_count() == 2);
    REQUIRE(rows.newest(0) == no_row_node);
    REQUIRE(rows.visible_target(0, 1) == no_target);

    const RowNodeId first = rows.allocate_node(
        {.commit_id = 1, .target = 17, .previous = no_row_node});
    rows.set_newest(0, first);
    REQUIRE(rows.live_node_count() == 1);
    REQUIRE(rows.newest(0) == first);
    REQUIRE(rows.node_at(first)->get().target == 17);
    REQUIRE(rows.visible_target(0, 0) == no_target);
    REQUIRE(rows.visible_target(0, 1) == 17);

    const RowNodeId second = rows.allocate_node(
        {.commit_id = 3, .target = 23, .previous = first});
    rows.set_newest(0, second);
    REQUIRE(rows.visible_target(0, 1) == 17);
    REQUIRE(rows.visible_target(0, 2) == 17);
    REQUIRE(rows.visible_target(0, 3) == 23);

    rows.release_node(second);
    rows.release_node(first);
    REQUIRE(rows.live_node_count() == 0);
}

void check_commit_publication_and_historical_reads() {
    HistoryFixture fixture;
    const TargetId first = fixture.value(10);
    const TargetId second = fixture.value(20);
    const TargetId third = fixture.value(30);

    fixture.commit(1, 0, {diff(1, 0, no_target, first)});
    REQUIRE(fixture.rows.live_node_count() == 1);
    REQUIRE(fixture.commits.commit_count() == 1);
    REQUIRE(fixture.commits.diff_count() == 1);
    REQUIRE(fixture.commits.head_commit() == 1);
    REQUIRE(fixture.current.get(0) == first);
    REQUIRE(fixture.rows.visible_target(0, 1) == first);

    fixture.commit(2, 1, {diff(2, 0, first, second)});
    REQUIRE(fixture.rows.live_node_count() == 2);
    REQUIRE(fixture.current.get(0) == second);
    REQUIRE(fixture.rows.visible_target(0, 1) == first);
    REQUIRE(fixture.rows.visible_target(0, 2) == second);

    fixture.commit(3, 2, {
        diff(3, 0, second, third),
        diff(3, 1, no_target, first),
    });
    REQUIRE(fixture.rows.live_node_count() == 4);
    REQUIRE(fixture.current.get(0) == third);
    REQUIRE(fixture.current.get(1) == first);
    REQUIRE(fixture.rows.visible_target(0, 2) == second);
    REQUIRE(fixture.rows.visible_target(1, 2) == no_target);
    REQUIRE(fixture.rows.visible_target(1, 3) == first);
    REQUIRE(fixture.commits.commit(2).id == 3);
    REQUIRE(fixture.commits.commit(2).parent == 2);
    REQUIRE(fixture.commits.commit(2).diff_count == 2);
}

void check_tombstone_visibility() {
    HistoryFixture fixture;
    const TargetId value = fixture.value(7);

    fixture.commit(1, 0, {diff(1, 4, no_target, value)});
    fixture.commit(2, 1, {diff(2, 4, value, no_target)});

    REQUIRE(fixture.rows.visible_target(4, 1) == value);
    REQUIRE(fixture.rows.visible_target(4, 2) == no_target);
    REQUIRE(fixture.current.get(4) == no_target);
}

void check_prepare_failure_rolls_back_nodes() {
    HistoryFixture fixture;
    const TargetId first = fixture.value(1);
    const TargetId second = fixture.value(2);
    fixture.commit(1, 0, {
        diff(1, 0, no_target, first),
        diff(1, 1, no_target, second),
    });

    const RowNodeId first_head = fixture.rows.newest(0);
    const RowNodeId second_head = fixture.rows.newest(1);
    const std::size_t nodes_before = fixture.rows.live_node_count();
    const std::size_t commits_before = fixture.commits.commit_count();
    const std::size_t diffs_before = fixture.commits.diff_count();

    bool threw = false;
    try {
        CommitBuilder broken{
            2,
            1,
            fixture.rows,
            fixture.commits,
            fixture.current,
            {
                diff(2, 0, first, second),
                diff(2, 1, first, second),
            },
        };
        broken.prepare();
    } catch (const std::logic_error&) {
        threw = true;
    }

    REQUIRE(threw);
    REQUIRE(fixture.rows.newest(0) == first_head);
    REQUIRE(fixture.rows.newest(1) == second_head);
    REQUIRE(fixture.rows.live_node_count() == nodes_before);
    REQUIRE(fixture.commits.commit_count() == commits_before);
    REQUIRE(fixture.commits.diff_count() == diffs_before);
    REQUIRE(fixture.commits.head_commit() == 1);
}

void check_current_cache_rebuild() {
    HistoryFixture fixture;
    const TargetId first = fixture.value(1);
    const TargetId second = fixture.value(2);
    fixture.commit(1, 0, {diff(1, 0, no_target, first)});
    fixture.commit(2, 1, {diff(2, 0, first, second)});

    fixture.current.set(0, first);
    REQUIRE(fixture.current.get(0) == first);
    fixture.current.rebuild(fixture.rows, fixture.commits.head_commit());
    REQUIRE(fixture.current.size() == 1);
    REQUIRE(fixture.current.get(0) == second);
}

}  // namespace

int main() {
    check_row_index_chain();
    check_commit_publication_and_historical_reads();
    check_tombstone_visibility();
    check_prepare_failure_rolls_back_nodes();
    check_current_cache_rebuild();
}
