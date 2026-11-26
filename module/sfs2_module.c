/* SPDX-License-Identifier: GPL-3.0-or-later
 * SFS2 identity and geometry, sourced from the independent canonical core.
 * Exclusively Common V2: no path I/O, OS APIs or previous ABI export.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <infiltratr/filesystem_ro_sessions_v2.h>
#include "sfs2_core.h"
#include "sfs2_reader.h"
#include <stdint.h>
#include <stdatomic.h>
#include <limits.h>
#include <string.h>

enum { MIN_BLOCK = 512, MAX_BLOCK = 65536 };

typedef struct {
    const IfsFsHostRuntimeV2 *host;
    uint32_t block_size, total_blocks, bitmap_base;
    uint32_t object_root, extent_root, admin_root, node_root;
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
            record->last_byte <= (uint64_t)record->total_blocks * bs &&
            (number == 0u || number == record->total_blocks - 1u);
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
            if (h->volume_bytes % bs || h->volume_bytes / bs < 4u ||
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
         primary.adminspace_container != backup.adminspace_container ||
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
    out->admin_root = selected->adminspace_container;
    out->node_root = selected->object_node_root;
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
        /* A correct checksum is not enough: no metadata block required
         * to identify or read this mounted volume may be free.  Otherwise
         * an allocator could reuse an active root, bitmap, object container,
         * transaction marker or extent tree without a corrupt header. */
        for(uint64_t bit=0u;bit<count;++bit) {
            const uint64_t number=first+bit;
            const int free_bit=(raw[IFS_SFS2_BLOCK_HEADER_SIZE+(bit>>3u)] &
                                (uint8_t)(0x80u>>(bit&7u)))!=0u;
            if(free_bit &&
               (number==0u || number==v->total_blocks-1u ||
                (number>=v->bitmap_base &&
                 number<(uint64_t)v->bitmap_base+groups) ||
                number==v->object_root || number==v->extent_root ||
                number==v->admin_root || number==v->node_root ||
                (v->object_root<=v->total_blocks-3u &&
                 number==(uint64_t)v->object_root+2u))) {
                rc=IFS_FS_CORRUPT;
                break;
            }
            free_total+=(uint64_t)free_bit;
        }
        if(rc!=IFS_FS_OK)break;
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
typedef struct {
    uintptr_t cookie;
    unsigned active;
    IfsSfs2Reader reader;
    uint32_t bitmap_base;
} Sfs2Session;
static Sfs2Session readers[IFS_FS_RO_MAX_SESSIONS_V2];

/* Pin the reader before dropping the registry lock. Holding the lock over
 * host I/O would serialize independent volumes and deadlock a reentrant
 * callback. The active count prevents teardown until I/O returns. */
