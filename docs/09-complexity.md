# Complexity

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 13. Complexity

Let `V` be the number of virtual IDs and `k` the number of distinct IDs changed
by a commit.

| Operation | Expected cost |
| --- | --- |
| Snapshot acquisition | `O(1)` plus active-snapshot registration |
| Current `at` / `find` | `O(1)` row-head step, one node-chunk directory load, plus target resolution |
| Historical `at` / `find` | `O(changes to that row after the snapshot)`, one chunk-directory load per visited node |
| `Transaction::at` / `find`, overlay hit | `O(1)` expected hash lookup plus `O(1)` target resolution |
| `Transaction::at` / `find`, parent hit | `O(1)` when the checkout is the retained tip; otherwise `O(changes to that row after the checkout)` |
| Transaction overlay | `O(1)` expected insert/update time and `O(k)` space for `k` touched IDs |
| Emplace | `O(1)` commit work, plus one in-place `T` construction |
| Replace | `O(1)` commit work, plus one in-place `T` construction |
| Erase | `O(1)` commit work |
| Swap | `O(1)` commit work, no `T` construction |
| Commit with `k` changed IDs | `O(k)` row-version nodes |
| Commit-log append | `O(1)` per commit/diff using preallocated fixed-size blocks |
| Undo / redo checkout move | `O(V)` to rebuild the writer-only current-target cache |
| Commit after undo | `O(V + detached future row nodes + k)`; commit/diff logs truncate in place |
| History diff export | `O(k)` target-free `Diff` copies for the selected commit |
| Current full scan | `O(V)` |
| Historical full scan | `O(V + skipped row versions)` |
| Target resolution | `O(1)`: shift/mask decode, one block-directory load, one payload offset |
| History drop | `O(row nodes visited + released targets + released blocks)` |

Version construction and destruction are not payload-relocation operations;
they construct and destroy `T` in its final storage.

Block storage is returned eagerly when `live_versions` becomes zero. Directory
memory is metadata and may be shrunk independently. Partially occupied blocks
cannot be merged without relocating `T`.

The common current-view read performs no division, no modulus, no store lock,
and no per-read atomic increment. Atomic work is confined to row-head
publication, snapshot-horizon updates, and retirement.

Commit publication still performs no allocation. Commit records and diffs live
in `BlockedVector` logs with 2048-slot blocks (64 KiB for the 32-byte records).
Indexing is one shift and mask. If an append crosses a block boundary,
`CommitBuilder::prepare` allocates the next block; publication only writes
inside the already-allocated block. A blocked-vector boundary test guards this
contract.
