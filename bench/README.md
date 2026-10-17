# Benchmarks

Build with:

```sh
cmake --preset release -DMVCC_HIVE_BUILD_BENCHMARKS=ON
cmake --build --preset release
```

`mvcc_hive_commit_history_benchmark` measures one `replace` commit through the
public `MVCCHive` API after building the requested amount of retained history.

`mvcc_hive_commit_log_storage_benchmark` isolates commit/diff log appends and
compares a monolithic vector, `std::deque`, and blocked vectors with several
block sizes.

Representative MinGW/GCC 14.2 Release measurements:

| Retained history | Exact vector | Blocked log | `std::deque` | Public commit |
|---:|---:|---:|---:|---:|
| 5,000 | 0.11 ms | 0.01 µs | 0.02 µs | 1.31 µs |
| 20,000 | 0.16 ms | 0.03-0.05 µs at a block boundary | 0.02 µs | 1.34 µs |
| 50,000 | 0.72 ms | 0.01 µs | 0.03 µs | 1.35 µs |

The public commit cost is flat in retained history. The blocked log avoids
whole-history copies, keeps 64 KiB of adjacent records per block, and allocates
the next block during prepare rather than during the `noexcept` publish phase.