static Sfs2Session *acquire_reader(void *handle)
{
    const uintptr_t token=(uintptr_t)handle;
    Sfs2Session *result=NULL;
    if (!token) return NULL;
    ifs_fs_ro_sessions_lock_v2(&sessions);
    for (uint32_t i=0;i<IFS_FS_RO_MAX_SESSIONS_V2;++i)
        if (readers[i].cookie==token &&
            sessions.slots[i].cookie==token &&
            readers[i].active!=UINT_MAX) {
            ++readers[i].active;
            result=&readers[i];
            break;
        }
    ifs_fs_ro_sessions_unlock_v2(&sessions);
    return result;
}
static int finish_reader(Sfs2Session *session,int rc)
{
    ifs_fs_ro_sessions_lock_v2(&sessions);
    --session->active;
    ifs_fs_ro_sessions_unlock_v2(&sessions);
    return rc;
}
static int mount_impl(const IfsFsHostRuntimeV2 *h, void **volume,
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
    void *token=NULL;
    rc=ifs_fs_ro_session_open_v2(&sessions, &measured, &token, info);
    if (rc!=IFS_FS_OK) return rc;
    ifs_fs_ro_sessions_lock_v2(&sessions);
    for (uint32_t i=0;i<IFS_FS_RO_MAX_SESSIONS_V2;++i) {
        if (sessions.slots[i].cookie==(uintptr_t)token) {
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
            readers[i].bitmap_base=candidate.bitmap_base;
            readers[i].cookie=(uintptr_t)token;
            *volume=token;
            break;
        }
    }
    ifs_fs_ro_sessions_unlock_v2(&sessions);
    return *volume ? IFS_FS_OK : IFS_FS_INVALID;
}
static int unmount(void *volume)
{
    if (!volume) return IFS_FS_INVALID;
    int rc=IFS_FS_INVALID;
    ifs_fs_ro_sessions_lock_v2(&sessions);
    for(uint32_t i=0u;i<IFS_FS_RO_MAX_SESSIONS_V2;++i)
        if(readers[i].cookie==(uintptr_t)volume &&
           sessions.slots[i].cookie==(uintptr_t)volume) {
            if (readers[i].active) rc=IFS_FS_BUSY;
            else {
                /* Retire both records atomically; slot reuse cannot race
                 * cleanup of a previous generation's reader. */
                sessions.slots[i]=(IfsFsReadOnlySessionSlotV2){0};
                readers[i]=(Sfs2Session){0};
                rc=IFS_FS_OK;
            }
            break;
        }
    ifs_fs_ro_sessions_unlock_v2(&sessions);
    return rc;
}
static int query(void *volume, IfsFsVolumeInfoV2 *info)
{
    return ifs_fs_ro_session_query_v2(&sessions, volume, info);
}
static int stat_path(void *volume,const char *path,IfsFsEntryV2 *entry)
{
    Sfs2Session *session=acquire_reader(volume);
    return session?finish_reader(session,ifs_sfs2_ro_stat(&session->reader,path,entry)):IFS_FS_INVALID;
}
static int list_path(void *volume,const char *path,uint32_t index,
                     IfsFsEntryV2 *entry)
{
    Sfs2Session *session=acquire_reader(volume);
    return session?finish_reader(session,ifs_sfs2_ro_list(&session->reader,path,index,entry)):IFS_FS_INVALID;
}
static int read_path(void *volume,const char *path,uint64_t offset,
                     void *out,uint32_t length)
{
    Sfs2Session *session=acquire_reader(volume);
    return session?finish_reader(session,ifs_sfs2_ro_read(&session->reader,path,offset,out,length)):IFS_FS_INVALID;
}

#if !defined(IFS_SFS2_FIRMWARE_BUILD)
/* SmartFilesystem 1.279 empty-volume layout: two roots, a 32-block
 * administration region, root/recycled objects, hash table, clean
 * transaction marker, extent and object-node trees, and free bitmaps.
 * Layout reference: amipart nativefmt.c (retained in Filesystem-SFS2).
 * This implementation uses the provider's canonical checksum and
 * Common V2 byte transport; no pathname or external formatter.
 * The host supplies partition-relative geometry, not disk offsets. */
enum { FMT_ADMIN=1u, FMT_ROOT=2u, FMT_HASH=3u, FMT_TRANS=4u,
       FMT_EXTENT=5u, FMT_NODES=6u, FMT_RECYCLED=7u,
       FMT_ADMIN_COUNT=32u, FMT_BITMAP=33u, FMT_BLOCK=4096u };
