# Test plan

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 16. Test plan

### Compile- and type-level tests

- A type with deleted copy and move constructors can be inserted and replaced
  through `emplace` arguments.
- A non-default-constructible type works.
- A type with `alignas(64)` works.
- A type with an overloaded `operator&` works; internals use
  `std::addressof` where necessary.
- A type whose move constructor aborts if called proves that no stored value is
  ever moved.
- A type whose address is recorded at construction proves that its address is
  stable across commits and snapshots.
- A slot-alignment test proves that every payload offset is aligned for
  `alignas(T)` types, including types whose alignment exceeds `alignof(max_align_t)`.
- A construction-count test proves that allocating a block constructs zero
  `T` objects and that only the selected payload slot is placement-constructed.
- A layout test or static assertion confirms payload stride is exactly
  `sizeof(T)` and free/view metadata lives in separate arrays.
- Static assertions verify that block and directory page capacities are powers
  of two, and randomized index tests compare shift/mask projection against
  ordinary division.
- Adaptive-layout tests instantiate payload sizes 8, 64, 128, and 392 bytes and
  verify capacities 2048, 256, 128, and 32 respectively, with payload bytes
  never exceeding the configured budget and `free_words` at least one.
- Row-node layout assertions verify that `RowVersionNode` is 24 bytes and
  trivially copyable, `RowHead` is one 8-byte atomic `RowNodeId`, and
  `no_row_node` is zero.
- A static assertion or platform probe verifies that `std::atomic<RowNodeId>`
  is lock-free on the supported targets; the design has no atomic optional
  reference fallback.
- Row-node index tests exercise the chunk boundary (`slot == chunk_capacity -
  1`), the null sentinel, and shift/mask decoding across chunk and slot.
- Static assertions verify that `Transaction::find` is `noexcept`, `at`
  returns `const T&`, and the overlay stores one `TargetId` per touched key.
- A public-surface test confirms `TargetId`, `VersionBlock`, `DirectoryPage`,
  and `BlockDirectory` are not nameable by a caller.
- Nullability tests verify that missing lookups return `std::nullopt`, erased
  public diffs carry `DiffKind::remove`, and successful fallible lookups
  return an engaged optional reference.
- Ownership tests prove that releasing a block resets exactly one owning
  `unique_ptr` and that the reader-directory entry is cleared before readers
  can observe the empty slot.
- Lifetime-ownership assertions verify that `MVCCHive` and snapshot guards
  share the generation containing `VersionStore` and `RowIndex`, while a
  `Transaction` remains a writer-locked child handle.
- Layout assertions verify that public `Diff` is 24 bytes and trivially
  copyable, internal `DiffRecord` is 32 bytes and trivially copyable, and
  instrumentation verifies two adjacent internal records occupy one
  64-byte-aligned diff-log line.
- Sentinel tests verify that `no_target` is rejected by every target resolver
  and never appears as a constructed payload version.
- Sentinel tests verify that no live target equals `no_target`. A live target
  zero (`block 0, slot 0`) remains valid. Block allocation above
  `max_block_index` fails during prepare rather than producing a colliding
  target; the test covers both `shift == 0` (capacity 1) and a normal
  power-of-two capacity.
- Index-range tests verify shift/mask decoding near the top of the 64-bit
  target space without allocating a dense directory for unused blocks.
- Index instrumentation verifies that a commit touching `k` IDs creates
  exactly `k` row-version nodes and does not copy the virtual-ID table.
- Snapshot tests instrument diff-log access and prove that `at`, `find`, and
  `for_each` never consult the diff log or the writer-only current-target
  table.

### Behavioral tests

- Multiple snapshots of the same virtual ID expose different immutable
  targets.
- Current-view point reads touch only the newest row node and do not walk old
  versions.
- Historical point reads traverse only the requested row's chain after the
  snapshot commit, not unrelated rows or the global diff log.
- Historical full scans produce the same model state as point-wise resolution
  at every virtual ID.
- Updating and removing do not alter older snapshots.
- Swap changes only current targets and row nodes; it constructs no version.
- Same-commit coalescing produces one diff for multiple edits to one ID.
- Transaction rollback destroys only transaction-created versions.
- Undo/redo round-trip tests verify that `head()` follows the checkout while
  `snapshot(id)` can still inspect the retained redo tail.
- Undo from the original first commit reaches the empty origin; undo from a
  fresh empty hive returns false.
- Undo at a pruned or `clear_history()` retention floor throws
  `std::logic_error` and leaves the checkout unchanged. This includes a
  `clear_history()` baseline whose commit ID is 1.
- A replace after undo publishes a new commit at the checkout's next ID,
  removes the old future diffs, and makes `redo()` return false.
- An empty commit after undo preserves the redo tail.
- Caller-supplied commit IDs may be sparse and can align multiple hives on the
  same checkout ID; an explicit empty commit advances an otherwise unchanged
  hive.
