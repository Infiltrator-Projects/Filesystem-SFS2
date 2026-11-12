/* SPDX-License-Identifier: GPL-3.0-or-later
 * SFS2 identity and geometry, sourced from the independent canonical core.
 * Exclusively Common V2: no path I/O, OS APIs or previous ABI export.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "sfs2_core.h"
#include <stdint.h>
#include <string.h>

enum { MIN_BLOCK = 512, MAX_BLOCK = 65536 };

typedef struct {
    const IfsFsHostRuntimeV2 *host;
    uint32_t block_size, total_blocks, bitmap_base;
    uint64_t verified_free_blocks;
    uint16_t root_sequence;
    uint8_t primary_valid, backup_valid;
} Sfs2Volume;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int read_block(const IfsFsHostRuntimeV2 *h, uint64_t offset,
                      uint8_t *buffer, uint32_t bytes)
{
    /* The SFS2 logical root may be 512 bytes although the host's
     * physical/logical I/O sector is 4096. Never send sub-sector reads to
     * the host: Common V2 reconstructs byte ranges safely. */
    return ifs_fs_host_read_bytes_v2(h, offset, buffer, bytes);
}

static int root_at(const IfsFsHostRuntimeV2 *h, uint32_t bs, uint32_t number,
                   IfsSfs2RootRecord *record)
{
    uint8_t *buffer;
    int valid;
    if (bs < MIN_BLOCK || bs > MAX_BLOCK || (bs & (bs - 1u)) ||
        number >= h->volume_bytes / bs)
        return 0;
    buffer = h->allocate(h->context, bs);
    if (!buffer)
        return 0;
    valid = read_block(h, (uint64_t)number * bs, buffer, bs) == IFS_FS_OK &&
        ifs_sfs2_validate_block_header(buffer, bs, number, IFS_SFS2_ROOT_ID) &&
        ifs_sfs2_decode_root(buffer, bs, record) == 0 &&
        ifs_sfs2_validate_root_record(record) == 0 &&
        record->block_size == bs && record->total_blocks >= 4u &&
        (uint64_t)record->total_blocks * bs <= h->volume_bytes &&
        record->last_byte < (uint64_t)record->total_blocks * bs;
    h->release(h->context, buffer);
    return valid;
}

static int inspect(const IfsFsHostRuntimeV2 *h, Sfs2Volume *out)
{
    uint8_t *first;
    IfsSfs2RootRecord primary = {0}, backup = {0};
    uint32_t bs, total;
    int pvalid = 0, bvalid = 0, saw_root = 0;
    const IfsSfs2RootRecord *selected;
    if (!ifs_fs_host_v2_valid(h) || !out)
        return IFS_FS_INVALID;
    memset(out, 0, sizeof(*out));
    if (h->volume_bytes < 4u * MIN_BLOCK)
        return IFS_FS_NOT_FOUND;
    first = h->allocate(h->context, h->logical_sector_bytes);
    if (!first)
        return IFS_FS_NO_MEMORY;
    if (read_block(h, 0u, first, h->logical_sector_bytes) != IFS_FS_OK) {
        h->release(h->context, first);
        return IFS_FS_IO_ERROR;
    }
    if (be32(first) == IFS_SFS2_ROOT_ID) {
        saw_root = 1;
        bs = be32(first + 52u);
        total = be32(first + 48u);
        if (bs >= MIN_BLOCK && bs <= MAX_BLOCK && !(bs & (bs - 1u)) &&
            total >= 4u && (uint64_t)total * bs <= h->volume_bytes) {
            pvalid = root_at(h, bs, 0u, &primary);
            bvalid = root_at(h, bs, total - 1u, &backup);
        }
    }
    h->release(h->context, first);
    if (!pvalid && !bvalid) {
        for (bs = MIN_BLOCK; bs <= MAX_BLOCK; bs <<= 1u) {
            uint32_t last;
            if (h->volume_bytes % bs ||
                h->volume_bytes / bs > UINT32_MAX)
                continue;
            last = (uint32_t)(h->volume_bytes / bs - 1u);
            if (root_at(h, bs, last, &backup) &&
                backup.total_blocks == last + 1u) {
                bvalid = 1;
                break;
            }
        }
    }
    if (!pvalid && !bvalid)
        return saw_root ? IFS_FS_CORRUPT : IFS_FS_NOT_FOUND;
    if (pvalid && bvalid &&
        (primary.block_size != backup.block_size ||
         primary.total_blocks != backup.total_blocks ||
         primary.first_byte != backup.first_byte ||
         primary.last_byte != backup.last_byte ||
         primary.bitmap_base != backup.bitmap_base ||
         primary.root_object_container != backup.root_object_container ||
         primary.extent_bnode_root != backup.extent_bnode_root ||
         primary.object_node_root != backup.object_node_root))
        return IFS_FS_CORRUPT;
    selected = ifs_sfs2_select_root_copy(
        pvalid, primary.sequence, bvalid, backup.sequence) ? &backup : &primary;
    out->host = h;
    out->block_size = selected->block_size;
    out->total_blocks = selected->total_blocks;
    out->bitmap_base = selected->bitmap_base;
    out->root_sequence = selected->sequence;
    out->primary_valid = (uint8_t)pvalid;
    out->backup_valid = (uint8_t)bvalid;
    return IFS_FS_OK;
}

