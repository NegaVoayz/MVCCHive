# Public API

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 9. Proposed public API

The following is an API sketch, not final source code:

```cpp
template<class T>
class MVCCHive {
public:
    using VirtualId = std::uint64_t;
    using CommitId = std::uint64_t;

    // Opaque, equality-comparable target location in the retained trace.
    // Its bit layout is intentionally not part of the public contract.
    struct VersionToken {
        friend bool operator==(VersionToken, VersionToken) = default;
        static constexpr VersionToken none() noexcept {
            return VersionToken{};
        }
        bool has_value() const noexcept {
            return bits != none_bits;
        }

    private:
        friend class MVCCHive;
        static constexpr std::uint64_t none_bits =
            std::numeric_limits<std::uint64_t>::max();
        std::uint64_t bits = none_bits;
    };

    struct alignas(32) Diff {
        CommitId commit_id;
        VirtualId virtual_id;
        VersionToken previous_target;
        VersionToken current_target;
    };

    class Snapshot;
    class Transaction;

    MVCCHive();
    ~MVCCHive();

    Snapshot head() const;
    std::optional<Snapshot> snapshot(CommitId id) const;

    Transaction begin_transaction();
    Transaction begin_transaction(CommitId id);

    std::optional<Diff> diff(std::size_t index) const;
    std::vector<Diff> diffs(CommitId id) const;

    void prune_before(CommitId first_retained);
    void clear_history();
    void clear();  // destructive: invalidates snapshots and destroys the store

private:
    using TargetId = std::uint64_t;  // private block/slot location
    static constexpr TargetId no_target =
        std::numeric_limits<TargetId>::max();
    static_assert(no_target == VersionToken::none_bits);
    using RowNodeId = std::uint64_t; // private row-version index
    using TransactionId = std::uint64_t;

    struct BlockHeader;
    template<class U>
    struct BlockLayout;
    template<class U>
    struct VersionBlock;
    struct DirectoryPage;
    class BlockDirectory;
    class VersionStore;
    struct RowVersionNode;
    struct RowPage;
    struct RowNodeChunk;
    class RowIndex;
    class RowNodeArena;
    template<class U>
    class StableDirectory;
    class SnapshotRegistry;
    class SnapshotGuard;
    struct Commit;
    class CommitStore;
    class RetirementQueue;
    class RowLockManager;
    class RowLockSet;

    std::unique_ptr<VersionStore> store_;
    std::unique_ptr<RowIndex> row_index_;
    // Guards hold weak references; this is the sole strong owner.
    std::shared_ptr<SnapshotRegistry> snapshots_;
    std::vector<TargetId> current_targets_;  // derived writer-only cache
    std::atomic<CommitId> head_commit_;
    mutable std::mutex writer_mutex_;
    mutable std::shared_mutex history_mutex_;
    RowLockManager row_locks_;
    CommitStore commits_;
};
```

`VersionToken` is an opaque target-location token for history inspection. A
caller can compare tokens, but cannot decode a block index, reach payload
storage, or use a token to mutate a version. Historical identity comes from the
accompanying commit and virtual IDs. The token is one machine word, so the
public `Diff` representation remains 32 bytes as well. `VersionToken::none()`
is the all-ones sentinel, a default-constructed token is none, and the target
allocator's top-block reservation makes that bit pattern unreachable by any
live version. Projection is exactly
`bits = (target == no_target) ? none_bits : target`; no other transformation
or normalization is applied.

Because `store_` and `row_index_` are `unique_ptr`s to private incomplete
types, `~MVCCHive()` is defined out of line after `VersionStore` and
`RowIndex` are complete. Its explicit destruction order is: detach/invalidate
snapshot guards, release the row index, then destroy the version store. The
member declaration order is chosen so `store_` is the last member destroyed.

Token equality means equal physical target location in the retained trace. It
does not imply equal version identity across different commit IDs, because a
slot may be reused after all live anchors are gone. A token is not a
lifetime-extending handle; tokens returned before `clear()` or before the
hive was destroyed are stale afterward.

History inspection returns values rather than references into mutable log
storage. A concurrent writer can therefore append or compact the log without
invalidating a caller's range or exposing an internal `span`.

### 9.1 Transaction API

