# Implementation plan

Status: M0 through M5 are implemented. Concurrency stress testing and model
validation remain.

The first goal is a small, testable vertical slice that proves the layout,
payload lifetime, row-chain resolution, and commit publication rules before
adding pruning and multi-writer work.

## Proposed file layout

```text
include/mvcc_hive/
  mvcc_hive.hpp                  public entry point and MVCCHive<T>
  detail/
    block_layout.hpp             BlockLayout<T>, target encode/decode
    version_block.hpp            VersionBlock<T> and block metadata
    stable_directory.hpp         stable paged reader directories
    version_store.hpp            block ownership, placement lifetime, retirement
    row_node_arena.hpp           RowNodeChunk and RowNodeId allocation
    row_index.hpp                row pages, row heads, node directory
    commit_store.hpp             commit records and normalized diff log
    transaction.hpp              overlay, staging, commit builder
    snapshot.hpp                 snapshot resolution and guard
tests/
  layout_test.cpp
  storage_test.cpp
  history_test.cpp
  transaction_lookup_test.cpp
  lifetime_test.cpp
  concurrency_test.cpp
  model_test.cpp
```

The library starts header-only because `MVCCHive<T>` and its storage layers are
templates. A compiled `.cpp` layer should be added only for non-template
teardown or platform-specific directory code that benefits from it.

## Milestones

### M0: Build scaffold

- CMake interface target with C++23 enabled.
- Minimal include smoke test.
- Build presets for a local debug configuration.

Acceptance: configure, build, and `ctest` succeed with no warnings from the
project headers.

### M1: Layout and sentinel invariants

Status: implemented in `detail/layout.hpp`, `version_token.hpp`, and
`tests/layout_test.cpp`.

- Implement `BlockLayout<T>` with the 16 KiB payload budget and power-of-two
  capacities.
- Implement `TargetId` encode/decode and `max_block_index` reservation.
- Implement `RowNodeId` encode/decode and the zero sentinel.
- Add static assertions for `Diff`, `VersionToken`, `RowVersionNode`, and
  `RowHead` sizes.

Acceptance: layout tests cover `sizeof(T)` 8, 64, 128, and 392 bytes; target
zero is live; no live target equals `no_target`.

### M2: Version storage

Status: implemented in `detail/version_block.hpp`,
`detail/block_directory.hpp`, `detail/version_store.hpp`, and
`tests/storage_test.cpp`.

- The initial block directory uses stable pages plus a short shared-lock
  fallback. The compiler reports that
  `std::atomic<std::optional<std::reference_wrapper<T>>>` is 16 bytes and not
  lock-free on the MinGW/GCC target, so it is not used on the hot path yet.

- Allocate `VersionBlock<T>` with raw aligned payload bytes.
- Add free-bit allocation, per-slot view counters, and block live counts.
- Implement placement construction and in-place destruction.
- Publish blocks through the stable reader directory.
- Implement explicit block release and directory shrink.
- Implement final `VersionStore` teardown that destroys every remaining
  constructed `T`.

Acceptance: block allocation constructs zero `T` objects; explicit release
returns storage; final teardown destroys every live payload exactly once.

### M3: Row history and commit publication

Status: implemented in `detail/row_index.hpp`, `detail/commit_store.hpp`,
`detail/current_targets.hpp`, `detail/commit_builder.hpp`, and
`tests/history_test.cpp`.

- Implement row pages with atomic `RowNodeId` heads.
- Implement `RowNodeChunk` allocation, the chunk directory, and `previous`
  links.
- Implement the `current_targets` derived cache and its rebuild.
- Implement `CommitBuilder::prepare` and `CommitBuilder::publish`.
- Add debug assertions for prepare/publish and row-head/cache agreement.

Acceptance: a commit touching `k` rows creates exactly `k` nodes; a prepare
failure changes no row head; publish performs no allocation.

### M4: Snapshots and transactions

Status: implemented in `mvcc_hive.hpp`, `detail/snapshot_registry.hpp`,
`detail/transaction_overlay.hpp`, and `tests/transaction_test.cpp`.

The first writable-transaction policy requires a transaction to bind to the
current head. Historical commits remain readable through snapshots, but
writable rebasing onto a newer head is deferred to the multi-writer work.

- Implement the snapshot guard registry and weak registry references.
- Implement snapshot resolution by commit-filtered row-chain traversal.
- Implement the flat transaction overlay for insert, replace, erase, and swap.
- Implement `Transaction::at`/`find` overlay-first resolution.
- Implement commit normalization and anchor transfer.

Acceptance: transaction reads see their own staged changes; old snapshots see
old targets; references remain valid until the transaction ends.

### M5: Pruning and deterministic lifetime

Status: implemented in `prune_before`, `clear_history`, `clear`,
`detail/row_index.hpp`, `detail/current_targets.hpp`,
`detail/snapshot_state.hpp`, and `tests/prune_test.cpp`.

The pruning pass prepares compact commit metadata and a row-prune plan before
changing history. After those allocations succeed, it reserves the exact
retirement capacity, truncates retained chains, releases target anchors,
drains retirement, and shrinks empty block/chunk directories without moving a
live payload.

An old snapshot pins every row node whose `previous` link it may traverse, not
only targets it can visibly resolve. This conservative rule avoids mutating a
link concurrently with snapshot traversal without turning `previous` into an
atomic field.

- Implement `prune_before` and `clear_history`.
- Implement row-node horizon protection and empty-chunk release.
- Implement destructive `clear()` and hive teardown ordering with shared
  generation lifetime for snapshot readers.
- Add `Snapshot::valid()`.

Acceptance: snapshots remain valid across normal pruning, become invalid
immediately at `clear()`/hive destruction, and their retired generation is
destroyed safely when the last snapshot guard is released.

### M6: Concurrency and model testing

- Run one committing writer with multiple snapshot readers under TSAN.
- Exercise final-view retirement and block release.
- Add a randomized reference-model test over commit IDs, virtual IDs, and
  values.
- Leave the row-lock manager as the explicit multi-writer seam.

Acceptance: no reader can observe a destroyed payload, no target is recycled
while anchored, and the reference model matches every snapshot.

## First vertical slice

The first implementation should stop after a narrow version of M1-M4:

- one payload type whose size is known at compile time;
- one writer;
- snapshots that do not outlive the hive;
- no pruning;
- no multi-writer row locks;
- no history compaction beyond normal commit append.

That slice is enough to validate the two central bets:

1. payloads can remain completely stable while row heads move;
2. snapshot reads are row-chain resolution, not projection copies.

## Open decisions to settle during implementation

- Confirm `std::atomic<std::optional<std::reference_wrapper<...>>>` is
  lock-free on the target platform, or activate the documented fallback for
  the block and row-node directories.
- Confirm `std::atomic<RowNodeId>` and `std::atomic<CommitId>` are lock-free.
- Measure whether one node-chunk directory load per row-chain step is
  acceptable; retain the index representation unless profiling shows a
  material regression.
- Confirm the 16 KiB payload budget against the expected `T` sizes and cache
  behavior.
- Decide whether the public namespace is `mvcc_hive` or a project-specific
  alias before the first public header is finalized.

## Non-goals for the first implementation

- Concurrent write transactions.
- Branching or mergeable commit graphs.
- Snapshot handles that outlive their hive.
- Block compaction that moves a live `T`.
- Predicate locks or serializable read-write validation.
