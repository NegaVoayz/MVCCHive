# Operation semantics

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 10. Operation semantics

### 10.1 Emplace

1. Allocate a fresh `VirtualId`.
2. Construct a new version in an unused stable cell.
3. Add one transaction-local anchor to that target.
4. Record `virtual_id -> target` in the transaction overlay.
5. Return the virtual ID.

If the transaction rolls back, the version is destroyed and its cell is
recycled.

### 10.2 Replace

1. Require the virtual ID to be live in the transaction's current view.
2. Construct a new version in place.
3. Record the new target in the overlay.
4. Leave the old target untouched; older snapshots still project to it.

The normalized diff is `old_target -> new_target`.

### 10.3 Erase

1. Require the virtual ID to be live.
2. Record `virtual_id -> no_target` in the overlay.
3. Do not destroy the old target immediately. Older snapshots may still
   reference it.

The normalized diff is `old_target -> no_target`.

### 10.4 Swap

1. Require both IDs to be live.
2. Read their current targets from the transaction view.
3. Write `a -> target_b` and `b -> target_a` in the overlay.
4. Construct no version and touch no `T`.

The two diffs share one commit ID.

### 10.5 Commit normalization

Multiple transaction edits are collapsed to a deterministic diff set:

```text
for each touched virtual_id:
    previous = resolve(parent_snapshot, virtual_id).value_or(no_target)
    current  = transaction_final_value(virtual_id)
    if previous != current:
        emit DiffRecord{commit_id, virtual_id, previous, current}
```

The resulting diffs are sorted by `VirtualId`. This makes the log stable and
removes the need to interpret intermediate transaction states during replay
or audit.

Public inspection maps the internal record to
`Diff{commit_id, virtual_id, DiffKind}`: `previous == no_target` is `insert`,
`current == no_target` is `remove`, and two live targets are `update`. The
target values stay private.

If the normalized set is empty, `commit()` returns the parent snapshot and
publishes no commit.

### 10.6 Commit publication

The publication sequence is:

1. Validate the commit ID and transaction state.
2. Build the normalized diff records.
3. Grow the row table and `current_targets`, and reserve every node, log, and
   retirement slot.
4. Allocate every row-version node and record the old row heads in the
   rollback journal.
5. Validate each `previous_target` against the authoritative row head. Repair
   a stale cache entry from the head; reject the commit if the head's visible
   target does not match the diff.
6. Append the diff records and commit record into the reserved log slots.
7. Publish each new row head with a release store.
8. Update each touched `current_targets` entry.
9. Publish the new head commit with a release store.
10. Transfer staged target anchors into the installed row nodes and release
    the transaction's temporary anchors.

Steps 1-6 are the throwing prepare phase. If any allocation or validation
fails, the row heads, cache, and visible history are unchanged, and the
transaction can still be rolled back or retried.

Steps 7-10 are the `noexcept` publish phase. Rows installed in step 7 are not
visible to a snapshot at the old head because their commit ID is greater than
that snapshot's viewpoint. A new snapshot is not published until step 9.
Because no throwing operation occurs between steps 7 and 8, an exception
cannot leave the authoritative row head and the derived cache out of sync.
