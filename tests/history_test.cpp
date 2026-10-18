#include <mvcc_hive/detail/commit_builder.hpp>
#include <mvcc_hive/detail/version_store.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using mvcc_hive::detail::CommitBuilder;
using mvcc_hive::detail::CommitId;
using mvcc_hive::detail::CommitStore;
using mvcc_hive::detail::CurrentTargetCache;
using mvcc_hive::detail::DiffRecord;
using mvcc_hive::detail::RowIndex;
using mvcc_hive::detail::RowNodeLayout;
using mvcc_hive::detail::RowNodeId;
using mvcc_hive::detail::RowVersionNode;
using mvcc_hive::detail::TargetId;
using mvcc_hive::detail::VersionStore;
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

DiffRecord diff(
    CommitId commit,
    VirtualId id,
    TargetId previous,
    TargetId current) {
    return DiffRecord{
        .commit_id = commit,
        .virtual_id = id,
        .previous_target = previous,
        .current_target = current,
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
        std::vector<DiffRecord> diffs) {
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
    REQUIRE(fixture.commits.tip_commit() == 1);
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
    REQUIRE(fixture.commits.tip_commit() == 1);
}

void check_current_cache_rebuild() {
    HistoryFixture fixture;
    const TargetId first = fixture.value(1);
    const TargetId second = fixture.value(2);
    fixture.commit(1, 0, {diff(1, 0, no_target, first)});
    fixture.commit(2, 1, {diff(2, 0, first, second)});

    fixture.current.set(0, first);
    REQUIRE(fixture.current.get(0) == first);
    fixture.current.rebuild(fixture.rows, fixture.commits.tip_commit());
    REQUIRE(fixture.current.size() == 1);
    REQUIRE(fixture.current.get(0) == second);
}

void check_commit_log_uses_preallocated_blocks() {
    using Commits =
        mvcc_hive::detail::BlockedVector<
            mvcc_hive::detail::CommitRecord>;
    constexpr std::size_t block_capacity =
        Commits::block_capacity;

    CommitStore store;

    store.reserve(1, 1);
    const std::size_t initial_commit_capacity =
        store.commit_capacity();
    const std::size_t initial_diff_capacity =
        store.diff_capacity();
    REQUIRE(initial_commit_capacity == block_capacity);
    REQUIRE(initial_diff_capacity == block_capacity);

    const DiffRecord record = diff(1, 0, no_target, 0);
    for (std::size_t i = 1; i <= block_capacity; ++i) {
        store.reserve(1, 1);
        store.append_prepared(i, i - 1, std::span{&record, 1});
        store.publish_tip(i);
        REQUIRE(store.commit_capacity() == initial_commit_capacity);
        REQUIRE(store.diff_capacity() == initial_diff_capacity);
    }

    store.reserve(1, 1);
    REQUIRE(store.commit_capacity() == block_capacity * 2);
    REQUIRE(store.diff_capacity() == block_capacity * 2);
}

void check_truncate_keeps_room_for_replacement() {
    using Commits =
        mvcc_hive::detail::BlockedVector<
            mvcc_hive::detail::CommitRecord>;
    constexpr std::size_t block_capacity =
        Commits::block_capacity;

    CommitStore store;
    store.reserve(block_capacity + 2, block_capacity + 2);
    const DiffRecord record = diff(1, 0, no_target, 0);
    for (CommitId id = 1; id <= block_capacity + 2; ++id) {
        store.append_prepared(id, id - 1, std::span{&record, 1});
        store.publish_tip(id);
    }

    auto truncation = store.prepare_truncate_after(
        block_capacity, 1, 1);
    store.apply_truncate(std::move(truncation));
    REQUIRE(store.commit_count() == block_capacity);
    REQUIRE(store.diff_count() == block_capacity);

    store.append_prepared(
        block_capacity + 1,
        block_capacity,
        std::span{&record, 1});
    store.publish_tip(block_capacity + 1);
    REQUIRE(store.commit_count() == block_capacity + 1);
    REQUIRE(store.diff_count() == block_capacity + 1);
}

void check_row_pruning_keeps_snapshot_tail() {
    RowIndex rows;
    rows.ensure_rows(1);

    const RowNodeId first = rows.allocate_node(
        {.commit_id = 1, .target = 10, .previous = no_row_node});
    const RowNodeId second = rows.allocate_node(
        {.commit_id = 2, .target = 20, .previous = first});
    const RowNodeId third = rows.allocate_node(
        {.commit_id = 3, .target = 30, .previous = second});
    const RowNodeId fourth = rows.allocate_node(
        {.commit_id = 4, .target = 40, .previous = third});
    rows.set_newest(0, fourth);

    auto released = rows.prune_rows(2);
    REQUIRE(released.size() == 1);
    REQUIRE(released[0] == 10);
    REQUIRE(rows.live_node_count() == 3);
    REQUIRE(rows.visible_target(0, 1) == no_target);
    REQUIRE(rows.visible_target(0, 2) == 20);
    REQUIRE(rows.visible_target(0, 4) == 40);

    released = rows.prune_rows(4);
    REQUIRE(released.size() == 2);
    REQUIRE(rows.live_node_count() == 1);
    REQUIRE(rows.newest(0) == fourth);
    REQUIRE(rows.visible_target(0, 2) == no_target);
    REQUIRE(rows.visible_target(0, 4) == 40);

    RowIndex pinned;
    pinned.ensure_rows(1);
    const RowNodeId pinned_first = pinned.allocate_node(
        {.commit_id = 1, .target = 50, .previous = no_row_node});
    const RowNodeId pinned_second = pinned.allocate_node(
        {.commit_id = 2, .target = 60, .previous = pinned_first});
    pinned.set_newest(0, pinned_second);
    const auto pinned_release = pinned.prune_rows(2, 0);
    REQUIRE(pinned_release.empty());
    REQUIRE(pinned.live_node_count() == 2);
}

void check_row_chunk_directory_auto_shrinks() {
    RowIndex rows;
    rows.ensure_rows(1);

    std::vector<RowNodeId> nodes;
    nodes.reserve(600);
    for (std::size_t i = 0; i < 600; ++i) {
        nodes.push_back(rows.allocate_node(
            RowVersionNode{
                .commit_id = 1,
                .target = static_cast<TargetId>(i + 1),
                .previous = no_row_node,
            }));
    }

    REQUIRE(rows.live_node_count() == 600);
    REQUIRE(rows.chunk_count() == 3);
    REQUIRE(rows.chunk_directory_slots() == 3);

    for (const RowNodeId node : nodes) {
        rows.release_node(node);
    }

    REQUIRE(rows.live_node_count() == 0);
    REQUIRE(rows.chunk_count() == 0);
    REQUIRE(rows.chunk_directory_slots() == 0);

    const RowNodeId reused = rows.allocate_node(
        {.commit_id = 2, .target = 1, .previous = no_row_node});
    REQUIRE(RowNodeLayout::chunk_index(reused) == 0);
    REQUIRE(RowNodeLayout::slot_index(reused) == 1);
    rows.release_node(reused);
    REQUIRE(rows.chunk_directory_slots() == 0);
}

}  // namespace

int main() {
    check_row_index_chain();
    check_commit_publication_and_historical_reads();
    check_tombstone_visibility();
    check_prepare_failure_rolls_back_nodes();
    check_current_cache_rebuild();
    check_commit_log_uses_preallocated_blocks();
    check_truncate_keeps_room_for_replacement();
    check_row_pruning_keeps_snapshot_tail();
    check_row_chunk_directory_auto_shrinks();
}
