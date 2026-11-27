# SFS2: technical reference

## History and version boundary

SFS/2 extends Amiga Smart File System to larger file sizes and extent counts. Its binary root ID is 0x53465302 and supported structure version is 4. The baseline SFS/0 version 3 is not interchangeable. This chapter uses the preserved amipart native formatter and the repository's SFS2 disk layout and reader as evidence, not a complete original vendor specification. The common root and administrative architecture remains related to SFS/0; object and extent encoding differs materially. A parser must distinguish the two before reading variable records.

The AmigaOS 4 capability table distinguishes these formats; earlier 68K handlers must not interpret SFS/2 as SFS/0. Handler-advertised limits are not necessarily theoretical on-media bounds.

The cited AmigaOS table lists a 107-character filename limit, 512-byte recommended blocks, 1 TiB file-size support and nominal partition capacity of 1 TiB at 512-byte blocks (up to 64 TiB at 32 KiB blocks). These are published handler limits, not the full 48-bit encoded size or independently qualified provider limits. Older products labelled SFS2 can predate and be incompatible with the later SFS/2 version-4 layout. Record the producing handler and version in test fixtures.

## Root copies and geometry

All persistent numeric fields in these Amiga SFS profiles are big-endian. Root copies occur at block 0 and the last volume block. A common block header has four-byte ID at 0, checksum at 4 and self block at 8. Root fields: version 12 and sequence 14 (16-bit), creation date 16, flags byte 20, partition first and exclusive last byte positions 32 and 40 (64-bit), total blocks 48, block bytes 52, bitmap start 96, admin-space container 100, root object container 104, extent tree root 108, object-node tree root 112. Validate both root copies with native checksum, self identity, actual volume length, block size and all referenced regions; a larger sequence is not enough if the referenced state is corrupt. Compare sequence rollover using independently established native rules. Addresses inside the volume are partition-relative block numbers.

The baseline decoder uses a 128-byte root record. A 36-byte RootInfo record contains deleted block/file counts, free blocks, creation date, last allocated block, last admin-space block, last extent node, last object node and roving pointer. Treat these counts and hints as bounded summaries; validate against actual allocation ownership.

## SFS/2 checksum and expanded object layout

SFS/2 checksum at byte 4 is the negative modulo-2^32 sum of 2 plus every other big-endian word. The complete block sums to 0xFFFFFFFE, unlike SFS/0. Object bytes 16..19 hold the high 32 bits of file size and bytes 20..21 the low 16 bits. Length = high32*65536+low16. For high32=0x00010000 and low16=0x1234 the length is 4294971956 bytes. Modification date is at 22, flags at 26 and name begins 27. For directories the words at 12/16 still represent hash and first container; do not apply the size formula to them. Extent leaves are 16 bytes, including a 32-bit block count at 12, not SFS/0's 16-bit count. The 48-bit size field is an arithmetic bound, not proof of device capacity.

## Allocation bitmap and admin space

A BTMP block has a 12-byte common header, then big-endian bitmap words; set bits mean free, with bit 31 first in each word. A B-byte bitmap represents 8*(B-12) blocks. For B=512, coverage is 4000 blocks. Block 4100 uses bitmap block index 1, local bit 100, word 3, bit position 27 (mask 0x08000000); add the root bitmap base to the bitmap block index. Check final padding and reserve root copies, admin space and transaction blocks. ADMC records have next/previous at 12/16, flag byte 20, then eight-byte pairs from 24 containing block address and used mask. Unlike the general bitmap, set bits in admin masks denote used blocks. Reconcile both levels: available internal metadata slots are not unreserved free data blocks.

## Object containers and identity

OBJC blocks have parent at 12, next at 16, previous at 20, and variable object records from 24. Root node ID is 1; recycled node ID is 2. An object record has UID/GID at 0/2, object-node ID at 4, protection at 8, a type-dependent pair of words at 12/16: first extent and size encoding for a file, hash-table pointer and first directory container for a directory. Validate chain owner and reciprocal previous link, bounded record size and unique node identity. Names and comments are adjacent NUL-terminated native byte strings, with records rounded up to an even size. Reject missing terminators or overlong names, not merely a missing final padding byte. Object type flags include hidden 1, hard link 32, link 64 and directory 128; interpret the union only after checking type.

The historical native filename limit is 107 bytes in this single-byte representation. The SLNK soft-link block carries the common header, parent/next/previous linkage and target string. Interpret the link path under native semantics before translating to a host presentation.

