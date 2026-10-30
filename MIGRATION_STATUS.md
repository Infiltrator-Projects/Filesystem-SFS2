# SFS2 independent migration — 2026-10-30

The format-specific SFS2 canonical core and design were copied without modification from Filesystems commit ca2fa1210c3c1f2b6f52cd15b3da6dfa83d453ad. SFS2 is **not** SFS0 with its version field changed: its root signature, structure version, object records, extent record sizes and checksums differ. Do not mistake this for the earlier unrelated historical SFS2 label.

The combined SFS0/SFS2 Defragmenter native engine is retained in Defragmenter and staged once under `Filesystem-SFS/migration/defragmenter/native/` as a reference. It has NOT been imported as a second divergent copy here. The Linux kernel adapter remains under Filesystems during transition. This repository does not yet produce a tested SFS2.so; integrate independently against Common ABI and preserve distinct on-disk rules before cutover.
