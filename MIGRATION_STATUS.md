# SFS2 Common V2 migration status — 20 November 2026

SFS2 already implements only Common V2 and uses its format-owned
`native/filesystems/sfs2/core/` for root, checksum, allocation and
object-structure validation. The hosted provider's former heap-pointer
volume handle could be dereferenced after unmount. It now stores validated
volume metadata in the canonical bounded V2 session registry, refusing
stale/unmounted handles, supporting concurrent volumes and returning proper
BUSY when the bounded registry is exhausted. The actual on-disk root and
bitmap checksum validators remain unchanged.

**Not BOOT-qualified:** the V2 module still lacks genuine object B-tree and
extent traversal, Common V2 STAT/LIST/READ, a freestanding firmware ELF,
production BOOT validation and disposable OVMF kernel/initramfs tests.
The module must not masquerade as a boot reader.

CI packages exactly the hosted SFS2.so and its PDF in portable DEB and native
RUN. No kernel .ko substitute is included, and no new release is published
until relevant firmware/read tests pass.
