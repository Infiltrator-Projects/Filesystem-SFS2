/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_SFS2_V2_READER_H
#define IFS_SFS2_V2_READER_H
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
typedef struct IfsSfs2Reader {
    IfsFsHostRuntimeV2 host;
    uint32_t block_bytes;
    uint32_t total_blocks;
    uint32_t object_root;
    uint32_t extent_root;
} IfsSfs2Reader;
int ifs_sfs2_ro_validate(const IfsSfs2Reader *);
int ifs_sfs2_ro_label(const IfsSfs2Reader *,char *,uint32_t,uint32_t *);
int ifs_sfs2_ro_stat(const IfsSfs2Reader *,const char *,IfsFsEntryV2 *);
int ifs_sfs2_ro_list(const IfsSfs2Reader *,const char *,uint32_t,IfsFsEntryV2 *);
int ifs_sfs2_ro_read(const IfsSfs2Reader *,const char *,uint64_t,void *,uint32_t);
#endif
