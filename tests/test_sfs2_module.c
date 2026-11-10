/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strict Common V2 probe on disposable SFS2 root images.
 */
#include <infiltratr/filesystem_module_v2.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"FAIL %s line %d\\n",#x,__LINE__);return 1;} } while(0)
enum { BS = 512, NB = 16 };
static void be16(unsigned char *p, unsigned n) {p[0]=(unsigned char)(n>>8); p[1]=(unsigned char)n;}
static void be32(unsigned char *p, uint32_t n) {
    p[0]=(unsigned char)(n>>24);p[1]=(unsigned char)(n>>16);
    p[2]=(unsigned char)(n>>8);p[3]=(unsigned char)n;
}
static void be64(unsigned char *p,uint64_t n) {
    be32(p,(uint32_t)(n>>32));be32(p+4,(uint32_t)n);
}
static uint32_t checksum(const unsigned char *block) {
    uint32_t sum=2u;
    for(unsigned i=0;i<BS;i+=4)
        if(i!=4)sum+=(uint32_t)block[i]<<24|(uint32_t)block[i+1]<<16|
                     (uint32_t)block[i+2]<<8|block[i+3];
    return 0u-sum;
}
static void root(unsigned char *data,unsigned block,unsigned sequence) {
    unsigned char *p=data+BS*block;
    be32(p,0x53465302u);be32(p+8,block);be16(p+12,4u);be16(p+14,sequence);
    be64(p+32,0u);be64(p+40,NB*BS-1u);
    be32(p+48,NB);be32(p+52,BS);
    be32(p+96,1u);be32(p+100,2u);be32(p+104,3u);
    be32(p+108,4u);be32(p+112,5u);
    be32(p+4,checksum(p));
}
static int host_read(void *ctx,uint64_t at,void *buf,uint32_t n) {
    int fd=*(int *)ctx;
    return at <= INT64_MAX && n <= (uint64_t)INT64_MAX-at &&
        pread(fd,buf,n,(off_t)at)==(ssize_t)n ? 0:-1;
}
static int update(int fd,const unsigned char *data) {
    return pwrite(fd,data,NB*BS,0)==NB*BS ? 0:-1;
}
static void *host_alloc(void *ctx,size_t n) {
    (void)ctx;return malloc(n);
}
static void host_release(void *ctx,void *p) {
    (void)ctx;free(p);
}
int main(void) {
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    const IfsFsCoreOpsV2 *core;
    IfsFsHostRuntimeV2 host={0};
    uint32_t variant=0;
    char path[]="/tmp/sfs2-strict-v2-XXXXXX";
    int fd=mkstemp(path);
    unsigned char data[NB*BS]={0};
    CHECK(fd>=0);
    CHECK(unlink(path)==0);
    CHECK(ifs_fs_api_v2_valid(api,"sfs2"));
    CHECK(api->group_mask==IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE));
    core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    CHECK(core && core->header.capabilities==IFS_FS_CORE_PROBE &&
          core->probe && !core->mount && !core->read && !core->write);
    host.abi_version=IFS_FS_HOST_ABI_V2;
    host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.volume_bytes=sizeof(data);
    host.logical_sector_bytes=BS;
    host.physical_sector_bytes=BS;
    host.maximum_transfer_bytes=BS;
    host.context=&fd;
    host.read_at=host_read;
    host.allocate=host_alloc;
    host.release=host_release;
    CHECK(ifs_fs_host_v2_valid(&host));
    root(data,0u,4u);root(data,NB-1,5u);
    CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==4u);
    data[4]^=0x40u; CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)==IFS_FS_OK);
    data[(NB-1)*BS+4]^=0x40u; CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)!=IFS_FS_OK);
    memset(data,0,sizeof(data));root(data,0,4);root(data,NB-1,5);
    be32(data,0x53465300u);be32(data+4,checksum(data));
    be32(data+(NB-1)*BS,0x53465300u);
    be32(data+(NB-1)*BS+4,checksum(data+(NB-1)*BS));
    CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)!=IFS_FS_OK);
    memset(data,0,sizeof(data));root(data,0,4);root(data,NB-1,5);
    be32(data+(NB-1)*BS+96,6u);
    be32(data+(NB-1)*BS+4,checksum(data+(NB-1)*BS));
    CHECK(update(fd,data)==0);
    CHECK(core->probe(&host,&variant)!=IFS_FS_OK);
    CHECK(close(fd)==0);
    return 0;
}
