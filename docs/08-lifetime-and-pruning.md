# Lifetime, reclamation, and pruning

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 11. Target lifetime and reclamation

Each live target's anchor count is the corresponding entry in the block's
`views` array. A target is anchored by:

- every row-version node whose target is that version;
- an uncommitted transaction that constructed it;
- any other temporary owner that may expose it to a reader.

These rules govern reclamation while a generation is current. The hive and its
snapshot guards share ownership of the generation object that contains the
`VersionStore` and `RowIndex`; a retired generation is destroyed only after its
last snapshot handle is released.

A row-version node created by a commit owns one target view until that node is
pruned. The newest node for each virtual ID therefore keeps the current target
alive. Copying a `Snapshot` shares its active-snapshot guard and does not touch
any `views` entry. Row-chain traversal needs no atomic increment because the
snapshot guard prevents the visited nodes from being pruned.

For a `swap`, the two new row nodes point at existing targets, so each node
acquires one new relaxed view on its target; no payload is constructed or
destroyed. For an insert or replace, the transaction's staged anchor on the
new target is transferred to the installed row node instead of being
incremented again.

View acquisition has a deliberate precondition: the caller must already hold a
live path to the node or transaction stage. Under that condition the increment
is a relaxed `fetch_add`. Release is a `fetch_sub` with acquire/release
ordering.

When release observes the previous count as 1, that target has no views left.
The releasing thread does not destroy `T` or mutate block bitmaps directly. It
publishes the target ID to the store's retirement queue. The single writer
drains that queue under the store lock, verifies that `views` is still zero,
destroys the `T` in place, sets the free bit, and decrements the block's
`live_versions` count. Deferred destruction keeps arbitrary `T` destructors
and block metadata writes off reader threads.

If `live_versions` then becomes zero, the entire block is a release candidate;
no `T` remains in it, so returning the block to the allocator moves nothing.
The writer alone performs block release.

`SnapshotRegistry` tracks the commit IDs of all live snapshot handles.
Duplicate snapshot copies share one guard and one registered commit. The
oldest registered commit is the row-history horizon: the writer may not prune
the last row-version node at or below that commit for any virtual ID.

Row-version nodes are metadata, not payloads. Pruning a node releases its
target view, clears its bit in the row-node chunk, and releases the whole
chunk when its `live_nodes` count reaches zero. A snapshot never traverses a
node that the horizon has already allowed to be reclaimed.

This gives the desired MVCC lifetime rule:

- a new commit can add mappings without endangering old versions;
- removing or replacing an element does not destroy the old version while an
  older row-version node still maps to it;
- destroying a `Snapshot` releases its horizon registration;
- pruning a commit removes row versions older than every active snapshot
  horizon.

Target locations can be recycled only after every row-version anchor and
transaction view has been released. A target reached from a live row node
therefore cannot be confused with a later occupant of the same slot.

Reclamation is driven by target release, not by walking every cell. A block
becomes releasable as soon as its compact live counter reaches zero, even when
the block is in the middle of the directory.

## 12. History pruning

History retention is controlled by dropping commit metadata, diff ranges, and
obsolete row-version nodes:

```cpp
void prune_before(CommitId first_retained);
void clear_history();
```

`prune_before(c)`:

- retains commit `c` and later commits in the commit log;
- drops older commit metadata and diff records;
- computes the row horizon from the requested retention boundary and the
  oldest active snapshot, when one exists;
- for each virtual ID, keeps the newest node at or below that horizon and all
  younger nodes, dropping older nodes;
- returns dropped row nodes to the row-node arena and releases empty chunks;
- drains retired targets and releases every block whose live count reached
  zero;
- trims the block directory's released tail and shrinks it when empty;
- rebuilds `current_targets` from the retained row heads after the drop;
- does not mutate any payload.

The newest retained node at or below the horizon becomes the logical tail of
that row chain. Nodes older than it must never be traversed by a live
snapshot. The implementation may clear the retained node's `previous` link
under writer exclusion before retiring the older nodes, or retain those
metadata nodes until the row index is destroyed; either way the link is not
part of the visible read path.

Implementation note: snapshot traversal still reads the `previous` link of
nodes whose commit is newer than the snapshot. If an active snapshot is older
than every node in a row chain, it can walk the whole chain even though it
resolves no value. The first implementation therefore treats such a chain as
pinned instead of truncating its tail. When the horizon does find a retained
node at or below the snapshot commit, that node's `previous` link is outside
the visible traversal path and can be cleared before older nodes are released.

Snapshots created before pruning remain valid as long as the snapshot guard
and the hive are alive, because the row nodes they can traverse are retained.
Looking up a pruned commit by ID through `hive.snapshot(old_id)` is no longer
possible once its commit metadata is gone.

`clear_history()` keeps the newest row node for each virtual ID as the new
baseline, plus any older node still required by an active snapshot horizon,
drops unreachable row nodes, commit records, and diffs, and keeps the current
`VersionStore` alive. `clear()` is a destructive exclusive reset: it
invalidates all outstanding snapshots by replacing the snapshot registry,
retires the current `VersionStore` and `RowIndex` generation, clears the
writer-only `current_targets` cache, and installs a fresh empty generation.
Removing the hive's generation reference is safe while snapshot readers are
active because each guard owns the generation it reads. The retired
generation, including every remaining payload, is destroyed when its last
snapshot handle is released. No caller-side quiescence is required.

A commit from an older checkout first discards the redo tail. Its future row
nodes are detached from the current row heads but not released immediately:
they and their target anchors remain quarantined until no snapshot can still
be traversing them. The quarantine is reclaimed on a later writer operation
once the snapshot registry is empty. An active snapshot above the checkout
makes the discard fail before any row head changes.

`prune_before(c)` requires `c` to be at or below the current checkout, because
the checkout must remain addressable. `clear_history()` discards the redo tail
first and keeps the checkout as its baseline.
Undo stops at the oldest retained commit. Going below the original first
commit to the empty origin is valid; trying to undo below a pruned or cleared
retention floor throws and leaves the checkout unchanged.

`prune_before` and `clear_history` end with the same explicit reclamation
pass. A block still referenced by a retained row node or a live snapshot is
kept; a zero-live block is returned immediately. This is what makes shrinking
observable rather than dependent on a library container's unspecified capacity
policy.

The split is deliberate: `clear_history()` prunes metadata while retaining the
store, whereas `clear()` retires the whole generation. Its storage is destroyed
immediately when no snapshot guard owns it, or when the last outstanding guard
is released.
