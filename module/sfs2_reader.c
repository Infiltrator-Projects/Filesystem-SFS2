/* SPDX-License-Identifier: GPL-3.0-or-later
 * SFS\2 native read-only namespace for Common V2.
 *
 * Object containers, B-tree extent leaves, file data and directory chains
 * are read from the source volume through checked, partition-relative host
 * callbacks. Both hosted and freestanding profiles compile this same engine.
 * No Linux VFS, FUSE, device filename or legacy filesystem API is involved.
 */
#include "sfs2_reader.h"
#include "../native/filesystems/sfs2/core/sfs2_core.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define OBJC UINT32_C(0x4f424a43)
#define BNDC UINT32_C(0x424e4443)
#define HTAB UINT32_C(0x48544142)
#define TROK UINT32_C(0x54524f4b)
#define TRFA UINT32_C(0x54524641)
#define TYPE_DIRECTORY UINT8_C(128)
#define TYPE_LINK UINT8_C(64)
#define TYPE_HARDLINK UINT8_C(32)
#define MAX_PATH_BYTES 4096u
#define MAX_PATH_DEPTH 64u
#define OBJECT_PREFIX IFS_SFS2_OBJECT_FIXED_SIZE
#define OBJECT_BLOCK_PREFIX 24u
#define BTREE_BLOCK_PREFIX 16u

typedef struct {
    uint32_t node;
    uint32_t first_extent;
    uint32_t child_container;
    uint64_t length;
    uint8_t bits;
    uint8_t raw_name[IFS_SFS2_MAX_FILENAME + 1u];
    uint32_t name_bytes;
} Object;

typedef struct {
    uint32_t key;
    uint32_t next;
    uint32_t prev;
    uint32_t blocks;
} Extent;

