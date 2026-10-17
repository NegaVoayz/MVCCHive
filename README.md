# MVCCHive

C++23 header-only MVCC hive. Version 1.0.1.

The single-writer design is implemented: stable placement-constructed
payloads, per-row MVCC chains, immutable snapshots, transaction overlays,
normalized diffs, history pruning, automatic block and chunk reclamation, and
safe generation replacement for `clear()` and teardown.

Multiwriter row locks remain the explicit post-1.0 boundary. The current
writer model is one writer with any number of concurrent snapshot readers.

Version 1.0.1 fixes commit-log growth so a commit remains amortized `O(k)` in
the number of changed rows rather than reallocating retained history.

- [Design documentation](docs/README.md)
- [Implementation plan](docs/13-implementation-plan.md)

## Build

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Benchmarks are opt-in:

```sh
cmake --preset release -DMVCC_HIVE_BUILD_BENCHMARKS=ON
cmake --build --preset release
./build/release/bench/mvcc_hive_commit_history_benchmark
./build/release/bench/mvcc_hive_commit_log_storage_benchmark
```

The test suite covers layout and sentinels, placement storage, row history,
transaction lookup, pruning and reclamation, randomized reference-model
behavior, snapshot-reader concurrency, and alignment/address lifetime
guarantees.
