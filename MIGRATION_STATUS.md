# SFS2 source consolidation — 6 November 2026

Every file in the central `Filesystems` SFS2 source tree at main commit `a253fb724ce1fd760a361219281effb6abd71367` is now present in this independent repository. Its 4 existing identical canonical files were preserved; native Linux VFS adapter, original Aaru/Amipart reference implementations, and Windows adapter documentation were imported with source attribution unchanged.

The SFS2 native Linux `Makefile` and `core_bridge.c` compile the in-repository SFS2 core and Linux adapter; the previous `kernel.json` inclusion of central SFS and a historical central source revision was removed. `sfs2.ko` now builds from this repository's source, while `SFS2.so` continues to use the independent module source.

This establishes local source ownership, not yet end-to-end data safety, ABI, or kernel qualification; preserve and execute runtime regression tests before any release or removal of the central CI/build references.
