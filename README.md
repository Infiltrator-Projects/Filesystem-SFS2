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

## Installer files

- `/System/Filesystems/SFS2.so`
- `/System/Resources/Documentation/Filesystem-SFS2.pdf`
- `/System/Kernel/Filesystems/sfs2.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.
