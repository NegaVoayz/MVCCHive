# Concurrency and locking

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 14. Concurrency model

The supported model is one writer and any number of concurrent snapshot
readers.

### 14.1 Writer exclusion

- `begin_transaction`, `prune_before`, `clear_history`, and `clear` acquire an
  exclusive writer lock.
- A `Transaction` owns that lock for its lifetime. It is a writer-local object
  and must not be shared between threads.
- There is at most one active transaction and therefore at most one writer.
- `~MVCCHive` acquires the writer lock before teardown, so an active
  transaction must commit, roll back, or be destroyed before the hive can be
  destroyed. The transaction object must not be used afterward.
- Commit publication and all version-store allocation, retirement draining,
  free-bit updates, and block release happen under writer exclusion.
- All reads and writes of the derived `current_targets` cache happen under
  writer exclusion; snapshots never access it.
- Commit-log append and history drop also take the history lock exclusively;
  `diff` and `diffs` take it shared and copy the requested records.

This whole-transaction writer lock is the conservative v1 policy. Section 14.6
defines the row-lock seam that replaces it when concurrent write transactions
are enabled.

### 14.2 Snapshot reads

- A `Snapshot` holds a commit ID and an active-snapshot guard. The guard shares
  ownership of the immutable generation containing the row index and version
  store.
- A snapshot is valid only while its hive generation is current.
  `Snapshot::valid()` checks the guard's weak registry reference and is false
  after hive destruction or `clear()`.
- Invalidating a handle does not destroy a generation that an in-flight reader
  may still be using. The generation remains alive until its last snapshot
  guard is released.
- `Snapshot::at` and `Snapshot::find` are `const` operations with no store
  lock, no writer-lock acquisition, and no atomic increment in the common
  payload lookup path.
- Different threads may read the same live `Snapshot` concurrently. `clear()`
  and hive destruction may run concurrently with snapshot reads: the guard
  keeps the read generation alive, while the registry makes future handle
  checks report invalid.
- A snapshot copied to another thread shares the same guard, generation
  ownership, and row-index reference. Copying the snapshot does not ask the
  payload store for a new view.

Row heads and the head commit are published with release/acquire semantics.
Snapshot lookup by commit ID synchronizes only while registering the horizon;
after that, row-chain traversal and payload access are read-only.

Snapshot registration uses a small registry mutex and a multiset of active
commit IDs. Copies of one snapshot share one registration. Destruction removes
that registration, which may advance the oldest active horizon. The registry
is never consulted by `Snapshot::at`; it exists only to keep row nodes alive
and to bound pruning. Each guard holds a `std::weak_ptr` to the registry, so a
guard that outlives the hive performs no registration access on destruction.

Commit-log inspection (`diff` and `diffs`) takes a short shared commit-store
lock and returns owned `Diff` values. It never returns a pointer or span into
storage that the writer may append to or compact concurrently.

### 14.3 Stable block directory

Readers must never observe a block directory while a writer relocates its
entries. The directory therefore uses stable pages rather than a resizable
array of entries:

- directory pages are allocated once and are never moved;
- growing the directory publishes a new page reference;
- an optional non-null block reference is stored in an atomic reader-directory
  entry; a disengaged optional is its empty state;
- the writer initializes the block fully before publishing the reference with
  release ordering;
- a reader loads the reference with acquire ordering.

A reader reaching a block already holds a view on the target. The block cannot
be released until that view count reaches zero, so the loaded block reference
remains valid for the read. Block free-list metadata is writer-only and is
never raced by a snapshot read.

Unlike a row link, a block-directory entry must ultimately resolve to the
block object itself; there is no second index space that would replace the
reference. This lookup happens once per target resolution, not once per row
chain step, so the non-owning reference is the cheaper representation here.
If a target platform reports the atomic optional reference as not lock-free,
the private directory may fall back to a paged table guarded by a short
reader-writer lock; that fallback does not affect the row-node index design.

Implementation note: the current MinGW/GCC target reports the atomic optional
reference as 16 bytes and not lock-free. The first storage implementation
therefore uses the paged fallback with a shared mutex. The directory lookup is
isolated in `detail/block_directory.hpp` so a platform-specialized lock-free
publication layer can replace it without changing `VersionStore` or row
history.

The power-of-two block capacity gives the same benefit at the directory level:
`block_index` can be split into page index and page offset with shifts and
masks rather than division.

### 14.4 Atomic view counters and reclamation

The `views` array is the synchronization point between row-node reclamation
and the writer that recycles payload slots:

- a new view is acquired with a relaxed atomic increment because the source
  leaf or transaction already pins the target;
- a view is released with acquire/release ordering;
- the thread observing the final transition to zero places the target in the
  retirement queue;
- the writer drains the retirement queue, rechecks that the count is zero,
  destroys the payload in place, and updates the free bitmap;
- a block is released only by the writer, only after all of its slots have a
  zero view count and no constructed payload. Final store teardown is the
  exclusive exception and destroys all remaining blocks.

