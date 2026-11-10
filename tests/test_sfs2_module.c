/* SPDX-License-Identifier: GPL-3.0-or-later
 * Disposable SFS2 images, no path-level I/O or legacy interface.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <fcntl.h>
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
static int rd(void *ctx,uint64_t at,void *dest,uint32_t n)
{ return pread(*(int *)ctx,dest,n,(off_t)at)==(ssize_t)n?0:-1; }
static void *allocate(void *p,size_t n) { (void)p; return malloc(n); }
static void release(void *p,void *x) { (void)p; free(x); }
static int update(int fd,const unsigned char *data)
{ return pwrite(fd,data,NB*BS,0)==NB*BS?0:-1; }
int main(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
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
    CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==BS);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&volume,&info)==IFS_FS_OK && volume);
    CHECK(info.block_bytes==BS && info.total_bytes==NB*BS);
    info.structure_bytes=sizeof(info);
    CHECK(ops->query(volume,&info)==IFS_FS_OK && info.block_bytes==BS);
    CHECK(core->unmount(volume)==IFS_FS_OK);
    volume=NULL;
    data[4]^=0x40u;
    CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==BS);
    data[(NB-1)*BS+4]^=0x40u;
    CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)!=IFS_FS_OK);
    memset(data,0,sizeof(data));root(data,0u,4u);root(data,NB-1u,5u);
    be32(data,0x53465300u);be32(data+4,checksum(data));
    be32(data+(NB-1)*BS,0x53465300u);
    be32(data+(NB-1)*BS+4,checksum(data+(NB-1)*BS));
    CHECK(update(fd,data)==0 && core->probe(&host,&variant)!=IFS_FS_OK);
    memset(data,0,sizeof(data));root(data,0u,4u);root(data,NB-1u,5u);
    be32(data+(NB-1)*BS+96,6u);
    be32(data+(NB-1)*BS+4,checksum(data+(NB-1)*BS));
    CHECK(update(fd,data)==0 && core->probe(&host,&variant)==IFS_FS_CORRUPT);
    CHECK(close(fd)==0 && unlink(path)==0);
    return 0;
}
