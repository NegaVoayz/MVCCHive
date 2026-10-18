# Version storage

> Part of the [MVCCHive design index](README.md). Section numbers are preserved from the original design for stable cross-references.

## 6. Version storage

### 6.1 Stable SoA blocks

`T` is not wrapped in a per-version node. Its storage, view count, and free-slot
state use separate arrays inside one block. The block capacity is not a fixed
constant: it is derived at compile time from `sizeof(T)` so that a block holds
a similar payload byte budget for every instantiated type:

```cpp
template<class T>
struct BlockLayout {
    static constexpr std::size_t payload_budget = 16 * 1024;
    static constexpr std::size_t requested_slots =
        payload_budget / sizeof(T);

    static constexpr std::size_t capacity =
        requested_slots == 0 ? 1 : std::bit_floor(requested_slots);

    static_assert(std::has_single_bit(capacity));

    static constexpr unsigned shift = std::countr_zero(capacity);
    static constexpr std::size_t slot_mask = capacity - 1;
};

template<class T>
struct VersionBlock {
    using Layout = BlockLayout<T>;

    static constexpr std::size_t free_words =
        (Layout::capacity + 63) / 64;

    BlockHeader header;  // live_versions and a free-word search hint

    // SoA metadata: free state and live row-version views.
    std::array<std::uint64_t, free_words> free_bits;
    std::array<std::atomic<std::uint32_t>, Layout::capacity> views;

    // SoA payload: raw storage only, no per-version metadata between values.
    alignas(T) std::byte payload[Layout::capacity * sizeof(T)];
};
```

This is a block-local structure-of-arrays layout:

- `payload` is one contiguous byte region with stride `sizeof(T)`;
- `free_bits` records empty slots without a per-cell link word;
- `views` is the atomic refcount array used by row-version nodes and
  transactions;
- `header` contains only block-wide counters and search hints.

The block capacity is sized from `sizeof(T)` rather than using one fixed slot
count:

- `payload_budget` is a compile-time target for payload bytes, initially
  `16 KiB`;
- `capacity` is the largest power of two no greater than
  `payload_budget / sizeof(T)`;
- a type larger than the budget gets `capacity == 1`;
- `shift` and `slot_mask` come from that capacity.

For the expected type-size range, `T` from 8 B through 392 B maps to:

| `sizeof(T)` | Requested slots | Block capacity | Shift | Payload bytes |
| --- | --- | --- | --- | --- |
| 8 | 2048 | 2048 | 11 | 16 KiB |
| 64 | 256 | 256 | 8 | 16 KiB |
| 128 | 128 | 128 | 7 | 16 KiB |
| 392 | 41 | 32 | 5 | 12.5 KiB |

The layout is compile-time per `MVCCHive<T>` instantiation. Different payload
types can therefore have different block capacities and block shifts; all
targets within one hive use that hive's `BlockLayout<T>`.

The budget applies to payload bytes. Total block allocation also includes the
SoA view counters, free bitmap, and block header, so `sizeof(VersionBlock<T>)`
varies with `T` even when the payload budget is constant. This is intentional:
a fixed total-byte size would either waste most of a block for large `T` or
spend too much metadata per payload byte for small `T`. The payload budget is
the tuning knob; if a hard total-byte cap is ever required, it can be computed
with a constexpr search over power-of-two capacities instead of a single
division.

The 8 B to 392 B range is the sizing target, not a hard type restriction.
Types larger than `payload_budget` still work with one slot per block; their
block is simply larger than the budget.

When capacity is smaller than 64, `free_bits` still has one 64-bit word. Bits
outside `[0, capacity)` are initialized to zero and ignored by allocation and
release scans.

The steady-state per-slot metadata is one 32-bit view counter and one free bit.
There is no per-version pointer, tag byte, generation word, or intrusive link
embedded beside the payload.

A set bit in `free_bits` means "available"; allocation finds the first set bit
using the header's search hint and `std::countr_zero`. No per-slot free-list
pointer is required.

The payload slot for cell `i` starts at:

```cpp
std::byte* address = block->payload + i * sizeof(T);
```

Because each `BlockLayout<T>::capacity` is a power of two, a global cell
ordinal projects to its block and slot without division:

```cpp
std::size_t block_index = cell_index >> BlockLayout<T>::shift;
std::size_t slot_index  = cell_index & BlockLayout<T>::slot_mask;
```

The free-word count is also a shift, and the payload offset is a shift by
`std::countr_zero(sizeof(T))` when `sizeof(T)` is a power of two; otherwise it
is a single multiply by the compile-time constant `sizeof(T)`.

`sizeof(T)` is a multiple of `alignof(T)`, the block is aligned for
`VersionBlock`, and `payload` is `alignas(T)`. Every slot therefore has the
alignment required by `T`. There is no `VersionCell<T>` wrapper, no generation
field beside `T`, and no metadata inserted into the payload stride. Padding
introduced by `T` itself remains exactly `T`'s ABI size.

The payload array may additionally be cache-line aligned to keep metadata
traffic off the first payload lines. This is an alignment choice, not a
per-version layout change.

A block is one allocation containing the metadata arrays and raw payload
storage. It contains no constructed `T` array. Adapting the capacity to
`sizeof(T)` while keeping a power of two preserves branch-free block
projection.

The reader-facing block directory is a stable, paged table. It is separate
from the owning sequence so that a writer can grow or reclaim storage without
invalidating snapshot lookups:

```cpp
struct DirectoryPage {
    // Non-owning reader cache.
    std::atomic<std::optional<std::reference_wrapper<VersionBlock<T>>>>
        blocks[DirectoryPageCapacity];
};

class BlockDirectory;  // private stable two-level page table
```

`DirectoryPageCapacity` is also a power of two. A block index becomes
`block_index >> PageShift` and `block_index & PageMask` for the page and entry.
Both block and page projections therefore use shifts and masks only.

Directory pages are allocated once and never relocated. The top-level root is a
stable allocation sized from the private block-index width. Growing the
directory publishes a new page reference into an atomic root entry; it does
not reallocate the pages a reader may already be using. A block reference is
published with release semantics after the block is fully initialized. Readers
load it with acquire semantics.

`VersionStore` owns the blocks through a writer-only owning sequence:

```cpp
std::deque<std::unique_ptr<VersionBlock<T>>> owned_blocks;
```

`std::deque` is used only as the allocator/owner. It is not read by snapshot
threads. `unique_ptr` gives the required explicit block destruction without
moving `VersionBlock<T>` or its payload, and readers use the atomic reference
table instead. An empty directory entry is `std::nullopt`; an engaged entry
holds a non-null `std::reference_wrapper<VersionBlock<T>>`. The `unique_ptr`
indirection is therefore paid only when the
writer allocates, releases, or reuses a block, not on the snapshot read path.

The free-block list stores `std::optional<BlockIndex>` links. Block ownership
and free-list metadata are writer-only and remain private to the directory.

The hot read path is:

```text
TargetId
  -> decode block index and cell index
  -> stable directory page entry
  -> VersionBlock&
  -> VersionBlock::payload + cell_index * sizeof(T)
  -> const T&
```

There is one direct block lookup and no ownership-wrapper indirection. A target
reached from a live row-version node is guaranteed to be anchored, so its slot
cannot have been freed. The writer-only free bitmap is not consulted on the
snapshot payload path.

The directory is metadata only and never owns payloads inline. Moving or
reusing directory metadata does not move a payload block or a `T`. A cell
address remains stable from construction until destruction.

### 6.2 Explicit block release and structural shrink

A block has a `live_versions` count. During normal operation, a block may be
released only when that count reaches zero. Final `VersionStore` teardown is
the sole exception: no snapshot may remain valid, so all remaining blocks are
destroyed regardless of their view counts. Releasing a block normally:

1. verifies that every slot is marked free;
2. publishes the empty state to the reader directory with a release store;
3. calls `reset()` on the owning `std::unique_ptr`, destroying the block's raw
   payload and metadata storage;
4. links the slot into the free-block list using `std::optional<BlockIndex>`.

Dropping history releases old row-version nodes. When that makes the last
anchor of a target disappear, the `T` at its payload offset is destroyed in
place, its free bit is set, and its block's `live_versions` count is
decremented. If the count reaches zero, the block is released immediately or
marked for release by the store's reclamation step. The block does not have to
be at the front or back of the directory. This is why block ownership uses
`unique_ptr` instead of storing movable `VersionBlock<T>` objects directly in
a `std::deque`.