static atomic_flag format_gate=ATOMIC_FLAG_INIT;
static void fmt16(uint8_t *p,uint32_t x)
{p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void fmt32(uint8_t *p,uint32_t x)
{fmt16(p,x>>16);fmt16(p+2,x);}
static void fmt64(uint8_t *p,uint64_t x)
{fmt32(p,(uint32_t)(x>>32));fmt32(p+4,(uint32_t)x);}
static void fmt_header(uint8_t *p,uint32_t id,uint32_t own)
{
    memset(p,0,FMT_BLOCK);
    fmt32(p,id);fmt32(p+8,own);
}
static int fmt_write(const IfsFsHostRuntimeV2 *h,uint32_t own,uint8_t *p,int stamp)
{
    if(h->cancelled && h->cancelled(h->context))return IFS_FS_BUSY;
    if(stamp) {
        fmt32(p+8,own);
        fmt32(p+4,ifs_sfs2_calculate_block_checksum(p,FMT_BLOCK));
    }
    return ifs_fs_host_write_bytes_v2(h,(uint64_t)own*FMT_BLOCK,p,FMT_BLOCK);
}
static uint16_t fmt_hash(const char *name)
{
    uint16_t hash=(uint16_t)strlen(name);
    for(;*name;++name) {
        unsigned char ch=(unsigned char)*name;
        if(ch>='a' && ch<='z')ch=(unsigned char)(ch-32);
        hash=(uint16_t)(hash*13u+ch);
    }
    return hash;
}
static void fmt_object(uint8_t *p,uint32_t node,uint32_t protection,
                       uint32_t hash,uint32_t children,uint32_t date,
                       uint8_t bits,const char *name)
{
    fmt32(p+4,node);fmt32(p+8,protection);
    fmt32(p+12,hash);fmt32(p+16,children);
    fmt32(p+22,date);p[26]=bits;
    memcpy(p+27,name,strlen(name)+1u);
}
static int format_impl(const IfsFsHostRuntimeV2 *h,const char *label)
{
    const uint32_t total=(uint32_t)(h->volume_bytes/FMT_BLOCK);
    uint32_t bits=0u,maps=0u;
    if(ifs_sfs2_compute_bitmap_layout(FMT_BLOCK,total,&bits,&maps)!=0 ||
       !bits || !maps || (uint64_t)FMT_BITMAP+maps+17u>=total)
        return IFS_FS_UNSUPPORTED;
    const uint32_t first_free=FMT_BITMAP+maps;
    const uint32_t free_blocks=total-first_free-1u;
    uint32_t date=0u;
    if(h->realtime_ns) {
        uint64_t t=h->realtime_ns(h->context)/1000000000ull;
        if(t>=252460800ull && t-252460800ull<=UINT32_MAX)
            date=(uint32_t)(t-252460800ull);
    }
    uint8_t block[FMT_BLOCK]={0};
    int rc;
    /* Revoke both old root copies, then persist metadata before publishing
     * either new identity. Never expose a new root during partial format. */
    rc=fmt_write(h,0u,block,0);if(rc!=IFS_FS_OK)return rc;
    rc=fmt_write(h,total-1u,block,0);if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    for(uint32_t i=FMT_ADMIN;i<FMT_BITMAP;++i) {
        rc=fmt_write(h,i,block,0);if(rc!=IFS_FS_OK)return rc;
    }
    fmt_header(block,0x41444d43u,FMT_ADMIN);
    block[20]=FMT_ADMIN_COUNT;fmt32(block+24,FMT_ADMIN);
    /* The first seven admin slots are allocated; the remaining 25
     * belong to the admin allocator, not the general free-space map. */
    fmt32(block+28,0xfe000000u);
    rc=fmt_write(h,FMT_ADMIN,block,1);if(rc!=IFS_FS_OK)return rc;
    fmt_header(block,0x4f424a43u,FMT_ROOT);
    fmt_object(block+24,1u,15u,FMT_HASH,FMT_RECYCLED,date,128u,label);
    fmt32(block+FMT_BLOCK-28u,free_blocks);
    fmt32(block+FMT_BLOCK-24u,date);
    rc=fmt_write(h,FMT_ROOT,block,1);if(rc!=IFS_FS_OK)return rc;
    fmt_header(block,0x48544142u,FMT_HASH);
    fmt32(block+12,1u);
    const uint16_t hash=fmt_hash(".recycled");
    fmt32(block+16u+4u*(hash%((FMT_BLOCK-16u)/4u)),2u);
    rc=fmt_write(h,FMT_HASH,block,1);if(rc!=IFS_FS_OK)return rc;
    fmt_header(block,0x54524f4bu,FMT_TRANS);
    rc=fmt_write(h,FMT_TRANS,block,1);if(rc!=IFS_FS_OK)return rc;
    fmt_header(block,0x424e4443u,FMT_EXTENT);
    block[14]=1u;block[15]=16u;
    rc=fmt_write(h,FMT_EXTENT,block,1);if(rc!=IFS_FS_OK)return rc;
    fmt_header(block,0x4e444320u,FMT_NODES);
    fmt32(block+12,1u);fmt32(block+16,1u);
    fmt32(block+20,FMT_ROOT);fmt32(block+30,FMT_RECYCLED);
    fmt16(block+38,hash);
    for(uint32_t i=0;i<4u;++i)fmt32(block+40u+10u*i,UINT32_MAX);
    rc=fmt_write(h,FMT_NODES,block,1);if(rc!=IFS_FS_OK)return rc;
    fmt_header(block,0x4f424a43u,FMT_RECYCLED);
    fmt32(block+12,1u);
    fmt_object(block+24,2u,12u,0u,0u,date,135u,".recycled");
    rc=fmt_write(h,FMT_RECYCLED,block,1);if(rc!=IFS_FS_OK)return rc;
    for(uint32_t i=0;i<maps;++i) {
        fmt_header(block,0x42544d50u,FMT_BITMAP+i);
        for(uint32_t j=0;j<bits;++j) {
            uint64_t number=(uint64_t)i*bits+j;
            if(number>=first_free && number<total-1u)
                block[12u+j/8u]|=(uint8_t)(0x80u>>(j%8u));
        }
        rc=fmt_write(h,FMT_BITMAP+i,block,1);if(rc!=IFS_FS_OK)return rc;
    }
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    fmt_header(block,IFS_SFS2_ROOT_ID,total-1u);
    fmt16(block+12,IFS_SFS2_STRUCTURE_VERSION);
    fmt32(block+16,date);block[20]=64u;
    fmt64(block+40,h->volume_bytes);
    fmt32(block+48,total);fmt32(block+52,FMT_BLOCK);
    fmt32(block+96,FMT_BITMAP);fmt32(block+100,FMT_ADMIN);
    fmt32(block+104,FMT_ROOT);fmt32(block+108,FMT_EXTENT);
    fmt32(block+112,FMT_NODES);
    rc=fmt_write(h,total-1u,block,1);if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    rc=fmt_write(h,0u,block,1);if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0) {
        memset(block,0,sizeof(block));
        (void)ifs_fs_host_write_bytes_v2(h,0u,block,sizeof(block));
        (void)ifs_fs_host_write_bytes_v2(h,h->volume_bytes-FMT_BLOCK,block,sizeof(block));
        (void)h->flush(h->context);return IFS_FS_IO_ERROR;
    }
    return IFS_FS_OK;
}
static int format_volume(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                          const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if(variant!=FMT_BLOCK || flags ||
       h->logical_sector_bytes>FMT_BLOCK ||
       h->volume_bytes%FMT_BLOCK || h->volume_bytes<16ull*1024u*1024u ||
       h->volume_bytes>16ull*1024u*1024u*1024u)return IFS_FS_UNSUPPORTED;
    if(!label || !*label)label="Untitled";
    const size_t n=strlen(label);
    if(n>30u)return IFS_FS_INVALID;
    for(size_t i=0;i<n;++i)
        if((unsigned char)label[i]<32u || (unsigned char)label[i]>126u ||
           label[i]==':' || label[i]=='/')return IFS_FS_INVALID;
    if(atomic_flag_test_and_set_explicit(&format_gate,memory_order_acquire))
        return IFS_FS_BUSY;
    int rc=IFS_FS_OK;
    ifs_fs_ro_sessions_lock_v2(&sessions);
    for(uint32_t i=0;i<IFS_FS_RO_MAX_SESSIONS_V2;++i)
        if(sessions.slots[i].cookie){rc=IFS_FS_BUSY;break;}
    ifs_fs_ro_sessions_unlock_v2(&sessions);
    if(rc==IFS_FS_OK)rc=format_impl(h,label);
    atomic_flag_clear_explicit(&format_gate,memory_order_release);
    return rc;
}
#endif