## Object-node index and namespace

NDC (space in fourth byte) containers hold base node number at 12, coverage value at 16 and entries from 20. Internal node pointers and ten-byte leaf object-node records have different meanings; a leaf contains object-container location, next collision node and 16-bit hash. HTAB blocks hold parent at 12 and 32-bit bucket pointers from 16. Derive bucket capacity from (B-16)/4; use the native hash and comparison policy and follow validated collision chains. Compare exact bounded names after hash matches. An independent reader may enumerate the authoritative OBJC chain instead of relying on an optional hash index, but must detect duplicate names and loops. Root flags include case-sensitive bit 7. Use the actual native comparison semantics, not a host filesystem's Unicode rules.

## Extent B-tree and worked mapping

BNDC blocks have node count at 12 (16 bits), leaf flag 14, stride 15; records begin at 16. Internal records contain big-endian lower-bound key and child block (eight bytes). Leaf key is the physical start of a contiguous extent, with next and previous extent keys at 4 and 8. Search by the wanted key, taking the greatest lower-bound internal key, then require an exact leaf match. Follow the file's first extent and subsequent next links in logical order; validate previous links, length, range, allocation and cycles. For a 4096-byte block and a file with run (100,3) followed by (500,2), logical byte 14000 is 1712 bytes into the second run, at partition byte 2049712. Clip to logical EOF and distinguish raw payload from B-tree metadata.

## Transactions, mutation and recovery

SFS-family metadata uses transaction storage and markers TRST, TRFA and TROK. In the reference clean reader, the marker is at root_object_container+2; a valid TROK is required and TRFA is rejected instead of replayed. A dirty marker is not a repair instruction. A native writer must preserve old metadata until the recoverable transaction is established, update object containers, node/hash indexes, extent links and both allocation layers coherently, and publish the required native transaction state. Relocation must copy and verify data before changing ownership, then retire the old run only after durable publication. Exact transaction encoding, replay ordering, root sequence rollover and all native link semantics need independent original-format evidence before general writer qualification.

A reference empty-volume topology places OBJC with RootInfo at the root object block, HTAB at root+1, TROK at root+2, BNDC at root+3, NDC at root+4 and an optional recycled OBJC at root+5. Root copies and bitmap/admin-space locations must be independently validated; this topology is not a substitute for reading recorded pointers.

## Independent verification

Test authentic Amiga SFS media, both root copies, block-size boundaries, long names, native case comparison, multiple object containers, extent tree depth, fragmented files, admin-space exhaustion, allocation mismatch and dirty transaction state. For each file compare logical byte length and payload hash against an independent reader. Verify full metadata ownership, not just extent data, and inject interruption after every write phase. A formatter must establish matching root copies, bitmap and admin allocation, root object and node indexes, extent tree and clean transaction metadata. Source-level evidence is not runtime proof. 

## Implementation and qualification boundary

Historical qualification record 4551626055 described probe, mount, namespace read and format. Redundant geometry and allocation roots must agree; general namespace mutation and recovery require their own evidence.

Common V2 is the independent provider interface: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate real operation masks and media profiles. Use bounded host I/O, exclusive unmounted ownership and durable flush for mutation; distinguish unsupported, corrupt, I/O and stale-handle results. Test Media and Defragmenter use the same engine only for supported operations. Hosted, freestanding, BOOT, kernel, recovery and package qualification are independent. Current capability and release status belongs to the repository README and exact-source qualification records; historical evidence is not a current status claim.

Historical source or test reference: https://github.com/Infiltrator-Projects/Filesystem-SFS2/actions/runs/4551626055 .

## Source boundary

Source evidence: https://github.com/Infiltrator-Projects/Filesystem-SFS2/tree/bc72c35fe2d7fe2986804ee56204067b0245247b/native/filesystems/sfs2/original/amipart (src/nativefmt.c); compare linux/disk_layout.h, core/sfs2_core.c and module/sfs2_reader.c at the same commit. Verify large-file and long-extent fields against genuine SFS/2 media, including greater-than-4-GiB lengths. Exact native recovery, node allocation, links and name rules remain subject to independent qualification.

Historical cross-check sources include SmartFileSystem 1.279 (https://aminet.net/package/disk/misc/SFS), AROS SFS low-level structures and transaction code, AmiPart src/nativefmt.c and Aaru.Filesystems/SFS. The source comparison does not replace genuine handler-produced image tests.

