# Correctness invariants

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 15. Correctness invariants

The implementation should assert or enforce all of the following:

1. A `VirtualId` is never reused.
2. A commit ID is unique and increases along the linear timeline.
3. Installing a commit prepends exactly its normalized row-version nodes and
   updates exactly the touched current-target entries.
4. Every diff's `previous_target` matches the parent snapshot resolution.
5. A commit has at most one diff per `VirtualId`.
6. A public or logical absence is `std::nullopt`; compact internal row heads
   and row links use the `no_row_node` integer sentinel; reader-directory
   entries use engaged/disengaged optionals. No nullable raw pointer is used.
7. A published `T` is never moved, copied, assigned, swapped, or mutated
   through the public API.
8. A target is not released while any row-version node or transaction anchors
   it.
9. A valid snapshot sees exactly the row versions visible at its commit,
   regardless of later commits.
10. A transaction publishes no state until `commit()` succeeds.
11. Rollback leaves the current-target table, row chains, and all published
    versions untouched.
12. A physical cell is not recycled while any row-version node or transaction
    still anchors its previous target.
13. During normal operation, a block is released only when its `live_versions`
    count is zero. Final `VersionStore` teardown is the sole exception; it
    destroys every remaining block because no snapshot remains valid.
14. A reused block index is published only after its previous block has been
    fully released and no live target can still reference it.
15. A history-drop shrink never relocates a live `T`.
16. The payload region contains only constructed `T` objects and raw empty
    storage; hive metadata never shares a version's payload stride.
17. A free slot has its free bit set and view count zero; a live slot has its
    free bit clear.
18. `BlockLayout<T>::capacity` and `DirectoryPageCapacity` are powers of two,
    so block, slot, page, and page-offset projection use shifts and masks.
19. A snapshot read never mutates free bits, block live counts, or payload
    bytes.
20. During normal operation, a target's view count reaches zero exactly once
    and cannot be resurrected after it is queued for retirement.
21. Published directory pages never relocate; block and page references are
    published and consumed with release/acquire ordering.
22. No private storage type, packed target representation, lock, or block-index
    constant appears in the public API.
23. A missing lookup is `std::nullopt`; no public API returns a nullable raw
    pointer.
24. `VersionStore`'s owning `unique_ptr` sequence is the sole block owner; the
    atomic reader directory is non-owning and points only to a fully
    initialized block.
25. Snapshot readers never index or iterate the owning `std::deque`.
26. Snapshot resolution uses only its commit ID and row-version chains; it
    never scans the diff log or the writer-only current-target table.
27. In multi-writer mode, every mutation of an existing `VirtualId` holds its
    exclusive row lock from the first write through commit or rollback.
28. Multi-row locks are acquired in ascending `VirtualId` order.
29. A row-lock conflict aborts or retries the transaction; it never publishes
    a diff against a stale touched-row target.
30. Visitors never acquire row locks.
31. Commit IDs decrease strictly along each row-version chain from newest to
    oldest.
32. `current_targets[vid]` equals the target of the newest row-version node at
    or below `head_commit_` for `vid`; it is a rebuildable cache, while the row
    head is authoritative.
33. During normal operation, a row-version node visible at or below an active
    snapshot horizon is not reclaimed.
34. Row-head updates use release publication; snapshot traversal uses acquire
    loads.
35. A snapshot point lookup never scans unrelated rows or the global diff log.
36. Internal and public `Diff` records are 32-byte trivially copyable values;
    `no_target` never denotes a live version.
37. Row heads and row links use `RowNodeId`; block-directory and row-node-
    directory entries use engaged/disengaged optional references, never
    nullable raw pointers.
38. `RowNodeId` zero is the empty/tail sentinel, and allocation never returns
    zero as a live node.
39. A row node is fully initialized before its head is published; head stores
    are release stores and snapshot traversal uses acquire loads.
40. The prepare phase completes before the first row-head release store. The
    publish phase performs no allocation, construction, or other throwing
    operation.
41. A recoverable exception never changes a row head; a stale
    `current_targets` entry is repaired by rebuilding it from the row heads.
42. During normal operation, a row-node chunk is released only when its
    `live_nodes` count is zero and no live snapshot can traverse any node in
    it. Final `RowIndex` teardown destroys every remaining chunk.
43. `RowNodeChunk::capacity` and the row-node directory page capacity are
    powers of two, and chunk/slot/page projection uses shifts and masks.
44. Snapshot resolution never consults `current_targets` or the global diff
    log.
45. A row node whose commit is newer than `head_commit_` is skipped by both
    snapshot resolution and `rebuild_current_targets()`.
46. `VersionToken::none()` carries exactly `no_target`, a default-constructed
    `VersionToken` is none, and no live target can equal it. The block
    allocator enforces `block_id <= max_block_index`, reserving the top block
    index so the sentinel is structurally unreachable.
47. `MVCCHive` and its snapshot guards share ownership of immutable
    `HiveState` generations containing `VersionStore` and `RowIndex`.
    `Transaction` remains a writer-locked child handle.
48. `~MVCCHive` invalidates outstanding `Snapshot` objects deterministically.
    A generation's store and remaining payloads are destroyed when the last
    guard for that generation is released. An active `Transaction` must finish
    or be destroyed first because the destructor takes the writer lock.
49. A `SnapshotGuard` holds a weak reference to the snapshot registry and a
    strong reference to its generation. If the registry has already been
    destroyed, unregistering is a no-op while generation ownership remains
    intact.
50. A `Snapshot` remains memory-safe after its hive is destroyed or after
    `clear()`: `valid()` reports false, `find` returns `nullopt`, and `at`
    throws. Its guard keeps the retired generation alive until the handle is
    released.
51. `clear()` retires the current generation, replaces the snapshot registry
    to invalidate outstanding snapshots, and installs a fresh empty state.
52. `Transaction::at` and `find` consult the transaction overlay first and the
    fixed parent snapshot second. They never scan the diff log or
    `current_targets`.
53. The overlay contains at most one entry per touched `VirtualId`, and its
    value is the transaction's final `TargetId` or `no_target`. Keys are not
    removed during the transaction, so lookup needs no tombstones.
54. An overlay hit does not walk a row chain; a parent hit walks only the
    requested row's chain. Successful `at` and `find` calls allocate no
    memory; constructing the `at` exception on absence may allocate.
55. `at` throws `std::out_of_range` for an absent or erased row; `find` is
    `noexcept` and returns `std::nullopt` for the same case.
56. A reference returned by `Transaction::at` or `find` remains valid until the
    transaction ends. A later write may make it logically stale, but the
    target remains anchored for the transaction's lifetime.
57. A transaction never copies the parent projection; its space is `O(k + q)`
    for `k` touched IDs and `q` constructed staged versions.