/* Serialize mount publication against the hosted exclusive formatter.
 * Do not hold a registry spinlock while invoking a caller's I/O. */
static int mount(const IfsFsHostRuntimeV2 *h,void **volume,IfsFsVolumeInfoV2 *info)
{
#if !defined(IFS_SFS2_FIRMWARE_BUILD)
    if(atomic_flag_test_and_set_explicit(&format_gate,memory_order_acquire)) {
        if(volume)*volume=NULL;
        return IFS_FS_BUSY;
    }
#endif
    int rc=mount_impl(h,volume,info);
#if !defined(IFS_SFS2_FIRMWARE_BUILD)
    atomic_flag_clear_explicit(&format_gate,memory_order_release);
#endif
    return rc;
}

#if !defined(IFS_SFS2_FIRMWARE_BUILD)
/* Hosted extent inspection pins the same native reader as CORE.READ. */
static int map_path(void *volume,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    Sfs2Session *session=acquire_reader(volume);
    return session?finish_reader(session,ifs_sfs2_ro_map(&session->reader,path,
                       first,out,capacity,written)):IFS_FS_INVALID;
}
/* The read-only mount has already verified every bitmap checksum and
 * required metadata ownership. Retain its bitmap geometry in the pinned
 * session. Read only the relevant bitmap blocks per page; the host's
 * immutable media lease prevents a concurrent allocation change. */
