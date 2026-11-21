/* SPDX-License-Identifier: GPL-3.0-or-later
 * SFS2 identity and geometry, sourced from the independent canonical core.
 * Exclusively Common V2: no path I/O, OS APIs or previous ABI export.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <infiltratr/filesystem_ro_sessions_v2.h>
#include "sfs2_core.h"
#include "sfs2_reader.h"
#include <stdint.h>
#include <string.h>

enum { MIN_BLOCK = 512, MAX_BLOCK = 65536 };

typedef struct {
    const IfsFsHostRuntimeV2 *host;
    uint32_t block_size, total_blocks, bitmap_base;
    uint32_t object_root, extent_root;
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

/* Return I/O and allocation failures separately from an invalid root.
 * A failed block read must not be interpreted as absent/corrupt media when
 * BOOT decides whether it can safely mount the selected partition. */
static int root_at(const IfsFsHostRuntimeV2 *h, uint32_t bs, uint32_t number,
                   IfsSfs2RootRecord *record, int *valid)
{
    uint8_t *buffer;
    int status;
    *valid = 0;
    if (bs < MIN_BLOCK || bs > MAX_BLOCK || (bs & (bs - 1u)) ||
        number >= h->volume_bytes / bs)
        return IFS_FS_OK;
    buffer = h->allocate(h->context, bs);
    if (!buffer)
        return IFS_FS_NO_MEMORY;
    status = read_block(h, (uint64_t)number * bs, buffer, bs);
    if (status == IFS_FS_OK)
        *valid =
            ifs_sfs2_validate_block_header(buffer, bs, number, IFS_SFS2_ROOT_ID) &&
            ifs_sfs2_decode_root(buffer, bs, record) == 0 &&
            ifs_sfs2_validate_root_record(record) == 0 &&
            record->block_size == bs && record->total_blocks >= 4u &&
            (uint64_t)record->total_blocks * bs <= h->volume_bytes &&
            record->last_byte < (uint64_t)record->total_blocks * bs;
    h->release(h->context, buffer);
    return status;
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
    const int initial_read = read_block(h, 0u, first,
                                        h->logical_sector_bytes);
    if (initial_read != IFS_FS_OK) {
        h->release(h->context, first);
        return initial_read;
    }
    if (be32(first) == IFS_SFS2_ROOT_ID) {
        saw_root = 1;
        bs = be32(first + 52u);
        total = be32(first + 48u);
        if (bs >= MIN_BLOCK && bs <= MAX_BLOCK && !(bs & (bs - 1u)) &&
            total >= 4u && (uint64_t)total * bs <= h->volume_bytes) {
            int status = root_at(h, bs, 0u, &primary, &pvalid);
            if (status != IFS_FS_OK) {
                h->release(h->context, first);
                return status;
            }
            status = root_at(h, bs, total - 1u, &backup, &bvalid);
            if (status != IFS_FS_OK) {
                h->release(h->context, first);
                return status;
            }
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
            int candidate_valid = 0;
            int status = root_at(h, bs, last, &backup, &candidate_valid);
            if (status != IFS_FS_OK) return status;
            if (candidate_valid && backup.total_blocks == last + 1u) {
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
    out->object_root = selected->root_object_container;
    out->extent_root = selected->extent_bnode_root;
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

/* A query-after-unmount must never dereference host-owned freed memory.
 * This provider is metadata-only: use the canonical bounded V2 registry
 * rather than accepting heap addresses as unvalidated volume handles. */
static IfsFsReadOnlySessionsV2 sessions = IFS_FS_RO_SESSIONS_INIT_V2;
/* Host-owned read-only I/O and native geometry are retained behind
 * Common V2 generation handles, never raw caller pointers. */
static struct { uintptr_t cookie; IfsSfs2Reader reader; }
readers[IFS_FS_RO_MAX_SESSIONS_V2];
static const IfsSfs2Reader *reader_for(void *handle)
{
    const uintptr_t token=(uintptr_t)handle;
    if (!token) return NULL;
    for (uint32_t i=0;i<IFS_FS_RO_MAX_SESSIONS_V2;++i)
        if (readers[i].cookie==token &&
            sessions.slots[i].cookie==token)
            return &readers[i].reader;
    return NULL;
}
static int mount(const IfsFsHostRuntimeV2 *h, void **volume,
                 IfsFsVolumeInfoV2 *info)
{
    Sfs2Volume candidate = {0};
    IfsFsVolumeInfoV2 measured = {0};
    int rc;
    if (!volume || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *volume = NULL;
    if (!ifs_fs_host_v2_valid(h))
        return IFS_FS_INVALID;
    if (!(h->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
    rc = inspect(h, &candidate);
    if (rc != IFS_FS_OK)
        return rc;
    rc = count_free_bitmap(&candidate, &candidate.verified_free_blocks);
    if (rc != IFS_FS_OK)
        return rc;
    IfsSfs2Reader verify={0};
    size_t n=h->structure_bytes<sizeof(verify.host)?
             h->structure_bytes:sizeof(verify.host);
    memcpy(&verify.host,h,n);
    verify.host.structure_bytes=(uint32_t)n;
    verify.block_bytes=candidate.block_size;
    verify.total_blocks=candidate.total_blocks;
    verify.object_root=candidate.object_root;
    verify.extent_root=candidate.extent_root;
    rc=ifs_sfs2_ro_validate(&verify);
    if(rc!=IFS_FS_OK)return rc;
    volume_info(&candidate, &measured);
    rc=ifs_fs_ro_session_open_v2(&sessions, &measured, volume, info);
    if (rc!=IFS_FS_OK) return rc;
    ifs_fs_ro_sessions_lock_v2(&sessions);
    for (uint32_t i=0;i<IFS_FS_RO_MAX_SESSIONS_V2;++i) {
        if (sessions.slots[i].cookie==(uintptr_t)*volume) {
            IfsSfs2Reader *reader=&readers[i].reader;
            size_t n=h->structure_bytes<sizeof(reader->host)?
                     h->structure_bytes:sizeof(reader->host);
            memset(reader,0,sizeof(*reader));
            memcpy(&reader->host,h,n);
            reader->host.structure_bytes=(uint32_t)n;
            reader->block_bytes=candidate.block_size;
            reader->total_blocks=candidate.total_blocks;
            reader->object_root=candidate.object_root;
            reader->extent_root=candidate.extent_root;
            readers[i].cookie=(uintptr_t)*volume;
            break;
        }
    }
    ifs_fs_ro_sessions_unlock_v2(&sessions);
    return IFS_FS_OK;
}
static int unmount(void *volume)
{
    int rc=ifs_fs_ro_session_close_v2(&sessions,volume);
    if (rc!=IFS_FS_OK)return rc;
    ifs_fs_ro_sessions_lock_v2(&sessions);
    for (uint32_t i=0;i<IFS_FS_RO_MAX_SESSIONS_V2;++i)
        if (readers[i].cookie==(uintptr_t)volume) {
            readers[i].cookie=0u;
            memset(&readers[i].reader,0,sizeof(readers[i].reader));
            break;
        }
    ifs_fs_ro_sessions_unlock_v2(&sessions);
    return IFS_FS_OK;
}
static int query(void *volume, IfsFsVolumeInfoV2 *info)
{
    return ifs_fs_ro_session_query_v2(&sessions, volume, info);
}
static int stat_path(void *volume,const char *path,IfsFsEntryV2 *entry)
{
    const IfsSfs2Reader *reader=reader_for(volume);
    return reader?ifs_sfs2_ro_stat(reader,path,entry):IFS_FS_INVALID;
}
static int list_path(void *volume,const char *path,uint32_t index,
                     IfsFsEntryV2 *entry)
{
    const IfsSfs2Reader *reader=reader_for(volume);
    return reader?ifs_sfs2_ro_list(reader,path,index,entry):IFS_FS_INVALID;
}
static int read_path(void *volume,const char *path,uint64_t offset,
                     void *out,uint32_t length)
{
    const IfsSfs2Reader *reader=reader_for(volume);
    return reader?ifs_sfs2_ro_read(reader,path,offset,out,length):IFS_FS_INVALID;
}

static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
               IFS_FS_GROUP_CORE, 0u,
               IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
               IFS_FS_CORE_STAT | IFS_FS_CORE_LIST | IFS_FS_CORE_READ},
    .probe = probe,
    .mount = mount,
    .unmount = unmount,
    .stat=stat_path, .directory_entry=list_path, .read=read_path
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
__attribute__((used,retain,section(".ifsmeta")))
static const char identity[]="IFS-MODULE:sfs2:common2";
static const IfsFsModuleApiV2 api = {
    IFS_FS_COMMON_ABI_V2, sizeof(IfsFsModuleApiV2), "sfs2",
#if defined(IFS_SFS2_FIRMWARE_BUILD)
    IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
#else
    IFS_FS_PROFILE_HOSTED,
#endif
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    group
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