```cpp
template<class T>
class MVCCHive<T>::Transaction {
public:
    template<class... Args>
    VirtualId emplace(Args&&... args);

    template<class... Args>
    void replace(VirtualId id, Args&&... args);

    void erase(VirtualId id);
    void swap(VirtualId a, VirtualId b);

    const T& at(VirtualId id) const;  // throws std::out_of_range
    std::optional<std::reference_wrapper<const T>>
    find(VirtualId id) const noexcept;

    Snapshot commit();
    void rollback() noexcept;

private:
    friend class MVCCHive;

    std::unique_lock<std::mutex> writer_lock_;
    VersionStore& store_;
    RowIndex& rows_;
    Snapshot parent_;
    CommitId commit_id_;
    TransactionId transaction_id_;
    RowLockSet row_locks_;
    TransactionOverlay overlay_;
    std::vector<TargetId> staged_targets_;
};
```

The transaction is the only write surface. It binds to the head snapshot when
created. `commit()` publishes an atomic commit; `rollback()` destroys every
version constructed only by that transaction. A transaction holds non-owning
store and row-index references and must be completed or destroyed before its
hive is destroyed.

`replace` is the new name for the old `update`. It constructs a completely new
version and prepends a row-version node. It never assigns over the old `T`.

All staging, locking, anchor bookkeeping, and commit construction are private
to `Transaction`. A caller cannot reach a target ID, a block, or the parent
snapshot's row index.

References returned by `Transaction::at` and `Transaction::find` belong to the
transaction's current view. A later write in the same transaction may make such
a reference stale. Snapshot references have the stronger guarantee: they stay
valid while the snapshot and its hive are alive.

#### 9.1.1 Transaction lookup

`Transaction::at` and `Transaction::find` resolve in this order:

1. Look up `id` in the transaction's private overlay.
2. If the overlay contains the ID, the stored `TargetId` is the transaction's
   final staged value. `no_target` means erased; otherwise resolve that target
   in `VersionStore`.
3. If the overlay does not contain the ID, resolve it in the fixed parent
   snapshot.
4. If the parent has no visible target, the row is absent.

The parent snapshot is captured when the transaction begins. The overlay is
the only source of uncommitted state; `at` never copies the parent projection,
scans the diff log, or reapplies inverse operations.

The overlay is a flat, open-addressed table keyed by `VirtualId`:

```cpp
struct OverlaySlot {
    VirtualId key;
    TargetId target;  // no_target means erased
    bool occupied;
};
```

Keys are never removed during a transaction, so no tombstones are needed.
An erase stores `no_target`; a later insert or replace overwrites the value.
A separate append-only `touched` list records the first write to each key for
commit normalization. Capacity is a power of two, probing is linear, and the
maximum load factor is about 0.7. Growing the table can allocate and throw;
lookup itself does not.

```cpp
const T& Transaction::at(VirtualId id) const {
    if (const TargetId* staged = overlay_.find(id)) {
        if (*staged == no_target)
            throw std::out_of_range("transaction row is erased");
        return store_.resolve(*staged);
    }

    const TargetId parent = parent_.visible_target(id);
    if (parent == no_target)
        throw std::out_of_range("transaction row does not exist");
    return store_.resolve(parent);
}
```

`store_.resolve` decodes the block and slot with shift/mask, performs one
stable block-directory load, and computes the payload address. It is `O(1)`
and does not acquire a store lock or increment a view count. The parent
resolution is the same row-chain walk used by `Snapshot`. `at` returns a
reference to that payload; it never copies, moves, or constructs a `T`.

`find` follows the same path but returns `std::nullopt` instead of throwing.
It is `noexcept`: overlay lookup, target resolution, and optional/reference
construction do not allocate. The successful `at` path is also
allocation-free; constructing the `std::out_of_range` on an absent row may
allocate.

`at` touches only the overlay slot and, on an overlay miss, the requested
row's chain. It never scans unrelated rows, the diff log, or the writer-only
`current_targets` table.

| Case | Time | Extra space |
| --- | --- | --- |
| Overlay hit (insert, replace, erase, swap) | `O(1)` expected hash lookup plus `O(1)` target resolution | `O(1)` |
| Overlay hit that is erased | `O(1)` expected; throws or returns `nullopt` immediately | `O(1)` |
| Parent hit, head-bound transaction | `O(1)` expected: row head, one node-chunk load, target resolution | `O(1)` |
| Parent hit, old parent snapshot | `O(c)`, where `c` is the number of that row's changes after the parent commit | `O(1)` |
| Parent miss | same as parent hit, then absent | `O(1)` |
| Overlay storage for `k` touched IDs | insert/update `O(1)` expected; commit normalization sorts `k` keys in `O(k log k)` | `O(k)` |

