# Row index and snapshots

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 7. Row-oriented MVCC index

The index is PostgreSQL-like: each `VirtualId` has a version chain of targets,
and a snapshot resolves a row by choosing the newest version
whose `CommitId` is visible to that snapshot.

### 7.1 Row table and version chains

Virtual IDs are allocated densely and monotonically. They index a stable paged
row table. A row version is addressed through a compact private index:

```cpp
using RowNodeId = std::uint64_t;
inline constexpr RowNodeId no_row_node = 0;

struct RowVersionNode {
    CommitId  commit_id;
    TargetId  target;
    RowNodeId previous;   // no_row_node at the chain tail
};

struct RowHead {
    std::atomic<RowNodeId> newest{no_row_node};
};
```

`RowNodeId` is an index into a private row-node arena, not an address:

```text
RowNodeId = (chunk_id << RowNodeChunk::shift) | slot_index

chunk_id   = RowNodeId >> RowNodeChunk::shift
slot_index = RowNodeId &  RowNodeChunk::mask
```

The arena owns fixed-size chunks:

```cpp
using ChunkId = std::uint64_t;

struct RowNodeChunk {
    static constexpr std::size_t capacity = 256;  // power of two
    static constexpr unsigned shift = 8;
    static constexpr std::size_t mask = capacity - 1;

    std::array<std::uint64_t, capacity / 64> free_bits;  // writer-only
    std::size_t live_nodes = 0;                          // writer-only
    std::array<RowVersionNode, capacity> nodes;
};
```

Index zero is the empty/tail sentinel, and the allocator never returns it
(the first allocation is one; if chunk zero is later reused, its slot zero is
skipped). This makes a zero-initialized `RowHead` an empty row and a
zero-initialized `previous` field a chain tail, without a separate engaged
flag. The arena
owns chunks through `std::deque<std::unique_ptr<RowNodeChunk>>` and exposes
them to readers through a stable, paged, non-owning chunk directory. The
directory uses the same publication strategy as the block directory. Chunks
are metadata, not payloads, and are released explicitly when their last node
is freed. The `unique_ptr` indirection is paid only when a chunk is allocated
or released; reader traversal resolves the chunk through the directory, not
through the owning deque. A reader that resolves a row node is traversing a
chain protected by its snapshot horizon, so the node's chunk cannot be
released during that traversal.
The directory page capacity is a power of two, so chunk and page projection
also use shifts and masks. The writer-only free-chunk list uses
`std::optional<ChunkId>` links, so it contains no raw pointers either.

`RowVersionNode` objects are immutable after publication, except for the
prune-time truncation of the retained tail's `previous` link described in
Section 12. `previous` links form a newest-to-oldest chain per virtual ID. A
tombstone is a node whose `target` is `no_target`; it hides older live
versions from snapshots at or after that commit.

The node deliberately duplicates the commit ID and target that also appear
in the diff record. Snapshot resolution must not depend on the retention or
compaction policy of the audit log: a diff record may be dropped by a history
prune while its row node is still required by an active snapshot. The row
node costs 24 bytes per changed row and buys an independently prunable,
directly traversable row chain.

The index form is not required by the algorithm; a direct address would save
the one dependent load used to turn `chunk_id` into a chunk reference. The
index is chosen deliberately because:

- the row head is one lock-free atomic integer, with no atomic optional
  reference and no platform-dependent non-lock-free fallback; a future
  multi-writer publish can use a single-word CAS;
- `RowVersionNode` is 24 bytes rather than 32 bytes, so row metadata and
  traversed cache lines stay smaller;
- no non-owning pointer or reference-wrapper lifetime is stored inside
  immutable history nodes;
- node chunks can be released independently of row heads and snapshots.

The cost is one stable-directory load per visited node. That directory page is
normally hot and row chains are short. If profiling later shows the load
dominates, the only way to remove it is to store a direct address; that is a
deliberate later tradeoff, not a correctness requirement.

`RowNodeId` is 64-bit to match `CommitId`, `VirtualId`, and `TargetId`. A
32-bit link would not shrink `RowVersionNode`: with 64-bit commit and target
fields, a 32-bit link is padded to 8-byte alignment, so the node is 24 bytes
either way. The only saving from a 32-bit link is therefore 4 bytes per row
head, at the cost of a global `2^32` row-version ceiling. The 64-bit default
removes that ceiling and keeps every identifier width uniform.

