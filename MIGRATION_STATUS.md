# SFS2 Common V2 migration status — 21 November 2026

The installed SFS2.so has one Common V2 export and uses the source-owned
`native/filesystems/sfs2/core/` and `module/sfs2_reader.c` for native SFS2
root, checksum, allocation, namespace, directory and extent decoding.
The hosted and freestanding build profiles compile the same reader. The
freestanding ELF is embedded as `.infiltrator_boot` in the installed SO.

## Implemented, subject to qualification

- Common V2 PROBE, read-only MOUNT/UNMOUNT, STAT, LIST, READ and VOLUME.QUERY.
- Checked primary/backup roots, bitmap checksums, generation-safe sessions,
  transaction status, object containers, extent B-tree nodes and extent chains.
- Reject allocation bitmaps that identify live roots, bitmap blocks, object
  roots, transaction markers or extent roots as free, even when checksums
  have been recomputed.
- Host and freestanding V2 interface tests using disposable on-media images
  with files, directories, deliberately corrupted metadata, aligned I/O
  and source nonmutation.
- One `/System/Filesystems/SFS2.so` plus its PDF in hosted packages;
  no filesystem-specific kernel module or V1 runtime fallback.

## Not BOOT-qualified or released

Neither compiling the freestanding ELF nor hosted V2 tests prove BOOT
compatibility. The unmodified production BOOT `validate-module` must accept
the actual packaged SO, and OVMF must load a real EFI-stub kernel and
initramfs from a sufficiently large, independently created SFS2 volume.
Those firmware and release gates are still outstanding. No boot-safe
release is claimed and no live machine/ESP/NVRAM changes are involved.