/* Native SFS2 allocation: checked BTMP blocks, MSB-first free bits and
 * the independently shared SFS2 bitmap geometry. No V1 services or fd. */
static int count_free_bitmap(const Sfs2Volume *v,uint64_t *verified)
{
    uint32_t bits_per=0u,groups=0u;
    uint64_t free_total=0u;
    const IfsFsHostRuntimeV2 *h=v->host;
    if(!h || !verified ||
       ifs_sfs2_compute_bitmap_layout(v->block_size,v->total_blocks,
                                      &bits_per,&groups)!=0 ||
       !bits_per || !groups ||
       (uint64_t)v->bitmap_base+groups>v->total_blocks)
        return IFS_FS_CORRUPT;
    uint8_t *raw=h->allocate(h->context,v->block_size);
    if(!raw)return IFS_FS_NO_MEMORY;
    int rc=IFS_FS_OK;
    for(uint32_t i=0u;i<groups;++i) {
        uint32_t number=v->bitmap_base+i;
        uint64_t first=(uint64_t)i*bits_per,count=v->total_blocks-first;
        if(count>bits_per)count=bits_per;
        rc=read_block(h,(uint64_t)number*v->block_size,raw,v->block_size);
        if(rc!=IFS_FS_OK)break;
        if(!ifs_sfs2_validate_block_header(raw,v->block_size,number,
                                           UINT32_C(0x42544d50))) {
            rc=IFS_FS_CORRUPT;break;
        }
        for(uint64_t bit=0u;bit<count;++bit)
            free_total+=(raw[IFS_SFS2_BLOCK_HEADER_SIZE+(bit>>3u)] &
                         (uint8_t)(0x80u>>(bit&7u)))!=0u;
    }
    h->release(h->context,raw);
    if(rc==IFS_FS_OK)*verified=free_total;
    return rc;
}
static int probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant)
{
    Sfs2Volume volume = {0};
    int rc;
    if (!variant)
        return IFS_FS_INVALID;
    *variant = 0u;
    rc = inspect(h, &volume);
    if (rc == IFS_FS_OK)
        *variant = volume.block_size;
    return rc;
}

static void volume_info(const Sfs2Volume *v, IfsFsVolumeInfoV2 *info)
{
    info->structure_bytes = sizeof(*info);
    info->variant = v->block_size;
    info->max_name_bytes = IFS_SFS2_MAX_FILENAME;
    info->block_bytes = v->block_size;
    info->total_bytes = (uint64_t)v->block_size * v->total_blocks;
    info->free_bytes = v->verified_free_blocks * v->block_size;
}

static int mount(const IfsFsHostRuntimeV2 *h, void **volume,
                 IfsFsVolumeInfoV2 *info)
{
    Sfs2Volume candidate = {0}, *owned;
    int rc;
    if (!volume || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *volume = NULL;
    if (!ifs_fs_host_v2_valid(h))
        return IFS_FS_INVALID;
    if (!(h->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS; /* Unqualified writable mount is forbidden. */
    rc = inspect(h, &candidate);
    if (rc != IFS_FS_OK)
        return rc;
    rc = count_free_bitmap(&candidate,&candidate.verified_free_blocks);
    if (rc != IFS_FS_OK)
        return rc;
    owned = h->allocate(h->context, sizeof(*owned));
    if (!owned)
        return IFS_FS_NO_MEMORY;
    *owned = candidate;
    volume_info(owned, info);
    *volume = owned;
    return IFS_FS_OK;
}

static int unmount(void *volume)
{
    Sfs2Volume *v = volume;
    if (!v || !v->host)
        return IFS_FS_INVALID;
    v->host->release(v->host->context, v);
    return IFS_FS_OK;
}

static int query(void *volume, IfsFsVolumeInfoV2 *info)
{
    Sfs2Volume *v = volume;
    if (!v || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    volume_info(v, info);
    return IFS_FS_OK;
}

static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
               IFS_FS_GROUP_CORE, 0u,
               IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT},
    .probe = probe,
    .mount = mount,
    .unmount = unmount
};
static const IfsFsVolumeOpsV2 volume = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
               IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY},
    .query = query
};
static const IfsFsGroupHeaderV2 *group(
    uint32_t id, uint32_t minimum, uint32_t maximum)
{
    if (minimum > IFS_FS_GROUP_ABI_V2 ||
        maximum < IFS_FS_GROUP_ABI_V2)
        return NULL;
    if (id == IFS_FS_GROUP_CORE)
        return &core.header;
    if (id == IFS_FS_GROUP_VOLUME)
        return &volume.header;
    return NULL;
}
static const IfsFsModuleApiV2 api = {
    IFS_FS_COMMON_ABI_V2, sizeof(IfsFsModuleApiV2), "sfs2",
    IFS_FS_PROFILE_HOSTED,
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    group
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
