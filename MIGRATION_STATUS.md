# SFS2 independent migration — 2026-10-31

SFS2 is distinct from SFS0 (SFS\\2, structure version 4, 27-byte object prefix,
16-byte extent leaves, and SFS2 checksum rules). This repository now builds
`SFS2.so` from its own canonical core and exports a versioned read-only
format probe/geometry API. The library rejects SFS0, inconsistent roots and
invalid checksums. It installs under `/System/Filesystems/SFS2.so`.

**Not yet qualified:** SFS2-specific file/extent catalogue, allocation,
formatter, fragmented Test Media, destructive Defrag/Growth Defrag and Recover.
The driver exports no mutation capability and must not be used to enable
destructive SFS2 operations in Defragmenter until separately tested against
independent SFS2 media. The historic combined source is retained under
`Filesystem-SFS/migration/defragmenter/native/` for reference, not linked here.

Next qualification milestone: move and specialise the SFS2-aware catalogue,
transaction and relayout code into this repository; add independent SFS2
fixtures, malformed-media/cross-version tests, crash/Stop/replay proof; then
introduce a separate SFS2 worker in Defragmenter. Do not silently dispatch
SFS2 to SFS0 or claim the read-only ABI provides a defragmentation engine.
