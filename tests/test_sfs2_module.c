/* SPDX-License-Identifier: GPL-3.0-or-later
 * Disposable SFS2 images, no path-level I/O or legacy interface.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "../native/filesystems/sfs2/core/sfs2_core.h"
#include <fcntl.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s line %d\n", #x, __LINE__); return 1; } } while (0)
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
enum { BS = 512, NB = 16 };
static void be16(unsigned char *p,unsigned n)
{ p[0]=(unsigned char)(n>>8);p[1]=(unsigned char)n; }
static void be32(unsigned char *p,uint32_t n)
{ p[0]=(unsigned char)(n>>24);p[1]=(unsigned char)(n>>16);
  p[2]=(unsigned char)(n>>8);p[3]=(unsigned char)n; }
static void be64(unsigned char *p,uint64_t n)
{ be32(p,(uint32_t)(n>>32));be32(p+4,(uint32_t)n); }
static uint32_t checksum(const unsigned char *block)
{
    uint32_t sum=2u;
    unsigned i;
    for(i=0;i<BS;i+=4)
        if(i!=4)sum+=((uint32_t)block[i]<<24)|((uint32_t)block[i+1]<<16)|
                     ((uint32_t)block[i+2]<<8)|block[i+3];
    return 0u-sum;
}
static void root(unsigned char *data,unsigned block,unsigned sequence)
{
    unsigned char *p=data+BS*block;
    be32(p,0x53465302u);be32(p+8,block);be16(p+12,4u);be16(p+14,sequence);
    be64(p+32,0u);be64(p+40,NB*BS-1u);
    be32(p+48,NB);be32(p+52,BS);
    be32(p+96,1u);be32(p+100,2u);be32(p+104,3u);
    be32(p+108,4u);be32(p+112,5u);be32(p+4,checksum(p));
}
static void bitmap(unsigned char *data)
{
    unsigned char *p=data+BS;
    memset(p,0,BS);
    be32(p,UINT32_C(0x42544d50));be32(p+8u,1u);
    for(unsigned bit=0u;bit<NB;++bit)
        p[IFS_SFS2_BLOCK_HEADER_SIZE+(bit>>3u)] |=
            (unsigned char)(0x80u>>(bit&7u));
    const unsigned used[]={0u,1u,2u,3u,4u,5u,NB-1u};
    for(unsigned i=0u;i<sizeof(used)/sizeof(used[0]);++i) {
        unsigned bit=used[i];
        p[IFS_SFS2_BLOCK_HEADER_SIZE+(bit>>3u)] &=
            (unsigned char)~(0x80u>>(bit&7u));
    }
    be32(p+4u,checksum(p));
}
/* Real SFS\2 namespace fixture: two kernel-like files, nested directory,
 * checksummed object containers, and a genuine extent B-tree. Every byte
 * lives in the disposable 16-block image, never in a source volume. */
