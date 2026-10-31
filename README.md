# Filesystem-SFS2

Independent canonical SmartFilesystem **SFS\\2** implementation.

The CMake build produces `SFS2.so` and the CTest suite validates a strict,
read-only SFS2 identity/geometry interface from the canonical SFS2 core.
`SFS2.so` is distinct from `SFS.so`, which implements the original SFS0
format and currently contains legacy combined-engine code.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The initial shared library is **read-only**; no defragmentation, recovery or
formatting capability has been qualified or advertised. See
[MIGRATION_STATUS.md](MIGRATION_STATUS.md) and the detailed format
[design](native/filesystems/sfs2/DESIGN.md).
