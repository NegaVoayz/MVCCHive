# MVCCHive

C++23 MVCC hive design and implementation workspace.

Status: implementation started; M1 through M5 are in place, including
snapshot-safe history pruning, target/block reclamation, and deterministic
clear operations.

- [Design documentation](docs/README.md)
- [Implementation plan](docs/13-implementation-plan.md)

## Build

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The current public header exposes the stable identifier and token types. The
internal layout layer is tested independently of storage allocation.
