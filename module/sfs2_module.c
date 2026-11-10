/* SPDX-License-Identifier: GPL-3.0-or-later
 * SFS2.so: sole Common V2 ABI; independent checksum-verified root probe.
 * No alternate historical export, pathname I/O or duplicate SFS engine.
 */
#include "sfs2_core.h"
#include <infiltratr/filesystem_module_v2.h>
__attribute__((used,section(".ifsmeta")))
static const char strict_v2_identity[]="IFS-MODULE:sfs2:common2";
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t block_size,total_blocks,bitmap_base;
    uint16_t structure_version,root_sequence;
    uint64_t filesystem_bytes,device_bytes;
    uint8_t primary_root_valid,backup_root_valid;
} Sfs2Geometry;
typedef int (*Sfs2ReadAt)(void *,uint64_t,void *,uint32_t);

#define SFS2_MIN_BLOCK 512u
#define SFS2_MAX_BLOCK 65536u
static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static int read_root(void *ctx, Sfs2ReadAt read_at, uint64_t device_bytes,
                     uint32_t block_size,
                     uint32_t number, IfsSfs2RootRecord *record) {
    if (block_size < SFS2_MIN_BLOCK || block_size > SFS2_MAX_BLOCK ||
        (block_size & (block_size - 1u)) != 0u ||
        (uint64_t)number >= device_bytes / block_size)
        return -1;
    unsigned char *buffer = malloc(block_size);
    if (!buffer) return -1;
    const uint64_t at = (uint64_t)number * block_size;
    const int valid =
        read_at(ctx, at, buffer, block_size) == 0 &&
        ifs_sfs2_validate_block_header(buffer, block_size, number, IFS_SFS2_ROOT_ID) &&
        ifs_sfs2_decode_root(buffer, block_size, record) == 0 &&
        ifs_sfs2_validate_root_record(record) == 0 &&
        record->block_size == block_size &&
        record->total_blocks >= 4u &&
        (uint64_t)record->total_blocks * block_size <= device_bytes &&
        record->last_byte < (uint64_t)record->total_blocks * block_size;
    free(buffer);
    return valid ? 0 : -1;
}
static int inspect_io(void *ctx, Sfs2ReadAt read_at,
                      uint64_t device_bytes, Sfs2Geometry *geometry) {
    if (!geometry || !read_at || device_bytes < 4u * 512u) return -1;
    memset(geometry, 0, sizeof(*geometry));
    /* The primary may be damaged. Independently try a last-block backup at
       each supported block size, never interpreting SFS0 as SFS2. */
    IfsSfs2RootRecord primary = {0}, backup = {0};
    int pvalid = 0, bvalid = 0;
    unsigned char probe[512];
    if (read_at(ctx,0,probe,sizeof(probe)) == 0 &&
        be32(probe) == IFS_SFS2_ROOT_ID) {
        const uint32_t bs = be32(probe + 52u);
        const uint32_t total = be32(probe + 48u);
        if (bs >= SFS2_MIN_BLOCK && bs <= SFS2_MAX_BLOCK &&
            (bs & (bs - 1u)) == 0u && total >= 4u &&
            (uint64_t)total * bs <= device_bytes) {
            pvalid = read_root(ctx, read_at, device_bytes, bs, 0u, &primary) == 0;
            bvalid = read_root(ctx, read_at, device_bytes, bs, total - 1u, &backup) == 0;
        }
    }
    if (!pvalid && !bvalid) {
        for (uint32_t bs = SFS2_MIN_BLOCK; bs <= SFS2_MAX_BLOCK; bs <<= 1u) {
            if (device_bytes % bs != 0u || device_bytes / bs > UINT32_MAX) continue;
            const uint32_t last = (uint32_t)(device_bytes / bs - 1u);
            if (read_root(ctx, read_at, device_bytes, bs, last, &backup) == 0 &&
                backup.total_blocks == last + 1u) {
                bvalid = 1;
                break;
            }
        }
    }
    if (!pvalid && !bvalid) return -1;
    if (pvalid && bvalid &&
        (primary.block_size != backup.block_size ||
         primary.total_blocks != backup.total_blocks ||
         primary.first_byte != backup.first_byte ||
         primary.last_byte != backup.last_byte ||
         primary.bitmap_base != backup.bitmap_base ||
         primary.root_object_container != backup.root_object_container ||
         primary.extent_bnode_root != backup.extent_bnode_root ||
         primary.object_node_root != backup.object_node_root)) return -1;
    const IfsSfs2RootRecord *chosen =
        ifs_sfs2_select_root_copy(pvalid, primary.sequence, bvalid, backup.sequence)
        ? &backup : &primary;
    geometry->block_size = chosen->block_size;
    geometry->total_blocks = chosen->total_blocks;
    geometry->bitmap_base = chosen->bitmap_base;
    geometry->structure_version = chosen->version;
    geometry->root_sequence = chosen->sequence;
    geometry->filesystem_bytes = (uint64_t)chosen->block_size * chosen->total_blocks;
    geometry->device_bytes = device_bytes;
    geometry->primary_root_valid = (uint8_t)pvalid;
    geometry->backup_root_valid = (uint8_t)bvalid;
    return 0;
}
static int v2_probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    Sfs2Geometry geometry = {0};
    if (!ifs_fs_host_v2_valid(host) || !variant) return IFS_FS_INVALID;
    if (inspect_io(host->context, host->read_at, host->volume_bytes,
                   &geometry) != 0) return IFS_FS_NOT_FOUND;
    *variant = geometry.structure_version;
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV2 v2_core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
               IFS_FS_GROUP_CORE, 0u, IFS_FS_CORE_PROBE},
    .probe = v2_probe
};
static const IfsFsGroupHeaderV2 *v2_query(uint32_t id,
                                          uint32_t minimum,
                                          uint32_t maximum)
{
    return id == IFS_FS_GROUP_CORE && minimum <= IFS_FS_GROUP_ABI_V2 &&
        maximum >= IFS_FS_GROUP_ABI_V2 ? &v2_core.header : NULL;
}
static const IfsFsModuleApiV2 common_v2 = {
    IFS_FS_COMMON_ABI_V2, sizeof(IfsFsModuleApiV2), "sfs2",
    IFS_FS_PROFILE_HOSTED, IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),
    v2_query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) {
    return &common_v2;
}
