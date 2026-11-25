# Filesystem-SFS2

## 1. Purpose and format scope

SmartFilesystem SFS2 (SFS\\2), not SFS0. This repository owns the format-specific implementation of `SFS2.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

Native SFS2 allocation roots and redundant geometry must agree; reject inconsistent allocation roots before trusting the volume. The SFS2 reader and core remain format-specific.

## 3. Common V2 implementation and capabilities

The current provider declares probe, mount, stat, directory enumeration, file read and format. These supersede the original probe-only description; consult the advertised operation masks for availability in each profile. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

No general mutation or relocation/recovery claim follows from these callbacks. Validate independent media, interrupted writes and recovery separately. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/SFS2.so` and `/System/Resources/Documentation/Filesystem-SFS2.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-SFS2/actions/runs/45516226055) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-SFS2/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/sfs2_module.c)
- [Native design](native/filesystems/sfs2/DESIGN.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/SFS2.pages) and [PDF](docs/SFS2.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.