- An explicit ID at or below the hive's high-water mark throws
  `std::invalid_argument`, leaves the checkout unchanged, and leaves the
  transaction active for retry.
- Sparse commit IDs resolve correctly across snapshot lookup, undo, redo, and
  pruning, including gaps that are not commits.
- An active snapshot above the checkout rejects a tail-discarding commit before
  any row head changes; releasing that snapshot allows a later commit to
  proceed.
- A snapshot at or below the checkout remains valid and readable after a
  tail-discarding commit.
- `prune_before` refuses a first-retained commit above the checkout.
- Transaction `at`/`find` see the transaction's own insert, replace, erase,
  and swap results; untouched IDs resolve through the fixed parent snapshot.
- An overlay-hit `at` performs no row-chain walk and no diff-log access;
  instrumentation verifies a tip-checkout parent hit touches one row node.
- An old-parent transaction walks only the requested row's changes after its
  parent commit, not unrelated rows.
- `at` throws `std::out_of_range` for erased or missing rows, while `find`
  returns `std::nullopt` and remains `noexcept`.
- A reference obtained before a later transaction write remains valid and
  still names the old target; a fresh `at` returns the new target.
- Overlay collision, growth, and erase-then-reinsert tests verify one entry per
  touched ID, no tombstones, and `O(k)` space proportional to touched IDs.
- Commit failure by injected allocation or constructor exception leaves the
  previous head unchanged.
- A prepare-phase fault injected after some row nodes are allocated leaves
  every row head and every `current_targets` entry unchanged, and returns the
  prepared nodes to the arena.
- A deliberate `current_targets` corruption is repaired by
  `rebuild_current_targets()`, and the rebuilt table equals a full scan of the
  authoritative row heads at the checkout.
- A fault-injection hook at the publication barrier proves that the publish
  phase performs no allocation and that a failure before the first row-head
  store is recoverable; a failure after the barrier is a fatal contract
  violation, not a silent partial commit.
- Snapshot handles remain valid after `prune_before` while their hive is alive.
- `snapshot(pruned_id)` is unavailable for a commit whose metadata was
  dropped.
- Physical cell reuse is refused while a row-version anchor or transaction
  still references the target, and target tokens are never accepted as
  capabilities outside the trace.
- After the last row-version anchor and snapshot horizon release a slot, a
  later version may reuse the same target index without affecting any live
  view.
- Dropping history and releasing all older snapshots reduces the number of
  allocated version blocks to the blocks still needed by live targets.
- An empty block in the middle of the directory is released and its index is
  safely reused without invalidating any live target.
- A retained snapshot prevents its blocks from being auto-shrunk.
- A lifetime test instruments `T` destructors, destroys the hive while a
  `Snapshot` object still exists, and verifies that the retired generation is
  destroyed when the last snapshot handle is released. `Snapshot::valid()`
  must return false immediately after hive destruction.
- A `clear()` test verifies that a fresh empty generation is installed,
  outstanding snapshot handles become invalid, and the retired generation is
  destroyed only after those handles are released.
- A concurrent-reader test races snapshot lookups with `clear()` and verifies
  that old payload storage remains alive until the snapshot handle is released.
- A teardown test with an active transaction verifies that `~MVCCHive` waits
  for the writer lock and does not destroy the store underneath a live
  transaction.
- Public diffs are ordinary copies and remain readable after `clear()`, but
  their commit and virtual IDs no longer resolve in the new generation. No
  public record contains a location that could be used as a capability.
- Directory growth and block release never invoke a `T` move or copy.
- Iterating or updating free-slot metadata does not read or write payload
  bytes.
- A concurrency stress test runs one committing writer with multiple snapshot
  readers, including readers retaining old commits while history is pruned and
  blocks are retired. It is also the ThreadSanitizer entry point on platforms
  that provide a TSAN runtime.
- Repeated concurrent final-view releases exercise the retirement queue and
  verify that no payload is destroyed while a reader can still reach it.
- Concurrent copies and destructions of snapshots sharing one guard preserve
  all observable values and eventually allow all unreachable blocks to shrink.
- Snapshot-horizon tests verify that old row nodes survive while a snapshot
  needs them and are pruned after the last matching guard is released.
- Row-node chunk tests verify that a chunk is not released while any snapshot
  can traverse a node in it, and that an empty chunk is released and its index
  reused only after the horizon passes.
- Blocked-vector truncation tests verify trailing-block release and safe reuse
  of a partially retained block.
- Row-lock tests cover same-row serialization, disjoint-row progress,
  canonical lock ordering for `swap`, rollback release, and stale-row abort.
- A visitor test holds an old snapshot while another transaction edits the same
  logical row; the visitor still observes its original target.
- Randomized operations are checked against a reference model of
  `CommitId -> VirtualId -> value`, including snapshots, pruning, history
  clearing, and destructive clear.