`at` itself uses `O(1)` extra space and never materializes a projection. The
transaction's total space is `O(k + q)`: `O(k)` for the overlay and touched
list, and `O(q)` for staged targets and constructed payload versions, where
`q` is the number of versions constructed (which can exceed `k` if one row is
replaced repeatedly). It is never `O(V)` merely because the parent has `V`
rows. The `O(k)` worst-case hash-collision time is avoided in normal use by a
non-adversarial hash and a load factor below one; a randomized or keyed hash
can be used if adversarial `VirtualId`s are a concern.

Reference lifetime follows the target's anchor:

- a parent target is anchored by the transaction's parent snapshot guard;
- a staged target is anchored by the transaction's staged-target record until
  commit or rollback;
- a later write may change what `at` returns, but a previously returned
  reference remains valid and logically stale until the transaction ends;
- references must not outlive the transaction or its hive.

`at` and `find` are active-transaction operations; querying the transaction
after `commit()` or `rollback()` is invalid.

There is no per-read memoization cache. Repeated `at` calls on the same
old-parent row pay the row-chain walk again. Adding a cache would use
`O(distinct reads)` space and require invalidation on every write; that is a
later optimization only if profiling shows it matters.

### 9.2 Snapshot API

```cpp
template<class T>
class MVCCHive<T>::Snapshot {
public:
    CommitId commit_id() const noexcept;
    bool valid() const noexcept;  // false after hive destruction or clear()

    bool contains(VirtualId id) const;
    const T& at(VirtualId id) const;
    std::optional<std::reference_wrapper<const T>>
    find(VirtualId id) const noexcept;

    template<class F>
    void for_each(F&& fn) const;

private:
    friend class MVCCHive;
    friend class Transaction;

    CommitId commit_id_;
    std::reference_wrapper<const RowIndex> rows_;
    std::reference_wrapper<const VersionStore> store_;
    std::shared_ptr<SnapshotGuard> guard_;

    TargetId visible_target(VirtualId id) const noexcept;  // no_target if absent
};
```

A snapshot is immutable and can be read after later commits are published
while its hive is alive. Its active-snapshot guard prevents row-version nodes
visible to its commit from being pruned. It holds only non-owning references
to the row index and version store; it does not own them and does not extend
their lifetime. Destroying the hive destroys the `VersionStore` immediately,
so an outstanding `Snapshot` is invalid after that point and must not be
used. `clear()` has the same lifetime effect: it invalidates outstanding
snapshots and installs a fresh empty state.

`Snapshot::valid()` checks the guard's weak registry reference and does not
touch the payload store. It becomes false after hive destruction or `clear()`.
The other read methods require `valid() == true`; debug builds may assert it,
but the hot read path does not acquire a lifetime check on every lookup.

| Owner | Strongly owns | Lifetime |
| --- | --- | --- |
| `MVCCHive` | `VersionStore`, `RowIndex`, `SnapshotRegistry` | the hive |
| `Snapshot` | shared `SnapshotGuard` | its copies, but valid only while the hive is alive |
| `Transaction` | writer-lock handle, overlay, staged anchors | the transaction; must end before the hive |

The row-index reference, store reference, target decoding, snapshot guard, and
resolution helpers are private. A caller can read a value or enumerate the
logical view, but cannot inspect how the snapshot is represented.

### 9.3 Time travel

`hive.snapshot(commit_id)` is the primitive operation. Old-style `undo()` and
`redo()` become navigation between snapshots, not payload mutations. If
convenience methods are retained, they should be expressed as:

```text
previous(snapshot) -> snapshot at the parent commit
next(snapshot)     -> snapshot at the next linear child, if any
```

The important semantic change is that viewing an old commit does not mutate
the hive's current state.

### 9.4 Visibility boundary

The public contract consists of:

- `MVCCHive<T>`, `Snapshot`, and `Transaction`;
- `VirtualId`, `CommitId`, and opaque `VersionToken`;
- logical snapshot reads and normalized history diffs.

Everything below the logical row index is private:

- block capacity, shift, masks, and free-bit layout;
- target location encoding and directory metadata;
- `VersionBlock`, `DirectoryPage`, and `BlockDirectory`;
- `RowNodeId`, `RowVersionNode`, `RowNodeChunk`, `RowNodeArena`, row-table
  pages, and `RowIndex`;
- `current_targets` and the commit prepare/publish builder;
- view-counter arrays, retirement queue, and free-block list;
- snapshot registry, commit records, and diff-log representation;
- writer mutex, publication atomics, and storage allocation.

No public method accepts a block index, slot index, `VersionToken`, or raw
target pointer as a capability. This keeps callers from depending on a layout
that the MVCC design must remain free to change.