After a history drop, `VersionStore::reap_empty_blocks()`:

- drains deferred target releases;
- releases every block whose `live_versions` is zero;
- pops trailing empty entries from the owning deque so its internal block
  storage can be returned;
- trims a null-only directory suffix;
- resets and optionally shrinks the directory when no blocks remain.

This is the automatic shrink step for history dropping. It returns payload
block storage even when empty blocks are scattered among live blocks.

There is deliberately no block compaction. Merging two partially occupied
blocks would move `T` objects, which violates the central no-move rule. Such
blocks remain until their last live version goes away.

Construction and destruction use:

```cpp
std::byte* storage = block->payload + index * sizeof(T);

// Placement construction into the selected payload slot.
T* object = std::construct_at(
    reinterpret_cast<T*>(storage),
    std::forward<Args>(args)...);

std::destroy_at(object);
```

The payload region is a byte array, not a `T[BlockLayout<T>::capacity]` object
array. This is important for two reasons:

- no `T` is default-constructed when a block is allocated;
- growing or releasing blocks never invokes a `T` move or copy.

The SoA split also means a metadata-only operation, such as finding or
releasing a slot, does not pull unrelated `T` bytes through the cache.

### 6.3 Target identity

A `TargetId` is location only:

```text
TargetId = block_id << BlockLayout<T>::shift | slot_index

block_id   = TargetId >> BlockLayout<T>::shift
slot_index = TargetId & BlockLayout<T>::slot_mask
```

All non-reserved bits are available for the cell location. There is no epoch.
The top block index is reserved, so no allocated `(block_id, slot_index)` pair
can form `no_target`.

`TargetId` zero is a valid live location (block 0, slot 0). The sentinel is
the all-ones value, not zero.

`no_target` is the all-ones 64-bit value.
The block allocator enforces the reservation structurally:

```cpp
static constexpr std::uint64_t max_block_index =
    (no_target >> BlockLayout<T>::shift) - 1;
static_assert(((max_block_index << BlockLayout<T>::shift) |
               BlockLayout<T>::slot_mask) < no_target);
```

The directory never issues a block index above `max_block_index`; the top
block index is reserved for the sentinel. Consequently every live target
satisfies

```text
(block_id << BlockLayout<T>::shift | slot_index) < no_target
```

so `no_target` cannot collide with any live version, regardless of
how many blocks or slots are used. The reservation is checked at block
allocation time, not assumed from the practical unlikelihood of reaching the
top of the address space.

Target identity across time comes from the trace, not from the target word:

- a `RowVersionNode` carries `CommitId`;
- an internal `DiffRecord` carries both `CommitId` and `VirtualId`;
- a `Snapshot` filters row nodes by its own `CommitId`.

The same physical location may therefore represent different versions in
different time ranges. That is not an alias, because the commit trace chooses
which row-version node is visible before the target is decoded.

The liveness rule makes location-only targets safe:

- a row-version node anchors its target until that node is pruned;
- a slot is never returned to the free bitmap while its view count is non-zero;
- a snapshot horizon prevents a row node visible to a live snapshot from being
  pruned;
- API operations never accept a bare target token as a capability to resolve
  payload storage.

The target is consequently a private location in the retained trace, not a
globally unique serial number. The accompanying `commit_id` and `virtual_id`
supply historical identity. Public `Diff` values expose those logical IDs and a
change tag, never the target.

Removing the epoch also removes the per-slot generation array and directory
generation array. A 64-bit target now supports nearly the full 64-bit cell
location space, with one top block index reserved for `no_target`; the sparse
paged directory allocates metadata only for blocks that are actually used,
not for the full 64-bit address space.

If globally unique version IDs are needed later, they should be a separate
trace-level identity (for example based on creation commit plus target), not
bits stolen from the physical target index.

### 6.4 Immutability and access

Published versions are logically immutable. The API returns `const T&`, or an
`std::optional<std::reference_wrapper<const T>>` for fallible lookup, never a
mutable reference. Mutation is expressed by constructing a new version and
prepending a row-version node that maps the virtual ID to it.

This is what allows old snapshots to remain unchanged without copying `T`.