static void structured(unsigned char *disk,uint32_t block,uint32_t id)
{
    unsigned char *p=disk+BS*block;
    memset(p,0,BS);
    be32(p,id);be32(p+8u,block);
    be32(p+4u,checksum(p));
}
static uint32_t object(unsigned char *block,uint32_t off,uint32_t node,
                       uint32_t data,uint32_t auxiliary,uint16_t size_low,
                       uint8_t bits,const char *name)
{
    size_t n=strlen(name);
    be32(block+off+4u,node);
    be32(block+off+12u,data);
    be32(block+off+16u,auxiliary);
    be16(block+off+20u,size_low);
    block[off+26u]=bits;
    memcpy(block+off+27u,name,n+1u);
    block[off+27u+n+1u]=0; /* zero-length comment */
    return (uint32_t)((off+27u+n+2u+1u)&~1u);
}
static void namespace_fixture(unsigned char *disk)
{
    const unsigned used[]={6u,7u,8u,9u,11u,12u,10u,13u};
    memset(disk,0,NB*BS);
    root(disk,0u,4u);root(disk,NB-1u,5u);
    for(unsigned i=0u;i<2u;++i) {
        unsigned char *p=disk+(i?NB-1u:0u)*BS;
        be32(p+108u,6u); /* extent tree root */
        be32(p+112u,7u); /* object-node table */
        be32(p+4u,checksum(p));
    }
    bitmap(disk);
    unsigned char *bm=disk+BS;
    for(unsigned i=0u;i<sizeof(used)/sizeof(used[0]);++i) {
        const unsigned bit=used[i];
        bm[12u+(bit>>3u)]&=(unsigned char)~(0x80u>>(bit&7u));
    }
    be32(bm+4u,checksum(bm));
    structured(disk,2u,UINT32_C(0x41444d43)); /* ADMC */
    structured(disk,3u,UINT32_C(0x4f424a43)); /* root OBJC */
    unsigned char *p=disk+3u*BS;
    object(p,24u,1u,4u,8u,0u,128u,"VOLUME");
    be32(p+4u,checksum(p));
    structured(disk,4u,UINT32_C(0x48544142)); /* HTAB */
    structured(disk,5u,UINT32_C(0x54524f4b)); /* TROK */
    structured(disk,6u,UINT32_C(0x424e4443)); /* extent BNDC leaf */
    p=disk+6u*BS;
    be16(p+12u,3u);p[14u]=1u;p[15u]=16u;
    be32(p+16u,9u);be32(p+20u,13u);be32(p+28u,1u);
    be32(p+32u,12u);be32(p+44u,1u);
    be32(p+48u,13u);be32(p+56u,9u);be32(p+60u,1u);
    be32(p+4u,checksum(p));
    structured(disk,7u,UINT32_C(0x4e444320)); /* NDC */
    p=disk+7u*BS;be32(p+12u,1u);be32(p+16u,1u);
    be32(p+4u,checksum(p));
    structured(disk,8u,UINT32_C(0x4f424a43)); /* root first child container */
    p=disk+8u*BS;be32(p+12u,1u);be32(p+16u,10u);
    object(p,24u,3u,9u,0u,2u*BS,0u,"KERNEL.BIN");
    be32(p+4u,checksum(p));
    structured(disk,10u,UINT32_C(0x4f424a43)); /* linked second container */
    p=disk+10u*BS;be32(p+12u,1u);be32(p+20u,8u);
    object(p,24u,4u,0u,11u,0u,128u,"BOOT");
    be32(p+4u,checksum(p));
    structured(disk,11u,UINT32_C(0x4f424a43)); /* BOOT children */
    p=disk+11u*BS;be32(p+12u,4u);
    object(p,24u,5u,12u,0u,256u,0u,"INITRD.IMG");
    be32(p+4u,checksum(p));
    for(unsigned i=0u;i<BS;++i) {
        disk[9u*BS+i]=(unsigned char)(i*31u+7u);
        disk[13u*BS+i]=(unsigned char)((i+BS)*31u+7u);
    }
    for(unsigned i=0u;i<256u;++i)
        disk[12u*BS+i]=(unsigned char)(i^0xa5u);
}
static uint32_t sector_bytes=512u;
static int fail_reads;
static const IfsFsCoreOpsV2 *unmount_during_read;
static void *active_volume;
static int observed_unmount;
static uint64_t fail_at=UINT64_MAX;
static int rd(void *ctx,uint64_t at,void *dest,uint32_t n)
{
    if (unmount_during_read) {
        const IfsFsCoreOpsV2 *ops=unmount_during_read;
        unmount_during_read=NULL;
        observed_unmount=ops->unmount(active_volume);
        if (observed_unmount!=IFS_FS_BUSY) return -1;
    }
    if(fail_reads || at==fail_at || at%sector_bytes ||
       n%sector_bytes || n>sector_bytes)return -1;
    return pread(*(int *)ctx,dest,n,(off_t)at)==(ssize_t)n?0:-1;
}
static unsigned allocation_count, fail_allocation_number;
static void *allocate(void *p,size_t n)
{
    (void)p;
    if (fail_allocation_number &&
        ++allocation_count==fail_allocation_number) return NULL;
    return malloc(n);
}
static void release(void *p,void *x) { (void)p; free(x); }
static int update(int fd,const unsigned char *data)
{ return pwrite(fd,data,NB*BS,0)==NB*BS?0:-1; }
int main(int argc,char **argv)
{
    const IfsFsModuleApiV2 *api=NULL;
    void *freestanding=NULL;
    if (argc==2 || (argc==3 && strcmp(argv[2],"hosted")==0)) {
        freestanding=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
        CHECK(freestanding!=NULL);
        void *symbol=dlsym(freestanding,IFS_FS_COMMON_EXPORT_V2);
        IfsFsModuleApiEntryV2 resolve=NULL;
        CHECK(symbol && sizeof(resolve)==sizeof(symbol));
        memcpy(&resolve,&symbol,sizeof(resolve));
        api=resolve ? resolve() : NULL;
        CHECK(api!=NULL);
        CHECK(api->runtime_profiles==
              (argc==3 ? IFS_FS_PROFILE_HOSTED :
               (IFS_FS_PROFILE_FIRMWARE|IFS_FS_PROFILE_FREESTANDING)));
    } else {
        CHECK(argc==1);
        api=infiltrator_filesystem_api_v2();
        CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    }
    const IfsFsCoreOpsV2 *core;
    const IfsFsVolumeOpsV2 *ops;
    IfsFsHostRuntimeV2 host={0};
    IfsFsVolumeInfoV2 info={0};
    unsigned char data[NB*BS]={0};
    char path[]="/tmp/sfs2-v2-XXXXXX";
    uint32_t variant=0u;
    int fd=mkstemp(path);
    void *volume=NULL;
    CHECK(fd>=0);
    CHECK(ftruncate(fd, NB*BS)==0);
    CHECK(ifs_fs_api_v2_valid(api,"sfs2"));
    core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    ops=(const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(core && ops && core->probe && core->mount && core->unmount && ops->query);
    CHECK(!core->write && !core->create && !ops->format && !ops->repair);
    host.abi_version=IFS_FS_HOST_ABI_V2;
    host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.volume_bytes=NB*BS;
    host.logical_sector_bytes=BS;
    host.physical_sector_bytes=BS;
    host.maximum_transfer_bytes=BS;
    host.context=&fd;
    host.read_at=rd;
    host.allocate=allocate;
    host.release=release;
    CHECK(ifs_fs_host_v2_valid(&host));
    CHECK(core->probe(&host,&variant)==IFS_FS_NOT_FOUND);
    root(data,0u,4u);root(data,NB-1u,5u);
    bitmap(data);
    CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==BS);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_CORRUPT && !volume);
    namespace_fixture(data);
    CHECK(update(fd,data)==0);
    /* A failed read of the otherwise valid backup root must report I/O,
     * never NOT_FOUND or CORRUPT. Both hosted and freestanding builds
     * exercise this exact Common V2 PROBE/MOUNT path. */
    fail_at=(NB-1u)*BS;
    CHECK(core->probe(&host,&variant)==IFS_FS_IO_ERROR);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_IO_ERROR && !volume);
    fail_at=UINT64_MAX;
    /* The first allocation is the initial sector scratch buffer and the
     * second is the native root scratch buffer. Report a real allocation
     * failure instead of an absent filesystem; do not leak the first. */
    allocation_count=0u;
    fail_allocation_number=2u;
    CHECK(core->probe(&host,&variant)==IFS_FS_NO_MEMORY);
    allocation_count=0u;
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_NO_MEMORY && !volume);
    fail_allocation_number=0u;
    /* Valid BTMP checksums must not conceal allocation of live metadata
     * as free space. Check both roots, the bitmap, object and extent roots,
     * and the transaction marker through the actual Common V2 mount. */
    {
        const unsigned critical[]={0u,1u,3u,5u,6u,NB-1u};
        for(unsigned i=0u;i<sizeof(critical)/sizeof(critical[0]);++i) {
            const unsigned block=critical[i];
            unsigned char *bit=&data[BS+IFS_SFS2_BLOCK_HEADER_SIZE+
                                     (block>>3u)];
            const unsigned char mask=(unsigned char)(0x80u>>(block&7u));
            CHECK((*bit&mask)==0u);
            *bit|=mask;
            be32(data+BS+4u,checksum(data+BS));
            CHECK(update(fd,data)==0);
            info.structure_bytes=sizeof(info);
            CHECK(core->mount(&host,&volume,&info)==IFS_FS_CORRUPT &&
                  !volume);
            *bit&=(unsigned char)~mask;
            be32(data+BS+4u,checksum(data+BS));
            CHECK(update(fd,data)==0);
        }
    }
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_OK && volume);
    CHECK(info.block_bytes==BS && info.total_bytes==NB*BS &&
          info.free_bytes==1u*BS);
    info.structure_bytes=sizeof(info);
    CHECK(ops->query(volume,&info)==IFS_FS_OK && info.block_bytes==BS);
    {
        void *second=NULL;
        IfsFsVolumeInfoV2 other={.structure_bytes=sizeof(other)};
        CHECK(core->mount(&host,&second,&other)==IFS_FS_OK &&
              second && second!=volume);
        CHECK(other.free_bytes==info.free_bytes &&
              other.total_bytes==info.total_bytes);
        other.structure_bytes=sizeof(other);
        CHECK(ops->query(second,&other)==IFS_FS_OK);
        CHECK(core->unmount(second)==IFS_FS_OK);
        other.structure_bytes=sizeof(other);
        CHECK(ops->query(second,&other)==IFS_FS_INVALID);
        CHECK(core->unmount(second)==IFS_FS_INVALID);
    }
    {
        void *stale=volume;
        CHECK(core->unmount(volume)==IFS_FS_OK);
        info.structure_bytes=sizeof(info);
        CHECK(ops->query(stale,&info)==IFS_FS_INVALID);
        CHECK(core->unmount(stale)==IFS_FS_INVALID);
    }
    volume=NULL;
    data[BS+4u]^=1u; /* damaged bitmap checksum */
    CHECK(update(fd,data)==0);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_CORRUPT && !volume);
    data[BS+4u]^=1u;CHECK(update(fd,data)==0);
    /* 4Kn physical hosts must receive ONLY aligned full-sector reads;
     * the SFS2 root and backup remain 512-byte on-media blocks. */
    host.logical_sector_bytes=4096u;
    host.physical_sector_bytes=4096u;
    host.maximum_transfer_bytes=4096u;
    sector_bytes=4096u;
    CHECK(ifs_fs_host_v2_valid(&host));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==BS);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_OK && volume &&
          info.block_bytes==BS && info.total_bytes==NB*BS);
    CHECK(core->unmount(volume)==IFS_FS_OK);
    volume=NULL;
    host.logical_sector_bytes=BS;
    host.physical_sector_bytes=BS;
    host.maximum_transfer_bytes=BS;
    sector_bytes=BS;
    {
        unsigned char original[NB*BS],readback[NB*BS]={0};
        unsigned char bytes[2u*BS]={0};
        char name[256]={0};
        IfsFsEntryV2 entry={0};
        entry.structure_bytes=sizeof(entry);
        entry.name=name;entry.name_capacity=sizeof(name);
        memcpy(original,data,sizeof(original));
        namespace_fixture(data);
        CHECK(update(fd,data)==0);
        CHECK(core->probe(&host,&variant)==IFS_FS_OK);
        info.structure_bytes=sizeof(info);
        CHECK(core->mount(&host,&volume,&info)==IFS_FS_OK && volume);
        CHECK(core->stat && core->directory_entry && core->read);
        /* An existing file with an undersized caller buffer must return
         * NO_SPACE, never INVALID or a partially successful entry. */
        {
            char short_name[2] = {0};
            IfsFsEntryV2 small = {
                .structure_bytes=sizeof(small),
                .name=short_name,
                .name_capacity=sizeof(short_name)
            };
            CHECK(core->stat(volume,"/KERNEL.BIN",&small)==IFS_FS_NO_SPACE);
            CHECK(core->directory_entry(volume,"/",0u,&small)==IFS_FS_NO_SPACE);
            small.name_capacity=1u;
            CHECK(core->stat(volume,"/",&small)==IFS_FS_NO_SPACE);
        }
        CHECK(core->stat(volume,"/",&entry)==IFS_FS_OK &&
              entry.type==IFS_FS_ENTRY_DIRECTORY);
        /* Both Common V2 STAT and LIST refuse insufficient output space
         * without exposing even a prefix of the native object name. */
        {
            char tiny[4]={'X','X','X','X'};
            const char original_name[4]={'X','X','X','X'};
            IfsFsEntryV2 short_entry={.structure_bytes=sizeof(short_entry),
                                      .name=tiny,.name_capacity=sizeof(tiny)};
            CHECK(core->stat(volume,"/KERNEL.BIN",&short_entry)==IFS_FS_NO_SPACE);
            CHECK(memcmp(tiny,original_name,sizeof(tiny))==0 &&
                  short_entry.name_bytes==0u);
            CHECK(core->directory_entry(volume,"/",0u,&short_entry)==IFS_FS_NO_SPACE);
            CHECK(memcmp(tiny,original_name,sizeof(tiny))==0 &&
                  short_entry.name_bytes==0u);
            short_entry.name_capacity=1u;
            CHECK(core->stat(volume,"/",&short_entry)==IFS_FS_NO_SPACE);
            CHECK(tiny[0]=='X' && short_entry.name_bytes==0u);
        }
        CHECK(core->stat(volume,"/KERNEL.BIN",&entry)==IFS_FS_OK &&
              entry.type==IFS_FS_ENTRY_FILE && entry.size_bytes==2u*BS);
        CHECK(core->stat(volume,"/BOOT/INITRD.IMG",&entry)==IFS_FS_OK &&
              entry.size_bytes==256u);
        CHECK(core->stat(volume,"/MISSING",&entry)==IFS_FS_NOT_FOUND);
        CHECK(core->directory_entry(volume,"/",0u,&entry)==IFS_FS_OK &&
              strcmp(entry.name,"KERNEL.BIN")==0);
        CHECK(core->directory_entry(volume,"/",1u,&entry)==IFS_FS_OK &&
              strcmp(entry.name,"BOOT")==0);
        CHECK(core->directory_entry(volume,"/",2u,&entry)==IFS_FS_END);
        CHECK(core->directory_entry(volume,"/BOOT",0u,&entry)==IFS_FS_OK &&
              strcmp(entry.name,"INITRD.IMG")==0);
        CHECK(core->read(volume,"/KERNEL.BIN",0u,bytes,BS)==IFS_FS_OK);
        for(unsigned i=0u;i<BS;++i)
            CHECK(bytes[i]==(unsigned char)(i*31u+7u));
        CHECK(core->read(volume,"/BOOT/INITRD.IMG",0u,bytes,256u)==IFS_FS_OK);
        for(unsigned i=0u;i<256u;++i)
            CHECK(bytes[i]==(unsigned char)(i^0xa5u));
        CHECK(core->read(volume,"/KERNEL.BIN",17u,bytes,79u)==IFS_FS_OK);
        for(unsigned i=0u;i<79u;++i)
            CHECK(bytes[i]==(unsigned char)((i+17u)*31u+7u));
        /* The kernel file crosses two nonadjacent on-disk extents. */
        /* Reentrant host callback: a live read must pin its session
         * without holding the registry lock during host I/O. */
        active_volume=volume;
        unmount_during_read=core;
        observed_unmount=IFS_FS_INVALID;
        CHECK(core->read(volume,"/KERNEL.BIN",0u,bytes,2u*BS)==IFS_FS_OK);
        CHECK(!unmount_during_read && observed_unmount==IFS_FS_BUSY);
        for(unsigned i=0u;i<2u*BS;++i)
            CHECK(bytes[i]==(unsigned char)(i*31u+7u));
        CHECK(core->read(volume,"/KERNEL.BIN",500u,bytes,100u)==IFS_FS_OK);
        for(unsigned i=0u;i<100u;++i)
            CHECK(bytes[i]==(unsigned char)((i+500u)*31u+7u));
        CHECK(core->read(volume,"/MISSING",0u,bytes,1u)==IFS_FS_NOT_FOUND);
        CHECK(core->read(volume,"/KERNEL.BIN",2u*BS-1u,bytes,2u)==IFS_FS_INVALID);
        fail_reads=1;
        CHECK(core->read(volume,"/KERNEL.BIN",0u,bytes,1u)==IFS_FS_IO_ERROR);
        fail_reads=0;
        CHECK(pread(fd,readback,sizeof(readback),0)==(ssize_t)sizeof(readback));
        CHECK(memcmp(data,readback,sizeof(data))==0); /* strictly read-only */
        /* A separately checksummed later container must not provide a
         * second case-folded pathname identity accepted by V2 STAT/READ. */
        {
            unsigned char *later=data+10u*BS;
            memset(later+24u,0,BS-24u);
            object(later,24u,4u,0u,11u,0u,128u,"kernel.bin");
            be32(later+4u,checksum(later));
            CHECK(update(fd,data)==0);
            CHECK(core->stat(volume,"/KERNEL.BIN",&entry)==IFS_FS_CORRUPT);
            CHECK(core->read(volume,"/KERNEL.BIN",0u,bytes,1u)
                  ==IFS_FS_CORRUPT);
            memcpy(later,original+10u*BS,BS);
            CHECK(update(fd,data)==0);
            CHECK(core->stat(volume,"/BOOT",&entry)==IFS_FS_OK);
        }
        /* A CRC-valid linked-container backlink mismatch is corruption. */
        be32(data+10u*BS+20u,7u);
        be32(data+10u*BS+4u,checksum(data+10u*BS));
        CHECK(update(fd,data)==0);
        CHECK(core->directory_entry(volume,"/",1u,&entry)==IFS_FS_CORRUPT);
        be32(data+10u*BS+20u,8u);
        be32(data+10u*BS+4u,checksum(data+10u*BS));
        CHECK(update(fd,data)==0);
        data[6u*BS+4u]^=1u; /* checksummed extent tree damage */
        CHECK(update(fd,data)==0);
        CHECK(core->read(volume,"/KERNEL.BIN",0u,bytes,1u)==IFS_FS_CORRUPT);
        data[6u*BS+4u]^=1u;
        CHECK(update(fd,data)==0);
        /* A checksummed cycle must fail: checksum validation alone is
         * insufficient to qualify fragmented file chains. */
        be32(data+6u*BS+20u,9u);
        be32(data+6u*BS+4u,checksum(data+6u*BS));
        CHECK(update(fd,data)==0);
        CHECK(core->read(volume,"/KERNEL.BIN",0u,bytes,1u)==IFS_FS_CORRUPT);
        be32(data+6u*BS+20u,13u);
        be32(data+6u*BS+4u,checksum(data+6u*BS));
        CHECK(update(fd,data)==0);
        be32(data+5u*BS,UINT32_C(0x54524641)); /* pending transaction */
        be32(data+5u*BS+4u,checksum(data+5u*BS));
        CHECK(update(fd,data)==0);
        CHECK(core->stat(volume,"/KERNEL.BIN",&entry)==IFS_FS_UNSUPPORTED);
        be32(data+5u*BS,UINT32_C(0x54524f4b));
        be32(data+5u*BS+4u,checksum(data+5u*BS));
        CHECK(update(fd,data)==0);
        CHECK(core->read(volume,"/KERNEL.BIN",0u,bytes,16u)==IFS_FS_OK);
        CHECK(core->unmount(volume)==IFS_FS_OK);
        volume=NULL;
        CHECK(core->read(volume,"/KERNEL.BIN",0u,bytes,16u)==IFS_FS_INVALID);
        memcpy(data,original,sizeof(data));
        CHECK(update(fd,data)==0);
    }
    data[4]^=0x40u;
    CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==BS);
    data[(NB-1)*BS+4]^=0x40u;
    CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)!=IFS_FS_OK);
    memset(data,0,sizeof(data));root(data,0u,4u);root(data,NB-1u,5u);
    bitmap(data);
    be32(data,0x53465300u);be32(data+4,checksum(data));
    be32(data+(NB-1)*BS,0x53465300u);
    be32(data+(NB-1)*BS+4,checksum(data+(NB-1)*BS));
    CHECK(update(fd,data)==0 && core->probe(&host,&variant)!=IFS_FS_OK);
    memset(data,0,sizeof(data));root(data,0u,4u);root(data,NB-1u,5u);
    bitmap(data);
    be32(data+(NB-1)*BS+96,6u);
    be32(data+(NB-1)*BS+4,checksum(data+(NB-1)*BS));
    CHECK(update(fd,data)==0 && core->probe(&host,&variant)==IFS_FS_CORRUPT);
    CHECK(close(fd)==0 && unlink(path)==0);
    if(freestanding)CHECK(dlclose(freestanding)==0);
    return 0;
}