static uint16_t be16(const uint8_t *p)
{ return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static int has_geometry(const IfsSfs2Reader *v)
{
    return v && ifs_fs_host_v2_valid(&v->host) &&
           v->block_bytes >= 512u && v->block_bytes <= 65536u &&
           (v->block_bytes & (v->block_bytes - 1u)) == 0u &&
           v->total_blocks >= 4u &&
           (uint64_t)v->total_blocks * v->block_bytes <= v->host.volume_bytes &&
           v->object_root > 0u && v->object_root < v->total_blocks &&
           v->extent_root > 0u && v->extent_root < v->total_blocks;
}
static int read_bytes(const IfsSfs2Reader *v, uint64_t position,
                      void *data, uint32_t length)
{
    if (!has_geometry(v) || (!data && length) ||
        position > (uint64_t)v->total_blocks * v->block_bytes ||
        length > (uint64_t)v->total_blocks * v->block_bytes - position)
        return IFS_FS_CORRUPT;
    return ifs_fs_host_read_bytes_v2(&v->host,position,data,length);
}
static int block_read(const IfsSfs2Reader *v,uint32_t number,
                      uint32_t kind,uint8_t *output)
{
    if (number == 0u || number >= v->total_blocks)
        return IFS_FS_CORRUPT;
    int rc=read_bytes(v,(uint64_t)number*v->block_bytes,
                      output,v->block_bytes);
    if (rc != IFS_FS_OK) return rc;
    if (!ifs_sfs2_validate_block_header(output,v->block_bytes,
                                        number,kind))
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
/* The SFS transaction marker is root-object-block + 2. A recoverable
 * unfinished transaction is not the clean snapshot required by BOOT. */
static int transaction_clean(const IfsSfs2Reader *v)
{
    if (v->object_root > v->total_blocks - 3u)
        return IFS_FS_CORRUPT;
    uint8_t header[12];
    int rc=read_bytes(v,((uint64_t)v->object_root+2u)*v->block_bytes,
                      header,sizeof(header));
    if (rc != IFS_FS_OK) return rc;
    if (be32(header) == TRFA) return IFS_FS_UNSUPPORTED;
    if (be32(header) != TROK) return IFS_FS_CORRUPT;
    uint8_t *block=v->host.allocate(v->host.context,v->block_bytes);
    if (!block) return IFS_FS_NO_MEMORY;
    rc=block_read(v,v->object_root+2u,TROK,block);
    v->host.release(v->host.context,block);
    return rc;
}
/* Decode native Latin-1/Amiga byte names into UTF-8 for the Common V2
 * user-facing API, without changing disk names or accepting malformed data. */
static int object_name_utf8(const Object *o, char *output, uint32_t capacity,
                            uint32_t *written)
{
    uint32_t size=0;
    for (uint32_t i=0u;i<o->name_bytes;++i) {
        const uint8_t c=o->raw_name[i];
        uint32_t need=c<0x80u ? 1u : 2u;
        if (need > capacity || size >= capacity-need)
            return IFS_FS_INVALID;
        if (need==1u) output[size++]=(char)c;
        else {
            output[size++]=(char)(0xc0u|(c>>6));
            output[size++]=(char)(0x80u|(c&0x3fu));
        }
    }
    if (size >= capacity) return IFS_FS_INVALID;
    output[size]=0;
    if (written) *written=size;
    return IFS_FS_OK;
}
static int decode_object(const uint8_t *record,uint32_t left,
                         Object *object,uint32_t *consumed)
{
    if (!record || !object || !consumed ||
        left < OBJECT_PREFIX+2u)
        return IFS_FS_CORRUPT;
    uint32_t size=0,name=0;
    if (ifs_sfs2_object_record_layout(record+OBJECT_PREFIX,
            left-OBJECT_PREFIX,&size,&name)!=IFS_SFS2_OBJECT_RECORD_OK ||
        size>left || !name ||
        ifs_sfs2_validate_name(record+OBJECT_PREFIX,name)!=IFS_SFS2_NAME_OK)
        return IFS_FS_CORRUPT;
    memset(object,0,sizeof(*object));
    object->node=be32(record+4u);
    object->first_extent=be32(record+12u);
    object->child_container=be32(record+16u);
    object->length=ifs_sfs2_decode_file_size(object->child_container,
                                              be16(record+20u));
    object->bits=record[26u];
    if (!object->node || name>IFS_SFS2_MAX_FILENAME)
        return IFS_FS_CORRUPT;
    memcpy(object->raw_name,record+OBJECT_PREFIX,name);
    object->name_bytes=name;
    *consumed=size;
    return IFS_FS_OK;
}
static int component_equal(const Object *obj,const char *path,size_t length)
{
    size_t a=0u,b=0u;
    if (!length || length>MAX_PATH_BYTES) return 0;
    while (a<length && b<obj->name_bytes) {
        uint8_t decoded;
        const uint8_t c=(uint8_t)path[a++];
        if (c<0x80u) decoded=c;
        else if ((c==0xc2u || c==0xc3u) && a<length &&
                 ((uint8_t)path[a]&0xc0u)==0x80u)
            decoded=(uint8_t)(((uint32_t)(c&0x1fu)<<6)|
                              ((uint8_t)path[a++]&0x3fu));
        else return 0;
        if (ifs_sfs2_fold_character(decoded)!=
            ifs_sfs2_fold_character(obj->raw_name[b++]))
            return 0;
    }
    return a==length && b==obj->name_bytes;
}
/* The container's previous/next links form the authoritative directory
 * sequence. Each container has checksummed, self-identifying metadata. */
static int directory_find(const IfsSfs2Reader *v,uint32_t first,
                          uint32_t parent,uint32_t index,
                          const char *name,size_t name_len,Object *found)
{
    if (!found) return IFS_FS_INVALID;
    if (!first) return name ? IFS_FS_NOT_FOUND : IFS_FS_END;
    uint8_t *buffer=v->host.allocate(v->host.context,v->block_bytes);
    if (!buffer) return IFS_FS_NO_MEMORY;
    uint32_t previous=0u,seen=0u,iterations=0u;
    int status=name ? IFS_FS_NOT_FOUND : IFS_FS_END;
    while (first) {
        if (first>=v->total_blocks || ++iterations>v->total_blocks) {
            status=IFS_FS_CORRUPT; break;
        }
        status=block_read(v,first,OBJC,buffer);
        if (status!=IFS_FS_OK) break;
        if (be32(buffer+12u)!=parent || be32(buffer+20u)!=previous) {
            status=IFS_FS_CORRUPT; break;
        }
        const uint32_t next=be32(buffer+16u);
        if (next>=v->total_blocks || next==first) {
            status=IFS_FS_CORRUPT;break;
        }
        uint32_t at=OBJECT_BLOCK_PREFIX;
        status=name ? IFS_FS_NOT_FOUND : IFS_FS_END;
        while (at+OBJECT_PREFIX+2u<=v->block_bytes &&
               buffer[at+OBJECT_PREFIX]) {
            uint32_t consumed=0u;
            Object object;
            status=decode_object(buffer+at,v->block_bytes-at,
                                 &object,&consumed);
            if (status!=IFS_FS_OK) break;
            if ((name && component_equal(&object,name,name_len)) ||
                (!name && seen++==index)) {
                *found=object;
                status=IFS_FS_OK;
                goto done;
            }
            if (consumed>v->block_bytes-at || !consumed) {
                status=IFS_FS_CORRUPT;break;
            }
            at+=consumed;
            status=name ? IFS_FS_NOT_FOUND : IFS_FS_END;
        }
        if (status!=IFS_FS_END && status!=IFS_FS_NOT_FOUND) break;
        previous=first;
        first=next;
    }
done:
    v->host.release(v->host.context,buffer);
    return status;
}
static int root_object(const IfsSfs2Reader *v,Object *root)
{
    uint8_t *block=v->host.allocate(v->host.context,v->block_bytes);
    if (!block) return IFS_FS_NO_MEMORY;
    int rc=block_read(v,v->object_root,OBJC,block);
    if (rc==IFS_FS_OK) {
        uint32_t consumed=0;
        rc=decode_object(block+OBJECT_BLOCK_PREFIX,
                         v->block_bytes-OBJECT_BLOCK_PREFIX,
                         root,&consumed);
        if (rc==IFS_FS_OK &&
            (root->bits&TYPE_DIRECTORY)==0u)
            rc=IFS_FS_CORRUPT;
        if (rc==IFS_FS_OK && (root->node!=1u ||
              root->child_container>=v->total_blocks))
            rc=IFS_FS_CORRUPT;
    }
    v->host.release(v->host.context,block);
    return rc;
}
static int resolve(const IfsSfs2Reader *v,const char *path,Object *object)
{
    if (!has_geometry(v) || !path || !object || path[0]!='/')
        return IFS_FS_INVALID;
    size_t len=0u;
    while (len<=MAX_PATH_BYTES && path[len]) ++len;
    if (len>MAX_PATH_BYTES) return IFS_FS_INVALID;
    int rc=transaction_clean(v);
    if (rc!=IFS_FS_OK)return rc;
    rc=root_object(v,object);
    if (rc!=IFS_FS_OK)return rc;
    if (len==1u)return IFS_FS_OK;
    size_t start=1u;
    unsigned depth=0u;
    while(start<len) {
        if (++depth>MAX_PATH_DEPTH || !(object->bits&TYPE_DIRECTORY))
            return IFS_FS_UNSUPPORTED;
        if (object->bits&(TYPE_LINK|TYPE_HARDLINK))
            return IFS_FS_UNSUPPORTED;
        size_t end=start;
        while(end<len && path[end]!='/')++end;
        if (end==start || (end-start==1u && path[start]=='.') ||
            (end-start==2u && path[start]=='.' && path[start+1u]=='.'))
            return IFS_FS_INVALID;
        Object child;
        rc=directory_find(v,object->child_container,object->node,
                          0u,path+start,end-start,&child);
        if (rc!=IFS_FS_OK) return rc;
        *object=child;
        if (end==len) return IFS_FS_OK;
        start=end+1u;
        if (start==len) return IFS_FS_INVALID;
    }
    return IFS_FS_INVALID;
}
static int fill_entry(const Object *object,IfsFsEntryV2 *entry,
                      int root)
{
    if (!object || !entry || entry->structure_bytes<sizeof(*entry) ||
        !entry->name || !entry->name_capacity)
        return IFS_FS_INVALID;
    if (root) {
        if (entry->name_capacity<2u) return IFS_FS_INVALID;
        entry->name[0]='/'; entry->name[1]=0; entry->name_bytes=1u;
    } else {
        int rc=object_name_utf8(object,entry->name,entry->name_capacity,
                                &entry->name_bytes);
        if (rc!=IFS_FS_OK) return rc;
    }
    entry->type=(object->bits&TYPE_DIRECTORY) ?
        IFS_FS_ENTRY_DIRECTORY :
        (object->bits&(TYPE_LINK|TYPE_HARDLINK)) ?
        IFS_FS_ENTRY_LINK : IFS_FS_ENTRY_FILE;
    entry->size_bytes=(entry->type==IFS_FS_ENTRY_FILE)?object->length:0u;
    entry->attributes=0u;
    return IFS_FS_OK;
}
int ifs_sfs2_ro_stat(const IfsSfs2Reader *v,const char *path,
                     IfsFsEntryV2 *entry)
{
    Object object;
    int rc=resolve(v,path,&object);
    return rc==IFS_FS_OK ?
           fill_entry(&object,entry,path[1]==0) : rc;
}
int ifs_sfs2_ro_list(const IfsSfs2Reader *v,const char *path,
                     uint32_t index,IfsFsEntryV2 *entry)
{
    if (!entry) return IFS_FS_INVALID;
    Object directory,selected;
    int rc=resolve(v,path,&directory);
    if (rc!=IFS_FS_OK)return rc;
    if (!(directory.bits&TYPE_DIRECTORY))return IFS_FS_INVALID;
    if (directory.bits&(TYPE_LINK|TYPE_HARDLINK))return IFS_FS_UNSUPPORTED;
    rc=directory_find(v,directory.child_container,directory.node,
                      index,NULL,0u,&selected);
    return rc==IFS_FS_OK ? fill_entry(&selected,entry,0) : rc;
}
static int extent_lookup(const IfsSfs2Reader *v,uint32_t wanted,Extent *out)
{
    uint8_t *block=v->host.allocate(v->host.context,v->block_bytes);
    if (!block) return IFS_FS_NO_MEMORY;
    uint32_t number=v->extent_root;
    int status=IFS_FS_CORRUPT;
    for (uint32_t depth=0;depth<64u;++depth) {
        status=block_read(v,number,BNDC,block);
        if (status!=IFS_FS_OK)break;
        uint32_t count=be16(block+12u),leaf=block[14u]!=0u,
                 stride=block[15u];
        const uint32_t required=leaf?IFS_SFS2_EXTENT_NODE_SIZE:8u;
        if (stride!=required ||
            count>(v->block_bytes-BTREE_BLOCK_PREFIX)/required) {
            status=IFS_FS_CORRUPT;break;
        }
        uint32_t prior=0u,child=0u;
        int matching=0;
        for (uint32_t i=0;i<count;++i) {
            const uint8_t *p=block+BTREE_BLOCK_PREFIX+i*stride;
            const uint32_t key=be32(p);
            if (i>0u && key<=prior) {
                status=IFS_FS_CORRUPT;goto done;
            }
            prior=key;
            if (leaf && key==wanted) {
                *out=(Extent){key,be32(p+4u),be32(p+8u),be32(p+12u)};
                status=ifs_sfs2_validate_extent(out->key,out->next,
                            out->blocks,v->total_blocks)==0 &&
                       out->prev<v->total_blocks ?
                       IFS_FS_OK:IFS_FS_CORRUPT;
                goto done;
            }
            if (!leaf && key<=wanted) {
                child=be32(p+4u);
                matching=1;
            }
        }
        if (leaf || !matching || !child || child>=v->total_blocks) {
            status=IFS_FS_CORRUPT;break;
        }
        number=child;
    }
done:
    v->host.release(v->host.context,block);
    return status;
}
/* Mount is only successful when the clean transaction marker, root object,
 * directory container and extent root are genuine on-disk structures.
 * Root/bitmap signatures alone must never masquerade as a bootable volume. */
int ifs_sfs2_ro_validate(const IfsSfs2Reader *v)
{
    if(!has_geometry(v))return IFS_FS_INVALID;
    int rc=transaction_clean(v);
    if(rc!=IFS_FS_OK)return rc;
    Object root;
    rc=root_object(v,&root);
    if(rc!=IFS_FS_OK)return rc;
    uint8_t *buffer=v->host.allocate(v->host.context,v->block_bytes);
    if(!buffer)return IFS_FS_NO_MEMORY;
    if(root.first_extent) {
        rc=block_read(v,root.first_extent,HTAB,buffer);
        if(rc!=IFS_FS_OK)goto done;
    }
    rc=block_read(v,v->extent_root,BNDC,buffer);
    if(rc!=IFS_FS_OK)goto done;
    const uint32_t count=be16(buffer+12u);
    const uint32_t stride=buffer[14u]?16u:8u;
    if(buffer[15u]!=stride ||
       count>(v->block_bytes-BTREE_BLOCK_PREFIX)/stride) {
        rc=IFS_FS_CORRUPT;goto done;
    }
    if(root.child_container) {
        Object child;
        rc=directory_find(v,root.child_container,root.node,0u,NULL,0u,&child);
        if(rc==IFS_FS_END)rc=IFS_FS_OK;
    }
done:
    v->host.release(v->host.context,buffer);
    return rc;
}
/* Verify the complete native extent chain BEFORE copying user data.
 * A valid image must terminate at the right size and preserve prev links. */
static int check_file_extents(const IfsSfs2Reader *v,
                              const Object *file,uint64_t *blocks)
{
    if (!blocks) return IFS_FS_INVALID;
    *blocks=0u;
    if (!file->length)
        return file->first_extent ? IFS_FS_CORRUPT : IFS_FS_OK;
    if (!file->first_extent) return IFS_FS_CORRUPT;
    uint64_t coverage=0u;
    uint32_t pointer=file->first_extent,previous=0u;
    for (uint32_t i=0u;i<v->total_blocks && pointer;++i) {
        Extent e;
        int rc=extent_lookup(v,pointer,&e);
        if (rc!=IFS_FS_OK)return rc;
        if (e.prev!=previous ||
            (uint64_t)e.blocks>v->total_blocks-*blocks)
            return IFS_FS_CORRUPT;
        *blocks+=e.blocks;
        coverage+=(uint64_t)e.blocks*v->block_bytes;
        if (coverage >= file->length) {
            if (e.next || coverage-file->length>=v->block_bytes)
                return IFS_FS_CORRUPT;
            return IFS_FS_OK;
        }
        if (!e.next) return IFS_FS_CORRUPT;
        previous=e.key;
        pointer=e.next;
    }
    return IFS_FS_CORRUPT;
}
int ifs_sfs2_ro_read(const IfsSfs2Reader *v,const char *path,
                     uint64_t offset,void *output,uint32_t bytes)
{
    if (!output && bytes) return IFS_FS_INVALID;
    Object file;
    int rc=resolve(v,path,&file);
    if (rc!=IFS_FS_OK)return rc;
    if (file.bits&TYPE_DIRECTORY)return IFS_FS_INVALID;
    if (file.bits&(TYPE_LINK|TYPE_HARDLINK))return IFS_FS_UNSUPPORTED;
    if (offset>file.length || bytes>file.length-offset)
        return IFS_FS_INVALID;
    uint64_t blocks=0u;
    rc=check_file_extents(v,&file,&blocks);
    if (rc!=IFS_FS_OK || !bytes)return rc;
    uint32_t key=file.first_extent,previous=0u;
    uint64_t logical=0u;
    uint8_t *dest=output;
    uint32_t remaining=bytes;
    while(key && remaining) {
        Extent e;
        rc=extent_lookup(v,key,&e);
        if (rc!=IFS_FS_OK)return rc;
        if (e.prev!=previous)return IFS_FS_CORRUPT;
        const uint64_t extent_bytes=(uint64_t)e.blocks*v->block_bytes;
        if (offset<logical+extent_bytes) {
            const uint64_t within=offset>logical ? offset-logical : 0u;
            const uint64_t available=extent_bytes-within;
            const uint32_t count=remaining<available?remaining:(uint32_t)available;
            rc=read_bytes(v,(uint64_t)e.key*v->block_bytes+within,
                          dest,count);
            if (rc!=IFS_FS_OK)return rc;
            dest+=count;remaining-=count;offset+=count;
        }
        logical+=extent_bytes;
        previous=e.key;
        key=e.next;
    }
    return remaining ? IFS_FS_CORRUPT : IFS_FS_OK;
}
