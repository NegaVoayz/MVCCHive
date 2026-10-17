# Architecture and design decisions

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 5. Core architecture

```text
Stable row table
  VirtualId 0 -- newest RowVersion(C8, T12) -> RowVersion(C3, T2)
  VirtualId 1 -- newest RowVersion(C7, T9)
  VirtualId 2 -- newest RowVersion(C5, no_target) -> RowVersion(C2, T4)
  ...

TargetId -> stable directory page -> VersionBlock -> payload[cell]
                                                        |
                                                        +--> immutable T

Diff log: (8, V0, T2, T12), (7, V1, T5, T9), ...
```

There are four cooperating layers:

1. `VersionStore<T>` owns stable payload blocks and their SoA metadata.
2. `RowIndex` owns stable row heads and the row-node arena. It is the
   snapshot-visible row state and is owned by the hive, not by snapshots.
3. `CommitStore` owns commit metadata and the normalized diff log.
4. `MVCCHive<T>` owns `VersionStore`, `RowIndex`, and the writer-only
   `current_targets` cache, and coordinates transactions, snapshots,
   publication, and pruning.

The old `FreeChain`, ownership `book`, temporary payload array, and physical
slot swapping disappear entirely.

## 17. Deliberate departures from SwapDiffHive

- No `FreeChain`, ownership books, or temporary payload slots.
- No physical slot payload swapping.
- No requirement that `T` be movable. A value-taking insert or update overload
  may exist only when `T` is nothrow movable, and history still never moves a
  stored version.
- Virtual IDs are monotonic and are not recycled.
- Undo/redo are snapshot navigation, not inverse payload operations.
- Historical reads are first-class immutable snapshots.
- The diff log is a row-target change log:
  `commit_id, virtual_id, previous_target, current_target`.
- Version reclamation is based on row-chain retention and snapshot horizons,
  not on a global "current book" value.
- Payload blocks are explicitly owned through
  `std::deque<std::unique_ptr<VersionBlock<T>>>` and exposed to readers through
  a separate non-owning atomic block directory.
- Payloads and metadata use a block-local SoA layout instead of a per-version
  wrapper node.
- Block and directory-page capacities are powers of two, and index projection
  uses shifts and masks rather than division.
- Block capacity is derived from `sizeof(T)` at compile time; there is no
  fixed slot count that would over-allocate large payloads or under-allocate
  small ones.
- Row heads and row-version links are compact `RowNodeId` indices, not
  optional references or raw pointers. Row heads remain a single lock-free
  atomic integer.
- Commit installation has an explicit prepare/publish boundary. The prepared
  nodes are allocated before the boundary; publication is `noexcept` and
  performs no allocation or construction.
- `current_targets` is a derived writer-only cache. The row head is the
  authoritative current mapping, and the cache can be rebuilt after a fault.
- `MVCCHive` is the sole owner of `VersionStore` and `RowIndex`. Snapshots and
  transactions are non-owning child handles; destroying the hive destroys the
  store and all remaining payloads immediately.
- Caller-visible target history uses opaque `VersionToken` values; packing,
  directory pages, view counters, and locks are private.
- Payload liveness uses atomic per-slot view counters and deferred retirement;
  immutable snapshots read without store locks.
- Public nullable values and fallible lookups use `std::optional`; compact
  internal `Diff` targets use the reserved `no_target` sentinel to stay at
  32 bytes.

The row-chain representation, target encoding, and public transaction spelling
are intentionally separable. They can be revised without changing the central
MVCC rule: commits advance immutable row-version chains to new targets, and
payloads never move.