The cost is that `RowHead` is 8 bytes instead of 4, so a 64-byte line holds
eight heads rather than sixteen. On a 32-bit target where a 64-bit atomic is
not lock-free, the private `RowNodeId` width can be made a compile-time
policy; that is an explicit tradeoff, not a hidden assumption. On the
intended 64-bit targets, `std::atomic<RowNodeId>` is a single lock-free word.

The hive also owns a writer-only dense table:

```cpp
std::vector<TargetId> current_targets;  // no_target means absent
```

It lives outside `RowIndex`, so snapshots cannot observe it and its lifetime is
tied directly to the hive. This is not a projection copied per commit and it is
not the source of truth. The source of truth is `row_head[vid].newest`.
`current_targets` is a derived cache that gives the writer an `O(1)`
current-view scan and fast validation and rebasing. Snapshot reads never
consult it. The cache is accessed only under writer exclusion; in
multi-writer mode that means the short publication mutex.

If the cache is ever stale, it is rebuilt from the authoritative heads under
writer exclusion:

```cpp
// Writer-only. O(V); never called on the snapshot read path.
void rebuild_current_targets() noexcept {
    const CommitId head = head_commit_.load(std::memory_order_relaxed);
    for (VirtualId vid = 0; vid < current_targets.size(); ++vid) {
        RowNodeId id = row_head(vid).newest.load(std::memory_order_relaxed);
        while (id != no_row_node) {
            const RowVersionNode& n = node_arena_.at(id);
            if (n.commit_id <= head) break;
            id = n.previous;
        }
        current_targets[vid] =
            id == no_row_node ? no_target : node_arena_.at(id).target;
    }
}
```

The loop skips row nodes newer than `head_commit_`, so it is also correct if a
future fault leaves an unpublished but not-yet-recycled node at the front of a
chain.

The row heads use the same stable paged publication strategy as the block
directory. Growing the row table publishes a page; it never moves an existing
row head. A new row page is value-initialized, so every `RowHead` starts at
`no_row_node`.

### 7.2 Installing a commit

A commit has a throwing prepare phase and a `noexcept` publish phase. The
barrier between them is the first row-head release store.

```cpp
class CommitBuilder {
public:
    void prepare();                  // may throw
    void publish() noexcept;         // stores and anchor transfer only
    ~CommitBuilder() noexcept;       // returns nodes only if publish never started

private:
    std::vector<Diff> diffs_;
    std::vector<RowNodeId> new_nodes_;
    std::vector<RowNodeId> old_heads_;  // rollback/diagnostic journal
    bool publish_started_ = false;
    bool published_ = false;
};
```

The destructor must not recycle prepared nodes after `publish_started_`: a
reader may already have loaded a new row head and be about to follow its
`previous` link. Returning such a node to the free list would be an ABA bug.
This is another reason a failure after the publication barrier is fatal rather
than silently rolled back.

#### Prepare phase (may throw)

1. Normalize the transaction overlay into one `Diff` per touched
   `VirtualId`, sorted by `VirtualId`.
2. Grow the row-head table and `current_targets` to cover the largest
   touched `VirtualId`. New cache entries start as `no_target`.
3. Reserve capacity for every new row node, diff record, commit record, and
   deferred target release. This is where chunk allocation, directory page
   growth, and log growth happen. A newly allocated row-node chunk is fully
   initialized and published through the directory before any row head can
   reference a node in it.
4. Allocate one unpublished `RowVersionNode` per diff and fill it with
   `{commit_id, current_target, old_head}`. Record `old_head` in a
   preallocated rollback journal.
5. Validate each diff against the authoritative row head, not merely
   against the cache. The head's visible target must equal
   `diff.previous_target`. If the cache disagrees, rebuild that entry from
   the head; in debug builds, assert that it agrees.

Any exception in this phase leaves every row head unchanged. Prepared nodes
are returned to the arena by the commit builder's destructor.

#### Publish phase (`noexcept`)

1. Append the normalized diffs and the commit record into their reserved
   slots, then publish the new commit-store length.
2. For each diff, in `VirtualId` order, publish its row head:

   ```text
   row_head[vid].newest.store(new_node, std::memory_order_release)
   ```

3. For each diff, in `VirtualId` order, assign its cache entry:

   ```text
   current_targets[vid] = diff.current_target
   ```

