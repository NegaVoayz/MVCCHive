# MVCCHive design

Release status: version 2.0.0 covers the single-writer design. Multiwriter row
locking is the intentionally deferred extension.

`MVCCHive<T>` is a C++23 MVCC container. Stored values are constructed once in
stable payload slots and are never moved, copied, assigned, or swapped by the
hive. History is represented by per-row version chains plus a compact diff log.

## Design index

- [Overview](01-overview.md): goals, hard constraints, and terminology.
- [Architecture and design decisions](02-architecture.md): cooperating layers
  and the deliberate departures from `SwapDiffHive`.
- [History model](03-history.md): normalized diffs, the commit store, and
  commit-record layout.
- [Version storage](04-version-storage.md): adaptive SoA blocks, target
  identity, block release, and the reader directory.
- [Row index and snapshots](05-row-index.md): row-chain indices, the node
  arena, commit publication, and snapshot resolution.
- [Public API](06-public-api.md): transaction and snapshot surfaces, lookup
  rules, time travel, and the visibility boundary.
- [Operation semantics](07-operations.md): emplace, replace, erase, swap,
  normalization, and publication.
- [Lifetime, reclamation, and pruning](08-lifetime-and-pruning.md): target
  anchors, deterministic store destruction, history retention, and shrink.
- [Complexity](09-complexity.md): expected costs for reads, writes, commits,
  and history drops.
- [Concurrency and locking](10-concurrency.md): one-writer v1, snapshot
  readers, and the row-lock seam for multi-writer mode.
- [Correctness invariants](11-correctness.md): the invariants an implementation
  must assert or enforce.
- [Test plan](12-test-plan.md): layout, behavioral, lifetime, and concurrency
  coverage.
- [Implementation plan](13-implementation-plan.md): first slice, file layout,
  milestones, and acceptance criteria.

## Core model in one page

- `VirtualId` is stable logical identity and is never reused.
- `TargetId` is an opaque payload location: `(block_id << shift) | slot`.
- `RowVersionNode` is an immutable newest-to-oldest chain node addressed by a
  private `RowNodeId`; zero is the empty/tail sentinel.
- `row_head[vid].newest` is authoritative. `current_targets` is a writer-only,
  rebuildable cache.
- A snapshot stores a commit ID and a guard that shares ownership of the
  generation containing `RowIndex` and `VersionStore`. It does not materialize
  a projection.
- A transaction resolves an ID through its private overlay first and its fixed
  parent snapshot second.
- Commit installation has a throwing prepare phase and a `noexcept` publish
  phase. Publication does not allocate or construct.
- `MVCCHive` and snapshot guards share ownership of the current generation.
  `clear()` and hive destruction invalidate outstanding snapshots immediately;
  the retired generation is destroyed when its last guard is released.
- Public history diffs are target-free values tagged `insert`, `update`, or
  `remove`. The block allocator's reserved top block keeps `no_target`
  unreachable from every live payload location.

Section numbers in the individual documents are preserved from the original
single-file design so existing cross-references remain stable.
