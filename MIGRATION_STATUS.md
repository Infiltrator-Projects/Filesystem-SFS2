# SFS2 strict Common V2 migration — 11 November 2026

The independent SFS2.so now exports only `infiltrator_filesystem_api_v2`.
Its CORE PROBE and read-only MOUNT callbacks use Common V2 host-sector reads,
the repository's original SFS2 root codec and checksum validation, including
backup-root recovery and contradictory-root refusal. VOLUME QUERY exposes
verified block geometry. No format-specific compatibility symbol, pathname-based engine
entry point, raw writer, formatter or file-defrag callback is advertised.

This is a read-only identity/geometry implementation, not qualification of
namespace I/O, metadata updates, defragmentation, EFI or kernel runtime.
The independently owned Linux SFS2 source/adapter remains in this repository
and is not replaced by an OS-specific Common fallback.
