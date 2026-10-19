# Overview

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 1. Goal

Provide a `MVCCHive<T>` that owns a sequence of immutable payload versions and
supports snapshots, transactions, recoverable undo/redo navigation with
redo-tail discard, commit history, and pruning.

The central rule is:

> A stored `T` is constructed once in a stable cell. Commits change an index
> from virtual IDs to version targets and prepend immutable row-version nodes.
> They never move, copy, assign, or swap a stored `T`.

This makes the history genuinely multi-version rather than a sequence of
payload mutations that happen to be reversible.

## 2. Hard constraints

- Target language: C++23.

- The public type is `MVCCHive<T>`.

- `T` only needs to be destructible and constructible in place from the
  arguments supplied to an operation. It does not need to be default
  constructible, copyable, movable, or assignable.

- Payload destruction is expected to be `noexcept`
  (`std::is_nothrow_destructible_v<T>`). Reclamation and commit cleanup run in
  no-throw contexts; a throwing destructor violates the publication contract.

- Insertion accepts constructor arguments, not an existing `T` object:
  `txn.emplace(args...)`. There is intentionally no `insert(T&&)` API
  unless `T` says it's movable without exception.

- The implementation never calls `std::move`, `std::swap`, copy assignment,
  move assignment, or any equivalent operation on a stored `T`.

- A published `T` is exposed only as `const T&`, or as an
  `std::optional<std::reference_wrapper<const T>>` when absence is possible.

- Payload addresses remain stable for the lifetime of the version.

- Per-version metadata is stored out of line from the payload; `T` is not
  wrapped in a node or padded with hive bookkeeping fields.

- Public and fallible absence is represented by `std::optional`; nullable
  values are not expressed with a null raw pointer. Compact internal
  `DiffRecord`s use the reserved `no_target` sentinel; public diffs expose only
  `insert`, `update`, or `remove` tags.

- Raw storage pointers may appear only as transient locals for placement
  construction or payload-offset arithmetic; they never represent ownership,
  element identity, row links, or absence.

- Row heads and row-version links use compact private indices, not pointers
  and not atomic optional references. A row head is one lock-free atomic
  integer; a node link is one integer sentinel.

- Commit installation has a throwing prepare phase followed by a `noexcept`
  publish phase. No allocation, construction, or other throwing operation is
  allowed after the first row-head release store.

- History is represented by per-row version chains and a compact diff log. It
  is not represented by swapping payloads between physical slots.

- Supported concurrency is one writer with concurrent immutable snapshot
  readers.

- Undo and redo move a linear checkout. The redo tail is retained until a new
  commit is published from the checkout; that commit discards the tail.

- `MVCCHive` and its `Snapshot` handles share ownership of one immutable
  generation containing the `VersionStore` and `RowIndex`. `clear()` and hive
  destruction invalidate outstanding handles immediately, while the retired
  generation remains alive until its last snapshot guard is released.

Stored values may own move-only resources, but even those resources are never
extracted from one version and moved into another. Users construct each new
version in place. Moving constructor arguments into a `T` constructor is the
user's concern and is outside the no-move rule for stored values.

## 3. Terminology

| Name | Meaning |
| --- | --- |
| `CommitId` | Identifies one point in the linear commit timeline. Automatic IDs start at 1; caller-supplied IDs may be sparse but must exceed the hive's monotonic high-water mark. |
| `VirtualId` | Stable logical element identity. It is never a physical payload address. |
| `TargetId` | Opaque location of one payload slot, encoded as `(block_id << BlockLayout<T>::shift) + slot_index`. |
| `DiffKind` | Public tag for a normalized history change: `insert`, `update`, or `remove`. |
| `Diff` | Public copied history record containing `commit_id`, `virtual_id`, and `DiffKind`; it contains no target identity. |
| `RowNodeId` | Compact private index of one `RowVersionNode`, encoded as `(chunk_id << RowNodeChunk::shift) + slot_index`. Zero is the empty/tail sentinel. |
| `RowVersionChain` | Immutable nodes ordered newest-to-oldest for one `VirtualId`, each naming a commit and the target visible from that commit onward. |
| `CurrentTargets` | Writer-only dense cache of the target currently mapped by each `VirtualId`; rebuildable from the authoritative row heads. |
| `Commit` | A point in the linear timeline plus its range in the diff log. |
| `Checkout` | The commit currently selected for `head()` and writable transactions. It may be older than the retained tip. |
| `Redo tail` | Retained commits newer than the checkout, available to `redo()` until a new commit is published. |
| `Snapshot` | A read-only view pinned to one `CommitId` and protected by the active-snapshot horizon. |
| `Transaction` | A private mutable overlay over a parent snapshot, plus versions it has constructed but not published. |
| `Install` | Apply a normalized diff set to the current targets and prepend the corresponding row-version nodes. |
| `Max commit ID` | The largest commit ID ever issued by a hive. It does not decrease after undo, pruning, or `clear_history()`; only `clear()` resets it. |

Public and fallible lookup APIs use `std::nullopt` for absence. Compact
internal mapping records use the reserved `no_target` value. A row-version node
with `no_target` acts as a tombstone for that commit and later views.

Virtual IDs are monotonically allocated and are not reused. This removes the
ABA ambiguity of the old physical-slot IDs: two handles can never refer to
different incarnations of the same `VirtualId`.