static int allocation_page(const Sfs2Session *session,uint64_t first,
                          IfsFsAllocationRangeV2 *out,uint32_t capacity,uint32_t *written)
{
    const IfsSfs2Reader *r=&session->reader;
    const IfsFsHostRuntimeV2 *h=&r->host;
    const uint64_t limit=h->volume_bytes;
    const uint64_t fs_bytes=(uint64_t)r->total_blocks*r->block_bytes;
    if(first>limit || fs_bytes>limit)return IFS_FS_INVALID;
    if(first==limit)return IFS_FS_END;
    uint32_t bits=0u,groups=0u;
    if(ifs_sfs2_compute_bitmap_layout(r->block_bytes,r->total_blocks,&bits,&groups)!=0 ||
       !bits || !groups || (uint64_t)session->bitmap_base+groups>r->total_blocks)
        return IFS_FS_CORRUPT;
    uint8_t *raw=h->allocate(h->context,r->block_bytes);
    if(!raw)return IFS_FS_NO_MEMORY;
    IfsFsAllocationRangeV2 page[64]={{0}};
    uint32_t count=0u,loaded=UINT32_MAX,scanned=0u;
    uint64_t at=first;
    int rc=IFS_FS_OK;
    while(at<limit && scanned<65536u) {
        if(h->cancelled && h->cancelled(h->context)){rc=IFS_FS_BUSY;goto done;}
        uint32_t state;
        uint64_t end;
        if(at>=fs_bytes) {
            state=IFS_FS_SPACE_RESERVED;end=limit;
        } else {
            const uint64_t block=at/r->block_bytes;
            const uint32_t group=(uint32_t)(block/bits),bit=(uint32_t)(block%bits);
            if(group>=groups){rc=IFS_FS_CORRUPT;goto done;}
            if(group!=loaded) {
                const uint32_t number=session->bitmap_base+group;
                rc=ifs_fs_host_read_bytes_v2(h,(uint64_t)number*r->block_bytes,
                                             raw,r->block_bytes);
                if(rc!=IFS_FS_OK)goto done;
                if(!ifs_sfs2_validate_block_header(raw,r->block_bytes,number,
                                                   UINT32_C(0x42544d50))) {
                    rc=IFS_FS_CORRUPT;goto done;
                }
                loaded=group;
            }
            state=(raw[IFS_SFS2_BLOCK_HEADER_SIZE+(bit>>3u)]&
                   (uint8_t)(0x80u>>(bit&7u)))?IFS_FS_SPACE_FREE:IFS_FS_SPACE_ALLOCATED;
            end=(block+1u)*r->block_bytes;
        }
        if(count && page[count-1u].state==state)page[count-1u].length+=end-at;
        else {
            if(count==capacity)break;
            page[count++]=(IfsFsAllocationRangeV2){
                .physical_offset=at,.length=end-at,.state=state};
        }
        at=end;++scanned;
    }
    memcpy(out,page,count*sizeof(*page));*written=count;
done:
    h->release(h->context,raw);
    return rc;
}
static int allocation_map(void *volume,uint64_t first,IfsFsAllocationRangeV2 *out,
                          uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    if(!out||!capacity||capacity>64u)return IFS_FS_INVALID;
    Sfs2Session *session=acquire_reader(volume);
    return session?finish_reader(session,allocation_page(session,first,out,capacity,written)):
        IFS_FS_INVALID;
}
static const IfsFsAllocationOpsV2 allocation_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(allocation_ops),IFS_FS_GROUP_ALLOCATION,0u,
             IFS_FS_ALLOCATION_MAP},
    .map=allocation_map
};
static const IfsFsDefragOpsV2 defrag_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(defrag_ops),IFS_FS_GROUP_DEFRAG,0u,
             IFS_FS_DEFRAG_MAP},
    .map=map_path
};
#endif
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
/* The same lifetime pin as CORE protects the reader during host I/O. */
static int v2_label(void *volume,char *out,uint32_t capacity,uint32_t *bytes)
{
    if(!bytes)return IFS_FS_INVALID;
    *bytes=0u;
    Sfs2Session *session=acquire_reader(volume);
    return session?finish_reader(session,ifs_sfs2_ro_label(&session->reader,out,capacity,bytes)):IFS_FS_INVALID;
}
static const IfsFsVolumeOpsV2 volume = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
               IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET
#if !defined(IFS_SFS2_FIRMWARE_BUILD)
             |IFS_FS_VOLUME_FORMAT
#endif
    },
    .query = query,.label=v2_label
#if !defined(IFS_SFS2_FIRMWARE_BUILD)
    ,.format=format_volume
#endif
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
#if !defined(IFS_SFS2_FIRMWARE_BUILD)
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag_ops.header;
    if(id==IFS_FS_GROUP_ALLOCATION)return &allocation_ops.header;
#endif
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
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)
#if !defined(IFS_SFS2_FIRMWARE_BUILD)
        |IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION)
#endif
        ,
    group
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
