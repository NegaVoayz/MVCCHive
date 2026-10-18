# History model

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 4. The history diff

The internal normalized history record is:

```cpp
struct alignas(32) DiffRecord {
    CommitId   commit_id;
    VirtualId  virtual_id;
    TargetId   previous_target;
    TargetId   current_target;
};

constexpr TargetId no_target = std::numeric_limits<TargetId>::max();
static_assert(sizeof(DiffRecord) == 32);
static_assert(std::is_trivially_copyable_v<DiffRecord>);
```

`DiffRecord` never appears in the public API. Its physical targets are needed
for commit validation and row-chain publication, but exposing them would leak
the storage projection and make the logical contract depend on an internal
layout.

`no_target` is the target-domain `npos` sentinel and is never accepted as a
live target. `no_row_node` is the analogous sentinel for row chains.

History inspection returns a copied public value:

```cpp
enum class DiffKind : std::uint8_t {
    insert,
    update,
    remove,
};

struct Diff {
    CommitId commit_id;
    VirtualId virtual_id;
    DiffKind kind;
};

static_assert(sizeof(Diff) == 24);
static_assert(std::is_trivially_copyable_v<Diff>);
```

The tag is derived from target presence, using the same Insert/Update/Remove
vocabulary as the history model:

- `insert`: `previous_target == no_target`, `current_target != no_target`;
- `update`: both targets are live;
- `remove`: `previous_target != no_target`,
  `current_target == no_target`.

The public record contains no target, block, slot, row-node, or generation
field. A caller that needs values resolves them through snapshots using
`commit_id` and `virtual_id`. This keeps the export stable if the physical
storage representation changes.

Diff-log blocks are allocated with 64-byte-aligned bases, so the internal
32-byte record places two records in a typical 64-byte cache line. This does
not change point-read complexity, but it removes the optional-state words and
improves diff-log scans and pruning.

For a normalized commit, there is at most one diff per `VirtualId`. If a
transaction touches the same virtual ID several times, those edits collapse
into one diff whose `previous_target` comes from the transaction's parent
snapshot and whose `current_target` is the transaction's final target for that
ID.

The meanings are:

- Insert: `no_target -> target(new_version)`.
- Remove: `target(old_version) -> no_target`.
- Update: `target(old_version) -> target(new_version)`.
- Swap of `a` and `b`: two diffs in one commit:
  - `a: target_a -> target_b`
  - `b: target_b -> target_a`

Swapping therefore creates no payload version and performs no payload
operation. The two virtual IDs exchange the targets they project to.

The internal `previous_target` is not merely an undo hint. It is an invariant
check against the row chain at the transaction's base. A commit is valid only
if, for every internal diff:

```text
resolve(parent_snapshot, diff.virtual_id).value_or(no_target)
    == diff.previous_target
```

This makes corruption and accidental out-of-order commits observable.

For example:

| Commit | Operation | Projection after commit | New target |
| --- | --- | --- | --- |
| 1 | `emplace("A")` as `V1` | `V1 -> T1` | `T1 = "A"` |
| 2 | `emplace("B")` as `V2` | `V1 -> T1`, `V2 -> T2` | `T2 = "B"` |
| 3 | `replace(V1, "A2")` | `V1 -> T3`, `V2 -> T2` | `T3 = "A2"` |
| 4 | `swap(V1, V2)` | `V1 -> T2`, `V2 -> T3` | `no_target` |

The corresponding diff log is:

```text
(1, V1, no_target, T1)
(2, V2, no_target, T2)
(3, V1, T1,        T3)
(4, V1, T3,        T2)
(4, V2, T2,        T3)
```

A snapshot at commit 3 reads `V1 = "A2"`, `V2 = "B"`. A snapshot at commit 4
reads `V1 = "B"`, `V2 = "A2"`. Both snapshots refer to the same three
immutable targets; only their visible row-version nodes differ.

## 8. Commit store

The commit store contains:

```cpp
struct Commit {
    CommitId id;
    CommitId parent;
    std::size_t first_diff;
    std::size_t diff_count;
};
```

The diff log is append-only for the retained history window. Commit IDs start
at 1; commit 0 means "before the first retained commit" where that distinction
is needed.

The append-only commit and diff logs use fixed-size blocked storage rather than
a monolithic reallocating vector. The next block is prepared before a commit
crosses its current block boundary, so retained history is never copied during
publication.

For the initial implementation the timeline is linear:

- a transaction extends the current head;
- historical snapshots are read-only;
- a new commit does not rewrite an old row-version node;
- branching is out of scope: row-version chains encode one linear commit
  history and would need a separate merge or fan-out representation.

The commit record owns the diff range for that commit. A snapshot resolves
against row-version chains filtered by the commit ID; it does not need a
per-commit index root.
