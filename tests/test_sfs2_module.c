/* SPDX-License-Identifier: GPL-3.0-or-later
 * Only disposable images are used. No external formatter or writable device.
 */
#include "ifs_sfs2_module_v1.h"
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
static int update(int fd,const unsigned char *data) {
    return pwrite(fd,data,NB*BS,0)==NB*BS ? 0:-1;
}
int main(void) {
    const IfsSfs2ModuleV1 *api=infiltrator_filesystem_sfs2_v1();
    CHECK(api && api->abi_version==1u && api->structure_bytes>=sizeof(*api));
    CHECK(!strcmp(api->format_id,"sfs2"));
    CHECK(api->capabilities==(IFS_SFS2_CAP_PROBE|IFS_SFS2_CAP_GEOMETRY));
    CHECK(api->probe && api->geometry && api->probe(NULL)!=0);
    char path[]="/tmp/sfs2-library-test-XXXXXX";
    int fd=mkstemp(path); CHECK(fd>=0);
    unsigned char data[NB*BS]={0};
    root(data,0u,4u);root(data,NB-1,5u);
    CHECK(update(fd,data)==0);
    IfsSfs2Geometry geo={0};
    CHECK(api->probe(path)==0 && api->geometry(path,&geo)==0);
    CHECK(geo.block_size==BS && geo.total_blocks==NB && geo.root_sequence==5);
    CHECK(geo.primary_root_valid && geo.backup_root_valid);
    /* Damaged primary: intact backup is accepted, without guessing SFS0. */
    data[4]^=0x40u; CHECK(update(fd,data)==0);
    CHECK(api->geometry(path,&geo)==0 && !geo.primary_root_valid && geo.backup_root_valid);
    data[(NB-1)*BS+4]^=0x40u; CHECK(update(fd,data)==0);
    CHECK(api->probe(path)!=0);
    /* Correct checksum but SFS0 identity must be refused. */
    memset(data,0,sizeof(data));root(data,0,4);root(data,NB-1,5);
    be32(data,0x53465300u);be32(data+4,checksum(data));
    be32(data+(NB-1)*BS,0x53465300u);
    be32(data+(NB-1)*BS+4,checksum(data+(NB-1)*BS));
    CHECK(update(fd,data)==0 && api->probe(path)!=0);
    /* Contradictory but individually checksummed redundant roots fail closed. */
    memset(data,0,sizeof(data));root(data,0,4);root(data,NB-1,5);
    be32(data+(NB-1)*BS+96,6u);
    be32(data+(NB-1)*BS+4,checksum(data+(NB-1)*BS));
    CHECK(update(fd,data)==0 && api->probe(path)!=0);
    CHECK(close(fd)==0);CHECK(unlink(path)==0);
    return 0;
}