4. Publish the new head commit:

   ```text
   head_commit_.store(commit_id, std::memory_order_release)
   ```

5. Give every installed row node its target anchor. A transaction-created
   target transfers the transaction's existing anchor; a target that predates
   the transaction (for example, a swap target) acquires one new view with a
   relaxed increment. Release anchors for intermediate versions that the
   normalization discarded. This step only adjusts view counts and enqueues
   targets for retirement; payload destruction happens later in the writer's
   drain.

No step in this phase allocates, constructs, or otherwise throws. The
transaction and its prepared nodes are marked committed only after the whole
phase succeeds. The cache may transiently lead the published head between the
row-head stores and the final head-commit store, but the writer/publication
lock is held for the whole interval, so neither a snapshot nor another writer
can observe that intermediate state.

In debug builds, assert for every touched row that
`current_targets[vid] == newest_visible_target(vid)`. The row head is
authoritative; the cache is expected to agree but can be rebuilt if it does
not.

Old snapshots ignore nodes whose commit is newer than their viewpoint. New
snapshots cannot see the commit until its commit record and head are
published.

#### Exception synchronization

`row_head` and `current_targets` cannot diverge because of a recoverable
exception:

- a prepare failure happens before the first row-head store, so no row is
  visible to any snapshot and every prepared node can be returned;
- the publish phase is `noexcept`, and its only mutations are scalar stores
  and pre-reserved trivially copyable writes;
- `current_targets` is not authoritative, so even an external fault that
  corrupts it is repaired by `rebuild_current_targets()` rather than by
  replaying history.

An exception escaping after the first row-head store is a fatal internal
error and calls `std::terminate`. Rolling back a partially published row head
would require a reader grace period: a reader may have loaded the new head
and been preempted before following its `previous` link, so the new node
cannot be recycled immediately. The design avoids that cost by making the
publication window non-throwing. A future recovery mode can add a quarantine
for new nodes, but it is not needed for the normal exception-safety contract.

Undo and redo are viewpoint changes. They do not apply inverse payload
operations or rebuild an index.

### 7.3 Snapshot resolution

A `Snapshot` stores a `CommitId`, non-owning references to `RowIndex` and
`VersionStore`, and an active-snapshot guard. It does not store a full
projection table and does not extend the store's lifetime.

Resolving a virtual ID at commit `C` is:

```text
resolve(vid, C):
    id = row_table[vid].newest.load(acquire)
    while id != no_row_node:
        node = node_arena.at(id)
        if node.commit_id <= C:
            break
        id = node.previous
    if id == no_row_node || node.target == no_target:
        return nullopt
    return version_store.resolve(node.target)
```

For the current snapshot, the newest node normally has
`commit_id <= C`, so each row lookup is `O(1)` after the stable row-table
load. For an older snapshot, lookup walks only that row's changes after `C`.
It never scans the global diff log and never copies a table.

Traversal follows a `previous` link only while the current node's commit is
newer than `C`. Those nodes lie above the active snapshot horizon and cannot
be reclaimed while the snapshot guard is alive. The first node at or below
`C` is where traversal stops; its own `previous` link is never needed by this
snapshot.

The resulting complexity is:

- current point lookup: `O(1)` row-chain step plus target resolution;
- historical point lookup: `O(changes to that row since C)` plus target
  resolution;
- snapshot acquisition: `O(1)` plus active-snapshot registration;
- historical full scan: `O(V + total skipped row versions)`.

This is deliberately not a Git-style checkout. If a workload later performs
many full scans of the same old commit, an opt-in materialized view can cache
the result of that scan. That cache is not required by the core model and
should not be built speculatively.

### 7.4 Why row chains fit better

| Approach | Commit cost | Current read | Old point read | Main tradeoff |
| --- | --- | --- | --- | --- |
| Full table copy | `O(V)` | `O(1)` | `O(1)` | prohibitive commit cost |
| Persistent trie | `O(k log V)` | `O(log V)` | `O(log V)` | expensive node/root infrastructure |
| Global current table + global diff scan | `O(k)` | `O(1)` | `O(changes since C)` | old point reads scan unrelated rows |
| Per-row version chains | `O(k)` | `O(1)` typical | `O(changes to that row)` | old full scans revisit each row |

The row-chain model matches the expected workload: recent and current views
are cheap, historical point reads pay only for the row being read, and no
global index structure is allocated per commit.
