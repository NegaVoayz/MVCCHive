# Benchmarks

Build with:

```sh
cmake --preset release -DMVCC_HIVE_BUILD_BENCHMARKS=ON
cmake --build --preset release
```

`mvcc_hive_commit_history_benchmark` measures one `replace` commit through the
public `MVCCHive` API after building the requested amount of retained history.
It also measures repeated `undo()` followed by `replace().commit()`, which
discards and replaces the redo tail.

`mvcc_hive_commit_log_storage_benchmark` isolates commit/diff log appends and
compares a monolithic vector, `std::deque`, and blocked vectors with several
block sizes.

Representative MinGW/GCC 14.2 Release measurements:

| Retained history | Exact vector | Blocked log | `std::deque` | Public commit | Undo + replace |
|---:|---:|---:|---:|---:|---:|
| 5,000 | 0.11 ms | 0.018 µs | 0.006 µs | 1.67 µs | 5.03 µs |
| 20,000 | 0.16 ms | 0.019 µs | 0.047 µs | 1.67 µs | 4.87 µs |
| 50,000 | 0.73 ms | 0.016 µs | 0.033 µs | 1.85 µs | 5.65 µs |

The public commit cost is flat in retained history. The blocked log avoids
whole-history copies, keeps 64 KiB of adjacent records per block, and allocates
the next block during prepare rather than during the `noexcept` publish phase.
At small retained sizes `std::deque` can win the isolated append microbenchmark;
the blocked log's advantage appears as history grows and as block-boundary
allocations are amortized. Undo followed by a replacement commit is also flat:
the retained prefix is not copied, and the discarded tail is truncated in
place.
