# MVCCHive

C++23 MVCC hive design and implementation workspace.

Status: implementation started; M1 layout, M2 version storage, and M3 row
history are in place. Public transaction and snapshot work is in progress.

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