The retirement queue is the only reader-to-writer handoff. It contains target
IDs or slot locations, never payloads. It may be protected by a small mutex or
implemented as a bounded MPSC queue. A history-drop operation drains it before
performing the block and directory shrink pass.

The hive and its snapshot guards share ownership of each `HiveState`
generation. `~MVCCHive` detaches the snapshot registry, making outstanding
handles invalid, then drops the hive's generation reference. If a guard still
exists, that generation remains alive and is destroyed when the final guard is
released. The store destructor drains remaining retirement state, destroys
every remaining constructed `T` exactly once, and releases all version blocks.
Because teardown is exclusive within a generation, it does not wait for target
view counts to reach zero.

### 14.5 Publication order

For every newly constructed target, the required order is:

1. placement-construct `T` in its stable payload slot;
2. initialize the view count and clear the free bit;
3. finish the containing block's metadata;
4. publish the block reference and/or target view with release semantics;
5. append the diff records and commit record into pre-reserved log slots;
6. publish each row head with a release store of its `RowNodeId`;
7. update each touched `current_targets` entry;
8. publish the new head commit with a release store.

Readers use the reverse acquire path. No `T` is mutated after step 2, so a
reader that observes a published target observes its complete value. Steps
5-8 are the `noexcept` publish phase; all allocation, node construction, and
validation happen before step 5.

### 14.6 Row-level write locks and the multi-writer seam

Row-level locks are only meaningful once a transaction stops holding the
global writer lock for its entire lifetime. The design reserves the seam now so
that enabling them does not require changing the payload, row-chain, or
snapshot layers.

The private lock manager is keyed by `VirtualId`:

```cpp
enum class RowLockMode { shared, exclusive };

class RowLockManager {
    // VirtualId -> owner TransactionId, mode, and waiter state
};

class RowLockSet {
    // RAII set of locks owned by one transaction
};
```

Lock records are created lazily in a sharded table, not as a
`std::mutex` embedded in every logical row. A record is removed when it has no
owner and no waiters.

#### Automatic acquisition

When multi-writer mode is enabled:

- `replace` and `erase` acquire an exclusive lock on the existing
  `VirtualId` before staging the change;
- `swap(a, b)` acquires exclusive locks on both IDs in ascending ID order;
- `emplace` needs no existing-row lock because its fresh `VirtualId` is not yet
  visible to another transaction;
- repeated edits to the same ID by the same transaction reuse one reentrant
  lock;
- all row locks are held until commit or rollback and are released by an RAII
  lock set even when construction or publication throws;
- a visitor never acquires a row lock. It reads an immutable snapshot.

An optional `lock_for_update(vid)` can acquire a shared lock for a
read-modify-write workflow. Exclusive upgrades should use timeout/abort rather
than blocking indefinitely.

#### Commit protocol with row locks enabled

1. `begin_transaction` captures a parent snapshot but does not take the global
   writer lock for its lifetime.
2. On the first write to an existing ID, the transaction acquires the row
   lock and verifies that the current head still maps that ID to the target
   used by the transaction view. A mismatch aborts with a row conflict.
3. The transaction stages versions and normalized changes while holding its
   row locks.
4. At commit, it takes only a short publication mutex.
5. Every touched row is revalidated against the current head. A stale cache
   entry is rebuilt from the head. Because the row locks were held
   continuously, another writer cannot have changed those rows; disjoint
   writers may have advanced the head.
6. The transaction's diffs are rebased onto the current head and published as
   a new linear commit.
7. The row locks are released after publication or abort.

Two transactions that touch disjoint rows can therefore prepare concurrently.
Two transactions that touch the same row serialize on that row. The global
publication mutex decides the commit order, not the transaction's interactive
lifetime.

The published commit record's `parent` is the current head at publication.
Each diff's `previous_target` is the value from the transaction's parent
snapshot, which the row-lock revalidation proves still matches the current
head for every touched row.

When multi-writer mode is enabled, an automatically assigned `CommitId` is
minted under the publication mutex. An explicit-ID overload must also validate
ordering and uniqueness there, or be removed from the concurrent API.

#### Deadlock and isolation limits

- Multi-row acquisition order is canonical: ascending `VirtualId`.
- A configurable wait timeout or deadlock detector should abort a waiting
  transaction rather than block forever.
- Row locks prevent write-write conflicts. They do not automatically provide
  serializable read-write validation.
- If a transaction reads row A and writes row B based on A, another
  transaction can still change A unless the caller uses a shared lock on A or
  the transaction validates its read set at commit.
- Predicate locks and phantom protection are out of scope.

#### Implementation placeholder

The v1 implementation may keep the whole-transaction writer lock from 14.1 and
let `RowLockManager` be a trivial/no-op layer. The required insertion points
already exist in the design:

- transaction identity and a private `RowLockSet`;
- automatic lock hooks in `replace`, `erase`, and `swap`;
- a short publication mutex separated from the interactive transaction;
- current-head validation and rebase before commit.

Activating multi-writer mode means narrowing the writer-lock scope to
publication and enabling those row-lock hooks. It does not affect
`Snapshot::at` or the row-chain format.
